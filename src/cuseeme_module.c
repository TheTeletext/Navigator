/* cuseeme_module.c - F7 "CU-SeeMe Live TV" tab: receive-only CU-SeeMe client.
 *
 * Lifecycle (locked brief):
 *   - tab created with NO connection (status "Disconnected")
 *   - on activate: start a worker thread that performs the OpenContinue
 *     handshake to cu-seeme.greektv.ca:7648 and receives media
 *   - on deactivate: signal the worker, send a CloseSocket (Message 6),
 *     tear the UDP socket + waveOut down, return to "Disconnected"
 *   - on shutdown: same clean teardown
 *   - on connection loss/timeout: retry with 5s..30s backoff
 *
 * Threading: the worker owns the socket, decoders and waveOut. It never
 * touches GDI/HWND directly - it converts each completed frame to an 8-bit
 * gray staging buffer under g_lock and PostMessages the render window to
 * repaint. The UI thread paints (DIB StretchDIBits + overlay) and, on a 1s
 * timer, formats the status strip from shared counters.
 *
 * Video is decoded at native 160x120 (the wire's DataType 1, "small").
 *
 * 2026-11-10, Navigator 1.1 (docs/2026-11-10_NAVIGATOR_WIN31_UX.md, 9):
 * the render window is a little System 7 desktop, drawn by cuseeme_mac.c.
 * The video sits in a Mac window at 4x (640x480) nearest-pixel, or 3x, 2x or
 * 1x when the tab is too small for 4x, always an integer scale. Under it,
 * Cornell's button bar (eye = freeze, speaker = mute, microphone drawn
 * crossed out because this client cannot send, a live level bar,
 * Transmission Statistics, Version and IP) and a status line with the
 * measured fps and Kbps. Beside it the Participant List, built from the
 * OpenContinues the reflector relays, and the Audio window. The strip
 * above and the status STATIC below keep the Windows 3.1 chrome and their
 * text is unchanged (test/verify_cuseeme_chop_2026-11-10.ps1 parses it).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "cuseeme_module.h"
#include "cuseeme_proto.h"
#include "cuseeme_video.h"
#include "cuseeme_deltamod.h"
#include "cuseeme_mulaw.h"
#include "cuseeme_idvi.h"
#include "suite_shell.h"
#include "suite_logo.h"   /* grayscale logo draw for the Start screen */
#include "suite_res.h"    /* IDR_LOGO_TELETEXT */
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "suite_w31.h"
#include "cuseeme_mac.h"
#include "cuseeme_macicons.h"

#define CUSM_RENDER_CLASS   "MGTCuSeeMeRender"
#define WM_APP_CUSM_FRAME   (WM_APP + 31)
#define CUSM_TIMER_STATUS   1
#define CUSM_TIMER_CONNECT  2      /* "Connecting..." dot animation */
#define CUSM_TIMER_METER    3      /* level bars, 10 Hz while connected */
#define CUSM_METER_MS       100
#define CUSM_STATUS_H       22
#define CUSM_STRIP_H        52     /* top connection strip (mirrors IRC/WAIS) */
#define CUSM_CONNECT_MS     300    /* dot-cycle interval (mirrors LiveTV) */
#define CUSM_CONNECT_FRAMES 4

/* Connection-strip control IDs (7300+). */
#define IDC_CUSM_REFL_EDIT       7301
#define IDC_CUSM_PORT_EDIT       7302
#define IDC_CUSM_AUDIO_COMBO     7303
#define IDC_CUSM_CONNECT_BTN     7304
#define IDC_CUSM_DISCONNECT_BTN  7305
#define IDC_CUSM_STATUS          7306

/* F7 UI lifecycle, mirroring the Modern Live TV tab (SPLASH/LOADING/PLAYING).
 * This gates WHEN the worker runs; it does not change the decode or 2x render. */
enum {
    CUSM_UI_SPLASH = 0,   /* Start screen: gray logo + caption + Start button */
    CUSM_UI_CONNECTING,   /* worker running, "Connecting" dots, awaiting 1st frame */
    CUSM_UI_LIVE          /* live 320x240 render + Stop button */
};

enum {
    ST_DISCONNECTED = 0,
    ST_CONNECTING,
    ST_HANDSHAKING,
    ST_RECEIVING,
    ST_LOST
};

/* waveOut buffer pool: audio arrives in ~200 ms chunks (1600 samples). */
#define CUSM_NWBUF      24
#define CUSM_WBUF_SAMP  8192   /* generous upper bound per packet */

/* ---- shared state (guarded by g_lock except where noted) ---- */
static CRITICAL_SECTION g_lock;
static int       g_lock_init = 0;
static int       g_state = ST_DISCONNECTED;
static uint8_t   g_gray[CUSM_VID_W * CUSM_VID_H];  /* latest frame, display gray */
static int       g_have_frame = 0;
static unsigned  g_frames_total = 0;     /* frame-end packets received */
static unsigned  g_drops_total = 0;      /* video frames completed while a real
                                            unified-0x10 stream gap occurred =
                                            incomplete/suspect frames (network loss) */
static unsigned  g_video_pkts = 0;
static unsigned  g_audio_bytes = 0;      /* compressed audio bytes (for kbps) */
static unsigned  g_audio_unknown = 0;    /* audio packets with unhandled format id */
static int       g_retry_secs = 5;
static unsigned  g_rx_bytes = 0;         /* every datagram byte received (Kbps) */
static unsigned  g_audio_pkts = 0;       /* audio packets decoded */
static int       g_level_peak = 0;       /* loudest sample sent to the speaker
                                            since the UI last looked (0..32767) */
static DWORD     g_session_t0 = 0;       /* tick of the first datagram this session */
static uint32_t  g_refl_ip = 0;          /* the reflector as resolved */
static uint32_t  g_my_ip = 0;            /* our address toward it */
static uint16_t  g_my_port = 0;          /* our bound UDP port */
static uint32_t  g_sender_ip = 0;        /* the sender we subscribe to */

/* The Participant List. The reflector relays each participant's
 * OpenContinue (ocp-SE): SrcAddr identifies it, 0x20 is its user name
 * (Pascal string), 0x34 its Send Mode, 0x36 its audio flags, 0x37 its
 * version. Verified live 2026-11-10 on cu-seeme.teletext.ca: one OC a
 * second from "Teletext TV" (Send Mode 1 = small video, flags 0x02 = sends
 * audio, version 1); our own OC is never echoed back, so Navigator is
 * added to the list by the UI as the lurker it is. An entry not heard
 * from for CUSM_ROSTER_AGE_MS, or that sends a Close, leaves the list. */
#define CUSM_ROSTER_MAX     16
#define CUSM_ROSTER_AGE_MS  20000
typedef struct {
    uint32_t ip;
    char     name[24];
    uint8_t  sendmode, flags, version;
    DWORD    seen;
} cusm_peer_t;
static cusm_peer_t g_roster[CUSM_ROSTER_MAX];
static int       g_roster_n = 0;

/* Listener controls: written by the UI thread, read by the worker. */
static volatile LONG g_muted  = 0;
static volatile LONG g_volume = 100;     /* percent */

/* ---- UI-thread state ---- */
static HWND      g_hContent = NULL;
static HWND      g_hRender  = NULL;
static HWND      g_hStatus  = NULL;
static int       g_controls_created = 0;
static int       g_class_reg = 0;
static HBITMAP   g_hDib = NULL;          /* 8-bit gray DIB section, 160x120 */
static void     *g_dibBits = NULL;
/* Gray BITMAPINFO (header + 256-entry identity palette) shared by the DIB
 * section and by StretchDIBits, so the frame stays in the 8-bit grayscale
 * domain end-to-end (COLORONCOLOR nearest-pixel scaling, no interpolation). */
static struct { BITMAPINFOHEADER h; RGBQUAD pal[256]; } g_dibInfo;

/* ---- Splash/Connecting/Live chrome state (UI thread only) ---- */
static int       g_ui_state = CUSM_UI_SPLASH;
static int       g_connect_frame = 0;         /* 0..3 dot count */
static RECT      g_caption_rect = {0, 0, 0, 0};
static HFONT     g_hCaptionFont = NULL;

/* ---- connection strip (WAIS/IRC-style) + committed session config ---- */
static HWND      g_hReflLbl, g_hReflEdit;
static HWND      g_hPortLbl, g_hPortEdit;
static HWND      g_hAudioLbl, g_hAudioCombo;
static HWND      g_hConnectBtn, g_hDisconnectBtn;   /* 2026-11-11: Disconnect
                                                       retired (one toggle); NULL */
static int       g_connected = 0;             /* UI session gate */
static char      g_reflector[256] = CUSEEME_HOST;   /* committed by Connect */
static int       g_port = CUSEEME_PORT;             /* reflector port (dest) */
static int       g_audio_codec = CUSM_AUDF_IDVI;    /* chosen decoder */

static const char *cusm_codec_name(int fmt) {
    return (fmt == CUSM_AUDF_DELTAMOD) ? "Delta-Mod" : "Intel DVI";
}

/* ---- worker thread control ---- */
static HANDLE    g_thread = NULL;
static HANDLE    g_stopEvent = NULL;     /* signalled to ask worker to exit */
static volatile LONG g_running = 0;

/* rate sampling (UI thread only) */
static unsigned  g_last_frames = 0;
static unsigned  g_last_abytes = 0;

/* ============================ waveOut ============================ */

static HWAVEOUT  g_wave = NULL;
static WAVEHDR   g_whdr[CUSM_NWBUF];
static short    *g_wbuf[CUSM_NWBUF];
static int       g_wbuf_init = 0;

static void waveout_open(void) {
    WAVEFORMATEX wf;
    int i;
    if (g_wave) return;
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = CUSM_AUDIO_RATE;   /* 8000 */
    wf.wBitsPerSample  = 16;
    wf.nBlockAlign     = (wf.wBitsPerSample / 8) * wf.nChannels;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    if (waveOutOpen(&g_wave, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        g_wave = NULL;
        return;
    }
    if (!g_wbuf_init) {
        for (i = 0; i < CUSM_NWBUF; i++)
            g_wbuf[i] = (short *)malloc(CUSM_WBUF_SAMP * sizeof(short));
        g_wbuf_init = 1;
    }
    memset(g_whdr, 0, sizeof(g_whdr));
}

static void waveout_close(void) {
    int i;
    if (!g_wave) return;
    waveOutReset(g_wave);
    for (i = 0; i < CUSM_NWBUF; i++) {
        if (g_whdr[i].dwFlags & WHDR_PREPARED)
            waveOutUnprepareHeader(g_wave, &g_whdr[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_wave);
    g_wave = NULL;
    memset(g_whdr, 0, sizeof(g_whdr));
}

/* Push decoded PCM to the soundcard. Worker thread only. */
static void waveout_push(const short *pcm, int nsamp) {
    int i;
    if (!g_wave || nsamp <= 0) return;
    if (nsamp > CUSM_WBUF_SAMP) nsamp = CUSM_WBUF_SAMP;
    for (i = 0; i < CUSM_NWBUF; i++) {
        WAVEHDR *h = &g_whdr[i];
        if (h->dwFlags == 0 || (h->dwFlags & WHDR_DONE)) {
            if (h->dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(g_wave, h, sizeof(WAVEHDR));
            memcpy(g_wbuf[i], pcm, nsamp * sizeof(short));
            memset(h, 0, sizeof(*h));
            h->lpData         = (LPSTR)g_wbuf[i];
            h->dwBufferLength = nsamp * sizeof(short);
            if (waveOutPrepareHeader(g_wave, h, sizeof(WAVEHDR)) == MMSYSERR_NOERROR)
                waveOutWrite(g_wave, h, sizeof(WAVEHDR));
            return;
        }
    }
    /* all buffers busy: drop this chunk (keeps real-time, avoids latency creep) */
}

/* ============================ worker ============================ */

static void set_state(int s) {
    EnterCriticalSection(&g_lock);
    g_state = s;
    LeaveCriticalSection(&g_lock);
}

/* Record a relayed OpenContinue in the Participant List. Worker thread. */
static void roster_note(const uint8_t *rb, int n, const cusm_packet_t *pk) {
    cusm_peer_t *p = NULL;
    int i, nl;
    if (n < CUSM_OC_OFF_CLIENTINFO || pk->src_addr == 0) return;
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_roster_n; i++)
        if (g_roster[i].ip == pk->src_addr) { p = &g_roster[i]; break; }
    if (pk->message == CUSM_MSG_CLOSE) {
        if (p) *p = g_roster[--g_roster_n];
        LeaveCriticalSection(&g_lock);
        return;
    }
    if (!p) {
        if (g_roster_n >= CUSM_ROSTER_MAX) { LeaveCriticalSection(&g_lock); return; }
        p = &g_roster[g_roster_n++];
        memset(p, 0, sizeof(*p));
        p->ip = pk->src_addr;
    }
    nl = rb[CUSM_OC_OFF_USERNAME];
    if (nl > 19) nl = 19;
    for (i = 0; i < nl; i++) {
        uint8_t c = rb[CUSM_OC_OFF_USERNAME + 1 + i];
        p->name[i] = (c >= 32 && c < 127) ? (char)c : '?';   /* Mac Roman high half */
    }
    p->name[nl] = '\0';
    p->sendmode = pk->oc_sendmode;
    p->flags    = pk->oc_flags;
    p->version  = rb[CUSM_OC_OFF_VERSION];
    p->seen     = GetTickCount();
    LeaveCriticalSection(&g_lock);
}

/* Discover the local IPv4 toward the reflector for the OpenContinue SrcAddr
 * identifier (ocp-SE: source address is a per-client unique id). */
static uint32_t local_ip_toward(struct sockaddr_in *dst) {
    uint32_t ip = 0;
    SOCKET t = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t != INVALID_SOCKET) {
        struct sockaddr_in loc;
        int ll = sizeof(loc);
        if (connect(t, (struct sockaddr *)dst, sizeof(*dst)) == 0 &&
            getsockname(t, (struct sockaddr *)&loc, &ll) == 0)
            ip = ntohl(loc.sin_addr.s_addr);
        closesocket(t);
    }
    return ip;
}

/* One connect/receive session. Returns when the session ends (timeout, error,
 * or stop requested). Returns 1 if stop was requested (caller should exit). */
static int session_run(void) {
    struct addrinfo hints, *res = NULL;
    struct sockaddr_in dst;
    SOCKET s = INVALID_SOCKET;
    uint32_t my_ip;
    uint16_t my_port = 0;        /* real local UDP port actually bound (see below);
                                    also written into the OpenContinue SrcPort. */
    uint8_t oc[CUSM_OC_MAX_LEN];
    uint32_t sender_ip = 0;      /* advertised addr of the broadcast video sender,
                                    learned from the reflector's relayed OpenContinues;
                                    0 until discovered. May be loopback/private -
                                    echoed verbatim into our subscribing ClientInfo. */
    int oclen, stop = 0;
    /* Monotonic OpenContinue sequence (seed from the ms clock). Not the reconnect
     * fix - the reflector keys its per-IP media slot on the real (IP, UDP port)
     * endpoint, not on this counter - but a non-decreasing seq is correct client
     * behaviour, so keep it. */
    uint32_t seq = (uint32_t)GetTickCount();
    DWORD last_keepalive, last_rx;
    cusm_video_t vid;
    cusm_deltamod_t dm;
    uint32_t last_seq = 0;       /* unified 0x10 seq of the last received packet */
    int      have_last_seq = 0;
    int      frame_lossy = 0;    /* real stream gap seen during the current frame */
    int      stabilized = 0;     /* set once the first full frame has displayed;
                                    drops before this are startup keyframe settle
                                    (we join mid-stream, so the first assembled
                                    frame is inherently partial) and not counted */

    set_state(ST_CONNECTING);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    {
        /* Reflector host:port come from the connection strip (committed into
         * g_reflector/g_port at Connect). The LOCAL bind below stays the
         * fixed client port 7648 (the reconnect-slot trick), independent of
         * the destination port. */
        char portstr[16];
        snprintf(portstr, sizeof(portstr), "%d", g_port);
        if (getaddrinfo(g_reflector, portstr, &hints, &res) != 0 || !res)
            return 0;  /* DNS failure -> caller backs off */
    }
    dst = *(struct sockaddr_in *)res->ai_addr;
    freeaddrinfo(res);

    s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return 0;
    {
        /* Bind a FIXED local UDP port (7648, the classic CU-SeeMe client port).
         * The reflector keys its per-IP media slot on the real (IP, UDP port)
         * endpoint and reaps it slowly on idle (~60-120 s). Reconnecting from the
         * SAME fixed local port reuses the same NAT-mapped endpoint, so a quick
         * Stop->Start lands on the still-live slot and resumes A/V immediately -
         * exactly what the real Cornell client does. Binding an OS-assigned
         * ephemeral port (the old behaviour) presents a NEW endpoint from an
         * already-occupied IP, which the reflector ignores until the old slot
         * reaps: the reconnect lockout. Proven against the live reflector: fixed
         * port resumes instantly, ephemeral stays silent back-to-back.
         * SO_REUSEADDR lets us re-bind 7648 immediately after the previous
         * socket closed; if 7648 is somehow busy (e.g. a 2nd instance) fall back
         * to ephemeral so media still works, only instant-reconnect is lost. */
        struct sockaddr_in local, bound;
        int bl = sizeof(bound);
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&yes, sizeof(yes));
        memset(&local, 0, sizeof(local));
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = INADDR_ANY;
        local.sin_port = htons(CUSEEME_PORT);   /* 7648 */
        if (bind(s, (struct sockaddr *)&local, sizeof(local)) != 0) {
            local.sin_port = 0;                 /* fallback: OS-assigned ephemeral */
            bind(s, (struct sockaddr *)&local, sizeof(local));
        }
        if (getsockname(s, (struct sockaddr *)&bound, &bl) == 0)
            my_port = ntohs(bound.sin_port);
    }
    my_ip = local_ip_toward(&dst);

    EnterCriticalSection(&g_lock);
    g_refl_ip   = ntohl(dst.sin_addr.s_addr);
    g_my_ip     = my_ip;
    g_my_port   = my_port;
    g_sender_ip = 0;
    g_roster_n  = 0;
    LeaveCriticalSection(&g_lock);

    cusm_video_init(&vid);
    cusm_deltamod_init(&dm);
    waveout_open();

    set_state(ST_HANDSHAKING);
    /* First OC: bare open (no sender known yet -> subscribe_ip 0). The reflector
     * registers us and starts relaying participant OpenContinues, from which we
     * learn the broadcast sender's advertised address (sender_ip). */
    oclen = cusm_build_opencontinue(oc, seq, my_ip, my_port, SUITE_APP_NAME,
                                    1, 0, 0);
    sendto(s, (char *)oc, oclen, 0, (struct sockaddr *)&dst, sizeof(dst));

    last_keepalive = GetTickCount();
    last_rx = GetTickCount();

    for (;;) {
        fd_set rf;
        struct timeval tv;
        int r;

        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) { stop = 1; break; }

        FD_ZERO(&rf);
        FD_SET(s, &rf);
        tv.tv_sec = 0;
        tv.tv_usec = 150000;
        r = select(0, &rf, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(s, &rf)) {
            uint8_t rb[4096];
            struct sockaddr_in from;
            int fl = sizeof(from);
            int n = recvfrom(s, (char *)rb, sizeof(rb), 0, (struct sockaddr *)&from, &fl);
            if (n > 0) {
                cusm_packet_t pk;
                last_rx = GetTickCount();
                EnterCriticalSection(&g_lock);
                g_rx_bytes += (unsigned)n;
                if (!g_session_t0) g_session_t0 = last_rx;
                LeaveCriticalSection(&g_lock);
                if (cusm_parse(rb, n, &pk)) {
                    /* Real packet-loss detection on the reflector's unified 0x10
                     * counter. Audio+video+control all share one strictly-
                     * increasing sequence, so it is contiguous unless the network
                     * actually drops a datagram; a forward jump > 1 therefore means
                     * >= 1 packet was genuinely lost. Remember that so the frame in
                     * flight is flagged incomplete at its VID_END. Reordered or
                     * duplicate packets (seq <= last) are ignored, not counted. */
                    if (have_last_seq) {
                        if (pk.seq > last_seq + 1) frame_lossy = 1;
                        if (pk.seq > last_seq)     last_seq = pk.seq;
                    } else {
                        last_seq = pk.seq;
                        have_last_seq = 1;
                    }
                    if (pk.data_type == CUSM_DT_VIDEO_SMALL) {
                        set_state(ST_RECEIVING);
                        cusm_video_decode(&vid, pk.vid_data, pk.vid_len);
                        EnterCriticalSection(&g_lock);
                        g_video_pkts++;
                        if (pk.message == CUSM_MSG_VID_END) {
                            cusm_video_to_gray8(&vid, g_gray);
                            g_have_frame = 1;
                            g_frames_total++;
                            if (!stabilized) {
                                /* First full frame decoded + displayed = settle
                                 * point. Any gap up to here is the startup
                                 * keyframe painting in from gray, not real loss,
                                 * so zero the counter and start counting honestly
                                 * from the next frame on. */
                                stabilized = 1;
                                g_drops_total = 0;
                            } else if (frame_lossy) {
                                /* frame completed while a real gap occurred -> suspect */
                                g_drops_total++;
                            }
                            frame_lossy = 0;
                        }
                        LeaveCriticalSection(&g_lock);
                        if (pk.message == CUSM_MSG_VID_END && g_hRender)
                            PostMessageA(g_hRender, WM_APP_CUSM_FRAME, 0, 0);
                    } else if (pk.data_type == CUSM_DT_AUDIO) {
                        /* Dispatch on the VAT-header format id (Audio-RBK):
                         *   30 -> IDVI (Intel DVI / IMA ADPCM) - the live wire
                         *   26 -> DELTAMOD (legacy, dormant)
                         *    0 -> G.711 mu-law (PCMU, dormant)
                         * All decode to 8 kHz mono 16-bit PCM and share the same
                         * waveOut playout path.
                         *
                         * As of the 2026-06-30 encoder fix the reflector emits
                         * VAT format 30 = IDVI: 204-byte blocks (4-byte seed
                         * header + 200 nibble bytes = 400 samples, timestamp
                         * +400). The Cornell-exact decode conventions live in
                         * cuseeme_idvi.[ch]. Unknown formats fall through to the
                         * unknown-format counter and are dropped (silent). */
                        /* The connection strip's "Audio Codec" selects the
                         * decoder (committed into g_audio_codec at Connect),
                         * overriding the wire format id: whatever the user
                         * picked is what runs on the audio payload. Default
                         * Intel DVI matches the reflector's current format 30
                         * emission; Delta-Mod engages the format-26 decoder. */
                        static short pcm[CUSM_WBUF_SAMP];
                        int ns;
                        if (g_audio_codec == CUSM_AUDF_DELTAMOD)
                            ns = cusm_deltamod_decode(&dm, pk.aud_data, pk.aud_len, pcm);
                        else
                            ns = cusm_idvi_decode(pk.aud_data, pk.aud_len, pcm);
                        if (ns > 0) {
                            /* Navigator 1.1 listener controls: the speaker
                             * button mutes (the chunk is not played), the
                             * Audio window's slider scales. The level bars
                             * show what reaches the speaker, so they fall
                             * to zero when muted. */
                            int k, peak = 0, vol = (int)g_volume;
                            if (!g_muted) {
                                if (vol < 100)
                                    for (k = 0; k < ns; k++)
                                        pcm[k] = (short)((pcm[k] * vol) / 100);
                                for (k = 0; k < ns; k++) {
                                    int a = pcm[k] < 0 ? -pcm[k] : pcm[k];
                                    if (a > peak) peak = a;
                                }
                                waveout_push(pcm, ns);
                            }
                            set_state(ST_RECEIVING);
                            EnterCriticalSection(&g_lock);
                            g_audio_bytes += (unsigned)pk.aud_len;
                            g_audio_pkts++;
                            if (peak > g_level_peak) g_level_peak = peak;
                            LeaveCriticalSection(&g_lock);
                        }
                    } else if (pk.data_type == CUSM_DT_OPENCONTINUE) {
                        /* Participant discovery: the reflector relays each
                         * sender's OpenContinue. A relayed OC whose Send Mode
                         * advertises video (SMALL/BIG) identifies the broadcast
                         * source ("GreekTV"); its advertised address is the
                         * packet SrcAddr. Record it so the next keepalive
                         * subscribes to it. We do NOT reject a loopback/private
                         * advertised address - on a reflector the media is
                         * relayed to us on this socket, not dialed at that addr.
                         * Our own/lurker OCs advertise Send Mode NONE and are
                         * ignored by this test. */
                        if ((pk.oc_sendmode == CUSM_SEND_SMALL ||
                             pk.oc_sendmode == CUSM_SEND_BIG) &&
                            pk.src_addr != 0)
                            sender_ip = pk.src_addr;
                        roster_note(rb, n, &pk);
                        EnterCriticalSection(&g_lock);
                        g_sender_ip = sender_ip;
                        LeaveCriticalSection(&g_lock);
                    }
                    /* DataType 105 welcome / 100 ack confirm liveness only. */
                }
            }
        }

        /* keepalive every 2 s (ocp-SE: Windows sends OpenContinue every 2 s) */
        if (GetTickCount() - last_keepalive >= 2000) {
            last_keepalive = GetTickCount();
            seq++;
            /* Once the broadcast sender is known, every keepalive carries the
             * subscribing ClientInfo (clientCount=1) so the reflector relays its
             * video+audio to us. Before discovery sender_ip is 0 -> bare OC. */
            oclen = cusm_build_opencontinue(oc, seq, my_ip, my_port,
                                            SUITE_APP_NAME, 1, 0, sender_ip);
            sendto(s, (char *)oc, oclen, 0, (struct sockaddr *)&dst, sizeof(dst));
        }
        /* connection lost: no media for 8 s */
        if (GetTickCount() - last_rx >= 8000) break;
    }

    /* Clean teardown: send Close (Message 6) on EVERY exit path (stop or timeout),
     * retransmitted twice since UDP is unreliable. Note the fixed-port bind above
     * is what actually guarantees instant reconnect (the reflector's per-IP slot,
     * keyed on the real (IP,port) endpoint, is reused); the Close is just polite
     * cleanup and does not by itself free the slot within the reap window. */
    seq++;
    oclen = cusm_build_opencontinue(oc, seq, my_ip, my_port, SUITE_APP_NAME,
                                    1, 1, 0);
    sendto(s, (char *)oc, oclen, 0, (struct sockaddr *)&dst, sizeof(dst));
    sendto(s, (char *)oc, oclen, 0, (struct sockaddr *)&dst, sizeof(dst));
    closesocket(s);
    waveout_close();
    return stop;
}

static DWORD WINAPI worker_proc(LPVOID arg) {
    (void)arg;
    while (WaitForSingleObject(g_stopEvent, 0) != WAIT_OBJECT_0) {
        int stop = session_run();
        if (stop) break;
        /* session ended without a stop request: connection lost -> backoff */
        EnterCriticalSection(&g_lock);
        g_state = ST_LOST;
        LeaveCriticalSection(&g_lock);
        /* interruptible backoff, 5s..30s */
        if (WaitForSingleObject(g_stopEvent, (DWORD)g_retry_secs * 1000) == WAIT_OBJECT_0)
            break;
        EnterCriticalSection(&g_lock);
        if (g_retry_secs < 30) g_retry_secs += 5;
        LeaveCriticalSection(&g_lock);
    }
    set_state(ST_DISCONNECTED);
    return 0;
}

static void worker_start(void) {
    if (g_running) return;
    if (!g_stopEvent) g_stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ResetEvent(g_stopEvent);
    EnterCriticalSection(&g_lock);
    g_retry_secs = 5;
    g_frames_total = g_drops_total = g_video_pkts = g_audio_bytes = 0;
    g_last_frames = g_last_abytes = 0;
    g_have_frame = 0;
    g_rx_bytes = g_audio_pkts = 0;
    g_level_peak = 0;
    g_session_t0 = 0;
    g_roster_n = 0;
    g_sender_ip = 0;
    LeaveCriticalSection(&g_lock);
    InterlockedExchange(&g_running, 1);
    g_thread = CreateThread(NULL, 0, worker_proc, NULL, 0, NULL);
}

static void worker_stop(void) {
    if (!g_running) return;
    if (g_stopEvent) SetEvent(g_stopEvent);
    if (g_thread) {
        WaitForSingleObject(g_thread, 4000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    InterlockedExchange(&g_running, 0);
    set_state(ST_DISCONNECTED);
}

/* ============================ rendering ============================ */

static void ensure_dib(void) {
    if (g_hDib) return;
    {
        HDC dc = GetDC(NULL);
        int i;
        memset(&g_dibInfo, 0, sizeof(g_dibInfo));
        g_dibInfo.h.biSize = sizeof(BITMAPINFOHEADER);
        g_dibInfo.h.biWidth = CUSM_VID_W;
        g_dibInfo.h.biHeight = -CUSM_VID_H;        /* top-down */
        g_dibInfo.h.biPlanes = 1;
        g_dibInfo.h.biBitCount = 8;
        g_dibInfo.h.biCompression = BI_RGB;
        g_dibInfo.h.biClrUsed = 256;
        for (i = 0; i < 256; i++) {
            g_dibInfo.pal[i].rgbRed = g_dibInfo.pal[i].rgbGreen =
                g_dibInfo.pal[i].rgbBlue = (BYTE)i;
        }
        g_hDib = CreateDIBSection(dc, (BITMAPINFO *)&g_dibInfo, DIB_RGB_COLORS,
                                  &g_dibBits, NULL, 0);
        if (dc) ReleaseDC(NULL, dc);
    }
}

/* ---- The System 7 desktop (Navigator 1.1, 2026-11-10) ----
 *
 * One paint draws everything from a snapshot of the shared state taken
 * under g_lock, and records where it put each clickable thing in g_lay so
 * the mouse handler hit-tests exactly what is on screen. */

typedef struct {
    int  scale;                         /* 4, 3, 2 or 1                    */
    int  meter_h;                       /* Audio meters: 100, or 76 when short */
    RECT vwin, video, bar, status;
    RECT b_eye, b_spk, b_mic, b_meter, b_stats, b_info;
    RECT plist, sect_hdr[3];
    int  has_row;                       /* the watched sender's row exists */
    RECT row_eye, row_spk;
    RECT audio, a_spk, a_slider, a_meter;
    RECT stats, stats_close, info, info_close;
} cusm_layout_t;

static cusm_layout_t g_lay;
static int       g_frozen = 0;          /* eye: hold the last frame        */
static int       g_sect_open[3] = { 1, 1, 1 };
static int       g_show_stats = 0, g_show_info = 0;
static int       g_meter = 0;           /* displayed level, 0..100         */
static int       g_drag_slider = 0;
static unsigned  g_fps10 = 0, g_kbps = 0;   /* measured, see update_status */

#define CUSM_PLIST_W   208
#define CUSM_AUDIO_W   150
/* Audio window height = 132 + meter height (100, or 76 in a short tab). */
#define CUSM_AUDIO_H(m) (132 + (m))
#define CUSM_INFO_W    228
#define CUSM_ROW_H     19
#define CUSM_PLIST_FOOT 18     /* "N participants" (refs 2, 4) */
#define CUSM_BTN       20     /* bevelled button-bar button (refs 5, 6) */
#define CUSM_BAR_H     24
#define CUSM_SLINE_H   16
#define CUSM_GAP       16
#define CUSM_STATS_H   (CMAC_TITLE_H + 6 + 10 * 14 + 6)
#define CUSM_INFOWIN_H (CMAC_TITLE_H + 6 + 7 * 14 + 6)

typedef struct {
    int      state, have, connected, ui;
    unsigned frames, drops, vpkts, apkts, rx, abytes;
    DWORD    t0;
    uint32_t refl_ip, my_ip, sender_ip;
    uint16_t my_port;
    int      n;
    cusm_peer_t peer[CUSM_ROSTER_MAX];
} cusm_snap_t;

static void ip_str(uint32_t ip, char *out, size_t cap) {
    snprintf(out, cap, "%u.%u.%u.%u", (ip >> 24) & 255, (ip >> 16) & 255,
             (ip >> 8) & 255, ip & 255);
}

static void take_snapshot(cusm_snap_t *s) {
    DWORD now = GetTickCount();
    int i;
    memset(s, 0, sizeof(*s));
    EnterCriticalSection(&g_lock);
    s->state = g_state; s->have = g_have_frame;
    s->frames = g_frames_total; s->drops = g_drops_total;
    s->vpkts = g_video_pkts; s->apkts = g_audio_pkts;
    s->rx = g_rx_bytes; s->abytes = g_audio_bytes; s->t0 = g_session_t0;
    s->refl_ip = g_refl_ip; s->my_ip = g_my_ip; s->my_port = g_my_port;
    s->sender_ip = g_sender_ip;
    /* A list from an ended session is not shown: when disconnected the
     * roster reads empty even though the table keeps its last entries. */
    for (i = 0; g_connected && g_state != ST_DISCONNECTED && i < g_roster_n; i++)
        if (now - g_roster[i].seen < CUSM_ROSTER_AGE_MS)
            s->peer[s->n++] = g_roster[i];
    /* the frame itself, unless the eye is shut */
    if (s->have && !g_frozen) {
        ensure_dib();
        if (g_dibBits) memcpy(g_dibBits, g_gray, CUSM_VID_W * CUSM_VID_H);
    }
    LeaveCriticalSection(&g_lock);
    s->connected = g_connected;
    s->ui = g_ui_state;
}

static const cusm_peer_t *watched_sender(const cusm_snap_t *s) {
    int i;
    for (i = 0; i < s->n; i++)
        if (s->peer[i].sendmode != CUSM_SEND_NONE && s->peer[i].ip == s->sender_ip)
            return &s->peer[i];
    return NULL;
}

/* Counts per section: 0 visible, 1 hidden, 2 lurkers (Navigator included
 * while connected). */
static void section_counts(const cusm_snap_t *s, int cnt[3]) {
    int i;
    cnt[0] = cnt[1] = cnt[2] = 0;
    for (i = 0; i < s->n; i++) {
        if (s->peer[i].sendmode == CUSM_SEND_NONE) cnt[2]++;
        else if (s->peer[i].ip == s->sender_ip)   cnt[0]++;
        else                                      cnt[1]++;
    }
    if (s->connected) cnt[2]++;
}

static int video_win_h(int sc) {
    return CMAC_TITLE_H + CUSM_VID_H * sc + 1 + CUSM_BAR_H + 1 + CUSM_SLINE_H + 1;
}

static void compute_layout(int W, int H, const cusm_snap_t *s, cusm_layout_t *L) {
    int sc, vw, vh, gx, gy, total, full, cnt[3], rows, x, y, ph, colh, x3;
    int c3 = 0, c3w = CUSM_INFO_W;
    /* 2026-11-11 operator spec: 4x native (160x120 -> 640x480) when it
     * fits, else the largest integer scale that does. At the 1024x768
     * minimum (2026-11-12) the render window is 1008x545 and 4x needs 566
     * px of height, so the minimum shows 3x; 2x and 1x are reached only on
     * a work area smaller than the minimum. */
    for (sc = 4; sc > 1; sc--) {
        vw = CUSM_VID_W * sc + 2;
        if (W >= vw + CUSM_GAP + CUSM_PLIST_W + 2 * 12 && H >= video_win_h(sc) + 2 * 12)
            break;
    }
    memset(L, 0, sizeof(*L));
    L->scale = sc;
    vw = CUSM_VID_W * sc + 2;
    vh = video_win_h(sc);

    section_counts(s, cnt);
    rows = 3;
    for (x = 0; x < 3; x++) if (g_sect_open[x]) rows += cnt[x];
    ph = CMAC_TITLE_H + 4 + rows * CUSM_ROW_H + 5 + CUSM_PLIST_FOOT;
    /* A short tab takes 24 px off the Audio meters so the second column
     * is not clipped at the bottom (800x600 left 377 px; the 1024x768
     * minimum leaves 545, so now only a long roster reaches this). */
    L->meter_h = 100;
    if (12 + ph + 14 + CUSM_AUDIO_H(100) + 4 > H) L->meter_h = 76;
    colh = ph + 14 + CUSM_AUDIO_H(L->meter_h);
    if (colh < vh) colh = vh;

    /* Statistics and Version/IP get their own third column so they never
     * cover the video or the Participant List (operator spec 2026-11-11:
     * they open on connect). The group is centred WITH that column, so
     * opening or closing them does not move the other windows. When the
     * full column does not fit, the group moves left and the column takes
     * what is left (at least 190 px); below that they stack over the
     * Audio window, which is the only thing they may cover. */
    total = vw + CUSM_GAP + CUSM_PLIST_W;
    full  = total + CUSM_GAP + CUSM_INFO_W;
    if (W >= full + 2 * 12) {
        gx = (W - full) / 2; c3 = 1;
    } else if (W - 8 - (12 + total + CUSM_GAP) >= 190) {
        gx = 12; c3 = 1; c3w = W - 8 - (12 + total + CUSM_GAP);
    } else {
        gx = (W - total) / 2; if (gx < 12) gx = 12;
    }
    if (c3 && colh < CUSM_STATS_H + 14 + CUSM_INFOWIN_H) colh = CUSM_STATS_H + 14 + CUSM_INFOWIN_H;
    gy = (H - colh) / 2;  if (gy < 12) gy = 12;

    /* the video window */
    L->vwin.left = gx; L->vwin.top = gy; L->vwin.right = gx + vw; L->vwin.bottom = gy + vh;
    L->video.left = gx + 1; L->video.top = gy + CMAC_TITLE_H;
    L->video.right = L->video.left + CUSM_VID_W * sc;
    L->video.bottom = L->video.top + CUSM_VID_H * sc;
    L->bar.left = gx + 1; L->bar.right = gx + vw - 1;
    L->bar.top = L->video.bottom + 1; L->bar.bottom = L->bar.top + CUSM_BAR_H;
    L->status.left = gx + 1; L->status.right = gx + vw - 1;
    L->status.top = L->bar.bottom + 1; L->status.bottom = L->status.top + CUSM_SLINE_H;
    y = L->bar.top + 2;
    x = L->bar.left + 6;
    SetRect(&L->b_eye,   x, y, x + CUSM_BTN, y + CUSM_BTN); x += 24;
    SetRect(&L->b_spk,   x, y, x + CUSM_BTN, y + CUSM_BTN); x += 24;
    SetRect(&L->b_mic,   x, y, x + CUSM_BTN, y + CUSM_BTN); x += 24;
    SetRect(&L->b_meter, x, L->bar.top + 3, x + 6, L->bar.bottom - 3);
    x = L->bar.right - 6 - CUSM_BTN;
    SetRect(&L->b_info,  x, y, x + CUSM_BTN, y + CUSM_BTN); x -= 24;
    SetRect(&L->b_stats, x, y, x + CUSM_BTN, y + CUSM_BTN);

    /* the Participant List and the Audio window, beside it */
    x = L->vwin.right + CUSM_GAP;
    SetRect(&L->plist, x, gy, x + CUSM_PLIST_W, gy + ph);
    SetRect(&L->audio, x, L->plist.bottom + 14, x + CUSM_AUDIO_W,
            L->plist.bottom + 14 + CUSM_AUDIO_H(L->meter_h));

    x3 = L->plist.right + CUSM_GAP;
    if (c3) {
        SetRect(&L->stats, x3, gy, x3 + c3w, gy + CUSM_STATS_H);
        y = g_show_stats ? L->stats.bottom + 14 : gy;
        SetRect(&L->info, x3, y, x3 + c3w, y + CUSM_INFOWIN_H);
    } else {
        x3 = L->plist.left;
        c3w = CUSM_INFO_W;
        if (x3 + c3w > W - 4) c3w = W - 4 - x3;
        y = L->audio.top;
        SetRect(&L->stats, x3, y, x3 + c3w, y + CUSM_STATS_H);
        y = g_show_stats ? y + 28 : y;          /* cascaded, Mac style */
        SetRect(&L->info, x3, y, x3 + c3w, y + CUSM_INFOWIN_H);
    }
}

/* The idle picture (2026-11-11): the RCA Indian Head test pattern at its
 * full 1024x768 (operator decision: keep the card at full resolution), a
 * 4 bpp gray BITMAP resource (IDB_CUSM_TESTPATTERN, from
 * tools/make_cusm_icons.py). The video area is smaller (640x480 at 4x), so
 * it is scaled DOWN with HALFTONE (area averaging, no dropped lines) once
 * per video size and cached; each paint is then one BitBlt. It replaced
 * the gray Teletext mark and "Live CU-SeeMe TV" / "Connecting..." caption
 * that stood in the pane until now. */
static HBITMAP g_tp_src = NULL;      /* the resource, as a DIB section   */
static HBITMAP g_tp_scaled = NULL;   /* cached at the current video size */
static int     g_tp_w = 0, g_tp_h = 0;

static void draw_test_pattern(HDC mem, const RECT *v) {
    int w = v->right - v->left, h = v->bottom - v->top;
    HDC sdc, ddc;
    HGDIOBJ os, od;
    BITMAP bm;
    if (!g_tp_src)
        g_tp_src = (HBITMAP)LoadImageA(GetModuleHandleA(NULL),
                       MAKEINTRESOURCEA(IDB_CUSM_TESTPATTERN), IMAGE_BITMAP,
                       0, 0, LR_CREATEDIBSECTION);
    if (!g_tp_src || w <= 0 || h <= 0) {
        HBRUSH blk = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(mem, v, blk);
        DeleteObject(blk);
        return;
    }
    if (!g_tp_scaled || g_tp_w != w || g_tp_h != h) {
        if (g_tp_scaled) DeleteObject(g_tp_scaled);
        g_tp_scaled = CreateCompatibleBitmap(mem, w, h);
        GetObjectA(g_tp_src, sizeof(bm), &bm);
        sdc = CreateCompatibleDC(mem);
        ddc = CreateCompatibleDC(mem);
        os = SelectObject(sdc, g_tp_src);
        od = SelectObject(ddc, g_tp_scaled);
        SetStretchBltMode(ddc, HALFTONE);
        SetBrushOrgEx(ddc, 0, 0, NULL);     /* required after HALFTONE */
        StretchBlt(ddc, 0, 0, w, h, sdc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
        SelectObject(sdc, os);
        SelectObject(ddc, od);
        DeleteDC(sdc);
        DeleteDC(ddc);
        g_tp_w = w; g_tp_h = h;
    }
    sdc = CreateCompatibleDC(mem);
    os = SelectObject(sdc, g_tp_scaled);
    BitBlt(mem, v->left, v->top, w, h, sdc, 0, 0, SRCCOPY);
    SelectObject(sdc, os);
    DeleteDC(sdc);
}

static void fill_rc(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc;
    HBRUSH br = CreateSolidBrush(c);
    SetRect(&rc, l, t, r, b);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void paint_video_window(HDC mem, const cusm_snap_t *s, const cusm_layout_t *L) {
    const cusm_peer_t *snd = watched_sender(s);
    const char *title = (s->ui == CUSM_UI_LIVE && snd && snd->name[0]) ? snd->name : "CU-SeeMe";
    RECT r;
    char left[48], right[32];
    const char *mid = NULL;

    cmac_window(mem, &L->vwin, title, 1, 0, CMAC_WHITE, NULL, NULL);

    /* The picture. A live frame while receiving, or the held frame while
     * the eye is shut (the pattern never covers either), at an integer
     * nearest-pixel scale (COLORONCOLOR replicates pixels, never
     * interpolates) so its squares stay chunky. Otherwise, idle before any
     * connect, after Disconnect, while waiting for the first frame and on
     * TIMED OUT, the full-resolution test pattern filling the same area. */
    if (s->ui == CUSM_UI_LIVE && s->have && g_dibBits &&
        (g_frozen || s->state == ST_RECEIVING)) {
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchDIBits(mem, L->video.left, L->video.top,
                      CUSM_VID_W * L->scale, CUSM_VID_H * L->scale,
                      0, 0, CUSM_VID_W, CUSM_VID_H,
                      g_dibBits, (BITMAPINFO *)&g_dibInfo, DIB_RGB_COLORS, SRCCOPY);
    } else {
        draw_test_pattern(mem, &L->video);
    }

    /* The button bar (refs 5, 6): every icon sits in a raised square
     * button; the level bar is one solid red bar; Statistics and
     * Version/IP are document icons at the right. */
    fill_rc(mem, L->bar.left, L->bar.top - 1, L->bar.right, L->bar.top, CMAC_BLACK);
    fill_rc(mem, L->bar.left, L->bar.top, L->bar.right, L->bar.bottom, CMAC_LTGRAY);
    cmac_bevel(mem, &L->b_eye, 1);
    cmac_glyph(mem, L->b_eye.left + 2, L->b_eye.top + 2, g_frozen ? CUSM_G_EYE_SHUT : CUSM_G_EYE);
    cmac_bevel(mem, &L->b_spk, 1);
    cmac_glyph(mem, L->b_spk.left + 2, L->b_spk.top + 2, g_muted ? CUSM_G_SPEAKER_OFF : CUSM_G_SPEAKER);
    cmac_bevel(mem, &L->b_mic, 1);
    cmac_glyph(mem, L->b_mic.left + 2, L->b_mic.top + 2, CUSM_G_MIC_X);   /* cannot transmit */
    cmac_meter_v(mem, &L->b_meter, g_meter, RGB(255, 0, 0), 1);
    cmac_bevel(mem, &L->b_stats, 1);
    cmac_glyph(mem, L->b_stats.left + 2, L->b_stats.top + 2, CUSM_G_STATS);
    cmac_bevel(mem, &L->b_info, 1);
    cmac_glyph(mem, L->b_info.left + 2, L->b_info.top + 2, CUSM_G_INFO);

    /* The status line (refs 5, 6): gray, fps centred in the left half and
     * Kbps centred in the right half. */
    fill_rc(mem, L->status.left, L->status.top - 1, L->status.right, L->status.top, CMAC_BLACK);
    fill_rc(mem, L->status.left, L->status.top, L->status.right, L->status.bottom, CMAC_LTGRAY);
    left[0] = right[0] = '\0';
    if (!s->connected || s->state == ST_DISCONNECTED) mid = "DISCONNECTED";
    else if (s->state == ST_LOST)                    mid = "TIMED OUT";
    else if (s->ui != CUSM_UI_LIVE || s->state != ST_RECEIVING) mid = "WAITING";
    else {
        snprintf(left, sizeof(left), "%u.%u fps", g_fps10 / 10, g_fps10 % 10);
        snprintf(right, sizeof(right), "%u Kbps", g_kbps);
        if (g_frozen) mid = "PAUSED";
    }
    {
        int half = (L->status.left + L->status.right) / 2;
        r = L->status; r.right = half;
        if (left[0])  cmac_text_rc(mem, &r, left, cmac_font_small(), CMAC_BLACK, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        r = L->status; r.left = half;
        if (right[0]) cmac_text_rc(mem, &r, right, cmac_font_small(), CMAC_BLACK, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        r = L->status;
        if (mid)      cmac_text_rc(mem, &r, mid, cmac_font_small(), CMAC_BLACK, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

/* One Participant List row (refs 2, 4): eye, speaker and microphone each in
 * a small raised box, a thin level bar, then the name. */
static void paint_row(HDC mem, int x, int y, int ge, int gs, int gm, int level,
                      const char *name) {
    RECT r, m;
    int i, g[3];
    g[0] = ge; g[1] = gs; g[2] = gm;
    for (i = 0; i < 3; i++) {
        SetRect(&r, x + i * 19, y, x + i * 19 + 18, y + 18);
        cmac_bevel(mem, &r, 1);
        cmac_glyph(mem, r.left + 1, r.top + 1, g[i]);
    }
    SetRect(&m, x + 59, y + 1, x + 64, y + 17);
    cmac_meter_v(mem, &m, level, RGB(255, 0, 0), 1);
    SetRect(&r, x + 70, y, x + 70 + 120, y + 18);
    cmac_text_rc(mem, &r, name, cmac_font_small(), CMAC_BLACK,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_participants(HDC mem, const cusm_snap_t *s, cusm_layout_t *L) {
    static const char *names[3] = { "Visible Users", "Hidden Users", "Lurkers" };
    RECT cl, r;
    int cnt[3], sec, i, y, x;
    char buf[48];

    cmac_window(mem, &L->plist, "Participant List", 0, 0, CMAC_LTGRAY, &cl, NULL);
    section_counts(s, cnt);
    y = cl.top + 4;
    for (sec = 0; sec < 3; sec++) {
        SetRect(&L->sect_hdr[sec], cl.left, y, cl.right, y + CUSM_ROW_H);
        cmac_glyph(mem, cl.left + 2, y + 1, g_sect_open[sec] ? CUSM_G_TRI_DOWN : CUSM_G_TRI_RIGHT);
        snprintf(buf, sizeof(buf), "%s (%d)", names[sec], cnt[sec]);
        SetRect(&r, cl.left + 20, y, cl.right - 4, y + 18);
        cmac_text_rc(mem, &r, buf, cmac_font_title(), CMAC_BLACK, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += CUSM_ROW_H;
        if (!g_sect_open[sec]) continue;
        x = cl.left + 8;
        for (i = 0; i < s->n; i++) {
            const cusm_peer_t *p = &s->peer[i];
            int sends_audio = (p->flags & CUSM_FLAG_SEND_AUDIO) != 0;
            int in_sec = (p->sendmode == CUSM_SEND_NONE) ? 2 : (p->ip == s->sender_ip ? 0 : 1);
            if (in_sec != sec) continue;
            if (sec == 0) {
                /* the sender we watch: eye and speaker are live controls,
                 * and its level bar is what the speaker plays */
                L->has_row = 1;
                SetRect(&L->row_eye, x, y, x + 18, y + 18);
                SetRect(&L->row_spk, x + 19, y, x + 37, y + 18);
                paint_row(mem, x, y, g_frozen ? CUSM_G_EYE_SHUT : CUSM_G_EYE,
                          sends_audio ? (g_muted ? CUSM_G_SPEAKER_OFF : CUSM_G_SPEAKER)
                                      : CUSM_G_SPEAKER_GRAY,
                          sends_audio ? CUSM_G_MIC : CUSM_G_MIC_GRAY,
                          g_meter, p->name[0] ? p->name : "(no name)");
            } else {
                paint_row(mem, x, y, CUSM_G_EYE_GRAY, CUSM_G_SPEAKER_GRAY,
                          sends_audio ? CUSM_G_MIC : CUSM_G_MIC_GRAY, 0,
                          p->name[0] ? p->name : "(no name)");
            }
            y += CUSM_ROW_H;
        }
        if (sec == 2 && s->connected) {
            /* Navigator itself: the reflector never echoes our own OC */
            paint_row(mem, x, y, CUSM_G_EYE_GRAY, CUSM_G_SPEAKER_GRAY, CUSM_G_MIC_GRAY, 0,
                      SUITE_APP_NAME);
            y += CUSM_ROW_H;
        }
    }
    /* footer: the participant count, bold, under a rule */
    fill_rc(mem, cl.left, cl.bottom - CUSM_PLIST_FOOT, cl.right, cl.bottom - CUSM_PLIST_FOOT + 1, CMAC_BLACK);
    snprintf(buf, sizeof(buf), "%d participant%s", cnt[0] + cnt[1] + cnt[2],
             (cnt[0] + cnt[1] + cnt[2]) == 1 ? "" : "s");
    SetRect(&r, cl.left + 6, cl.bottom - CUSM_PLIST_FOOT + 1, cl.right - 4, cl.bottom);
    cmac_text_rc(mem, &r, buf, cmac_font_title(), CMAC_BLACK, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/* The Audio window (refs 1, 2): big bevelled speaker and microphone
 * buttons, under each a dB-scaled meter and a slider, a square Push to
 * Talk button, and the PTT / Lurkers / arrow tabs. Only the speaker side
 * is live. */
static void paint_audio(HDC mem, cusm_layout_t *L) {
    RECT cl, r, mic_m, mic_s;
    int ty, mt, mh = L->meter_h;
    cmac_window(mem, &L->audio, "Audio", 0, 0, CMAC_LTGRAY, &cl, NULL);
    ty = cl.top + 6;
    mt = ty + 44;
    /* speaker column: live */
    SetRect(&L->a_spk, cl.left + 12, ty, cl.left + 48, ty + 36);
    cmac_bevel(mem, &L->a_spk, 1);
    cmac_glyph2(mem, L->a_spk.left + 2, L->a_spk.top + 2, g_muted ? CUSM_G_SPEAKER_OFF : CUSM_G_SPEAKER);
    SetRect(&L->a_meter, cl.left + 30, mt, cl.left + 38, mt + mh);
    SetRect(&L->a_slider, cl.left + 42, mt, cl.left + 56, mt + mh);
    cmac_db_scale(mem, L->a_meter.left, &L->a_meter, 1);
    cmac_meter_v(mem, &L->a_meter, g_meter, RGB(0, 192, 0), 1);
    cmac_slider_v(mem, &L->a_slider, (int)g_volume, 1);
    /* microphone column: a receive-only client has nothing to send */
    SetRect(&r, cl.left + 86, ty, cl.left + 122, ty + 36);
    cmac_bevel(mem, &r, 0);
    cmac_glyph2(mem, r.left + 2, r.top + 2, CUSM_G_MIC_GRAY);
    SetRect(&mic_m, cl.left + 104, mt, cl.left + 112, mt + mh);
    SetRect(&mic_s, cl.left + 116, mt, cl.left + 130, mt + mh);
    cmac_db_scale(mem, mic_m.left, &mic_m, 0);
    cmac_meter_v(mem, &mic_m, 0, RGB(0, 192, 0), 0);
    cmac_slider_v(mem, &mic_s, 0, 0);
    /* Push to Talk and the footer tabs, grayed */
    SetRect(&r, cl.left + 8, mt + mh + 8, cl.right - 8, mt + mh + 34);
    cmac_button(mem, &r, "Push to Talk", 0);
    fill_rc(mem, cl.left, mt + mh + 40, cl.right, mt + mh + 41, CMAC_GRAY);
    SetRect(&r, cl.left + 4, mt + mh + 43, cl.left + 40, mt + mh + 59);
    cmac_tab(mem, &r, "PTT", 0);
    SetRect(&r, cl.left + 40, mt + mh + 43, cl.left + 100, mt + mh + 59);
    cmac_tab(mem, &r, "Lurkers", 0);
    SetRect(&r, cl.left + 100, mt + mh + 43, cl.left + 118, mt + mh + 59);
    cmac_tab(mem, &r, ">", 0);
}

static void paint_kv(HDC mem, const RECT *cl, int line, const char *k, const char *v) {
    RECT r;
    SetRect(&r, cl->left + 8, cl->top + 6 + line * 14, cl->right - 8, cl->top + 6 + line * 14 + 14);
    cmac_text_rc(mem, &r, k, cmac_font_small(), CMAC_BLACK, DT_LEFT | DT_SINGLELINE);
    if (v) cmac_text_rc(mem, &r, v, cmac_font_small(), CMAC_BLACK, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static void paint_stats(HDC mem, const cusm_snap_t *s, cusm_layout_t *L) {
    RECT cl;
    char b[64];
    unsigned el = (s->t0 && s->connected) ? (GetTickCount() - s->t0) / 1000 : 0;
    /* the full title needs the full column; a squeezed one (800x600) says less */
    cmac_window(mem, &L->stats, (L->stats.right - L->stats.left >= CUSM_INFO_W)
                ? "Transmission Statistics" : "Statistics", 0, 0, CMAC_WHITE,
                &cl, &L->stats_close);
    snprintf(b, sizeof(b), "%02u:%02u:%02u", el / 3600, (el / 60) % 60, el % 60);
    paint_kv(mem, &cl, 0, "Connected for", b);
    snprintf(b, sizeof(b), "%u", s->frames);       paint_kv(mem, &cl, 1, "Frames received", b);
    snprintf(b, sizeof(b), "%u.%u fps", g_fps10 / 10, g_fps10 % 10);
    paint_kv(mem, &cl, 2, "Frame rate", b);
    snprintf(b, sizeof(b), "%u", s->drops);        paint_kv(mem, &cl, 3, "Incomplete frames", b);
    snprintf(b, sizeof(b), "%u", s->vpkts);        paint_kv(mem, &cl, 4, "Video packets", b);
    snprintf(b, sizeof(b), "%u", s->apkts);        paint_kv(mem, &cl, 5, "Audio packets", b);
    snprintf(b, sizeof(b), "%u", s->rx);           paint_kv(mem, &cl, 6, "Bytes received", b);
    snprintf(b, sizeof(b), "%u Kbps", g_kbps);     paint_kv(mem, &cl, 7, "Data rate", b);
    paint_kv(mem, &cl, 8, "Audio decoder", cusm_codec_name(g_audio_codec));
    paint_kv(mem, &cl, 9, "Sending", "Disabled");   /* operator wording 2026-11-10 */
}

static void paint_info(HDC mem, const cusm_snap_t *s, cusm_layout_t *L) {
    RECT cl;
    char b[300], ip[24];
    const cusm_peer_t *snd = watched_sender(s);
    cmac_window(mem, &L->info, "Version and IP", 0, 0, CMAC_WHITE, &cl, &L->info_close);
    paint_kv(mem, &cl, 0, SUITE_APP_NAME, SUITE_VERSION_STR);
    snprintf(b, sizeof(b), "%s:%d", g_reflector, g_port);
    paint_kv(mem, &cl, 1, "Reflector", b);
    if (s->refl_ip) ip_str(s->refl_ip, ip, sizeof(ip)); else lstrcpyA(ip, "-");
    paint_kv(mem, &cl, 2, "Reflector IP", ip);
    if (s->my_ip) { ip_str(s->my_ip, ip, sizeof(ip)); snprintf(b, sizeof(b), "%s:%u", ip, s->my_port); }
    else lstrcpyA(b, "-");
    paint_kv(mem, &cl, 3, "Your address", b);
    paint_kv(mem, &cl, 4, "Sender", snd ? snd->name : "-");
    if (snd) ip_str(snd->ip, ip, sizeof(ip)); else lstrcpyA(ip, "-");
    paint_kv(mem, &cl, 5, "Sender advertises", ip);
    if (snd) snprintf(b, sizeof(b), "%u", snd->version); else lstrcpyA(b, "-");
    paint_kv(mem, &cl, 6, "Sender OC version", b);
}

static void render_paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    HDC mem;
    HBITMAP membm, oldbm;
    int W, H;
    cusm_snap_t s;

    GetClientRect(hwnd, &rc);
    W = rc.right - rc.left;
    H = rc.bottom - rc.top;

    /* double-buffer the whole render area to kill flicker */
    mem = CreateCompatibleDC(dc);
    membm = CreateCompatibleBitmap(dc, W, H);
    oldbm = (HBITMAP)SelectObject(mem, membm);

    take_snapshot(&s);
    compute_layout(W, H, &s, &g_lay);
    cmac_desktop(mem, &rc);
    paint_video_window(mem, &s, &g_lay);
    paint_participants(mem, &s, &g_lay);
    paint_audio(mem, &g_lay);
    if (g_show_stats) paint_stats(mem, &s, &g_lay);
    if (g_show_info)  paint_info(mem, &s, &g_lay);

    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbm);
    DeleteObject(membm);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* Mouse on the desktop. Topmost windows first. Returns 1 if handled. */
static int desk_click(int x, int y) {
    POINT pt;
    int sec;
    pt.x = x; pt.y = y;
    if (g_show_info && PtInRect(&g_lay.info, pt)) {
        if (PtInRect(&g_lay.info_close, pt)) g_show_info = 0;
        return 1;
    }
    if (g_show_stats && PtInRect(&g_lay.stats, pt)) {
        if (PtInRect(&g_lay.stats_close, pt)) g_show_stats = 0;
        return 1;
    }
    if (PtInRect(&g_lay.b_eye, pt) || (g_lay.has_row && PtInRect(&g_lay.row_eye, pt))) {
        g_frozen = !g_frozen;
        return 1;
    }
    if (PtInRect(&g_lay.b_spk, pt) || PtInRect(&g_lay.a_spk, pt) ||
        (g_lay.has_row && PtInRect(&g_lay.row_spk, pt))) {
        InterlockedExchange(&g_muted, g_muted ? 0 : 1);
        return 1;
    }
    if (PtInRect(&g_lay.a_slider, pt)) {
        InterlockedExchange(&g_volume, cmac_slider_value_at(&g_lay.a_slider, y));
        g_drag_slider = 1;
        return 1;
    }
    if (PtInRect(&g_lay.b_stats, pt)) { g_show_stats = !g_show_stats; return 1; }
    if (PtInRect(&g_lay.b_info, pt))  { g_show_info = !g_show_info;   return 1; }
    for (sec = 0; sec < 3; sec++)
        if (PtInRect(&g_lay.sect_hdr[sec], pt)) { g_sect_open[sec] = !g_sect_open[sec]; return 1; }
    /* The microphone, Push to Talk and the footer have no handlers: a
     * receive-only client cannot transmit. */
    return 0;
}

static const char *state_text(int st, int retry) {
    static char buf[160];
    switch (st) {
        case ST_CONNECTING:
            snprintf(buf, sizeof(buf), "Connecting to %s:%d...",
                     CUSEEME_HOST, CUSEEME_PORT);
            return buf;
        case ST_HANDSHAKING:
            return "Handshaking...";
        case ST_LOST:
            snprintf(buf, sizeof(buf),
                     "Connection lost - retrying in %ds...", retry);
            return buf;
        case ST_DISCONNECTED:
            return "Disconnected";
        default:
            return "Disconnected";
    }
}

static DWORD    g_meas_tick = 0;
static unsigned g_meas_frames = 0, g_meas_rx = 0;

static void update_status(void) {
    int st, retry;
    unsigned frames, drops, abytes, rx;
    char buf[192];
    EnterCriticalSection(&g_lock);
    st = g_state; retry = g_retry_secs;
    frames = g_frames_total; drops = g_drops_total; abytes = g_audio_bytes;
    rx = g_rx_bytes;
    LeaveCriticalSection(&g_lock);

    /* The Mac status line's measurements: frames and bytes over the real
     * elapsed time since the last tick, so a late timer does not inflate
     * them. Bytes are whole datagrams (headers included); bytes * 8 / ms
     * is kbit/s. */
    {
        DWORD now = GetTickCount();
        if (g_meas_tick && now > g_meas_tick && st == ST_RECEIVING) {
            DWORD el = now - g_meas_tick;
            g_fps10 = (unsigned)(((unsigned long long)(frames - g_meas_frames) * 10000u) / el);
            g_kbps  = (unsigned)(((unsigned long long)(rx - g_meas_rx) * 8u) / el);
        } else if (st != ST_RECEIVING) {
            g_fps10 = g_kbps = 0;
        }
        g_meas_tick = now; g_meas_frames = frames; g_meas_rx = rx;
    }

    if (st == ST_RECEIVING) {
        unsigned fps = frames - g_last_frames;          /* timer fires ~1/s */
        unsigned akbps = ((abytes - g_last_abytes) * 8) / 1000;
        g_last_frames = frames;
        g_last_abytes = abytes;
        /* Include the engaged audio decoder so the chosen codec is visible
         * (and externally verifiable) while receiving. */
        snprintf(buf, sizeof(buf),
                 "Receiving (v: %u fps, a: %u kbps [%s], %u drops)",
                 fps, akbps, cusm_codec_name(g_audio_codec), drops);
        if (g_hStatus) SetWindowTextA(g_hStatus, buf);
    } else {
        if (g_hStatus) SetWindowTextA(g_hStatus, state_text(st, retry));
    }
}

/* Connection-strip session gate (mirrors WAIS/IRC). Disconnected:
 * Reflector/Port/Audio enabled, the toggle reads Connect. Connecting or
 * connected: those grayed, the toggle reads Disconnect, and a click while
 * connecting cancels (worker_stop: the worker polls its stop event every
 * 150 ms). One Connect/Disconnect toggle since 2026-11-11. */
static void cusm_set_connected_state(int connected) {
    g_connected = connected;
    EnableWindow(g_hReflEdit,       !connected);
    EnableWindow(g_hPortEdit,       !connected);
    EnableWindow(g_hAudioCombo,     !connected);
    w31_toggle_set(g_hConnectBtn, connected);
}

/* Connect: read reflector / port / audio-codec from the strip, commit them,
 * and start the receive worker. Replaces the old Start button / click path. */
static void cusm_connect(void) {
    char refl[256], ports[16];
    int  port, sel;

    if (g_connected) return;
    if (!g_hReflEdit || !g_hPortEdit || !g_hAudioCombo) return;

    GetWindowTextA(g_hReflEdit, refl, sizeof(refl));
    GetWindowTextA(g_hPortEdit, ports, sizeof(ports));
    if (!refl[0] || !ports[0]) {
        if (g_hStatus) SetWindowTextA(g_hStatus, "Reflector and port are required.");
        return;
    }
    port = atoi(ports);
    if (port <= 0 || port > 65535) {
        if (g_hStatus) SetWindowTextA(g_hStatus, "Port must be 1-65535.");
        return;
    }
    sel = (int)SendMessageA(g_hAudioCombo, CB_GETCURSEL, 0, 0);
    g_audio_codec = (sel == 1) ? CUSM_AUDF_DELTAMOD : CUSM_AUDF_IDVI;

    strncpy(g_reflector, refl, sizeof(g_reflector) - 1);
    g_reflector[sizeof(g_reflector) - 1] = '\0';
    g_port = port;

    {   /* trace the chosen codec at connect (verifiable via a debugger). */
        char t[320];
        snprintf(t, sizeof(t), "[F7] Connect reflector=%s:%d audio-codec=%s\n",
                 g_reflector, g_port, cusm_codec_name(g_audio_codec));
        OutputDebugStringA(t);
    }

    g_ui_state = CUSM_UI_CONNECTING;
    g_connect_frame = CUSM_CONNECT_FRAMES - 1;   /* seed on full dots */
    g_frozen = 0;                                /* a new session starts live */
    g_show_stats = g_show_info = 1;              /* operator spec 2026-11-11 */
    g_meas_tick = 0; g_fps10 = g_kbps = 0;
    worker_start();
    if (g_hRender) SetTimer(g_hRender, CUSM_TIMER_CONNECT, CUSM_CONNECT_MS, NULL);
    if (g_hRender) SetTimer(g_hRender, CUSM_TIMER_METER, CUSM_METER_MS, NULL);
    cusm_set_connected_state(1);
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
}

/* Disconnect: stop the worker (close, socket, waveOut) and return to the
 * disconnected state with the strip editable. Rendered logo/caption resume. */
static void cusm_disconnect(void) {
    worker_stop();
    if (g_hRender) KillTimer(g_hRender, CUSM_TIMER_CONNECT);
    if (g_hRender) KillTimer(g_hRender, CUSM_TIMER_METER);
    g_meter = 0;
    g_frozen = 0;          /* the session is over: nothing is held */
    g_ui_state = CUSM_UI_SPLASH;
    cusm_set_connected_state(0);
    if (g_hStatus) SetWindowTextA(g_hStatus, "Disconnected");
    if (g_hRender) InvalidateRect(g_hRender, NULL, FALSE);
}

static LRESULT CALLBACK render_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_APP_CUSM_FRAME:
            /* First frame after Start = session established: go live. */
            if (g_ui_state == CUSM_UI_CONNECTING) {
                KillTimer(hwnd, CUSM_TIMER_CONNECT);
                g_ui_state = CUSM_UI_LIVE;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_TIMER:
            if (wParam == CUSM_TIMER_STATUS) {
                update_status();
                /* repaint overlay counters even between frames while live */
                InvalidateRect(hwnd, NULL, FALSE);
            } else if (wParam == CUSM_TIMER_CONNECT) {
                if (g_ui_state == CUSM_UI_CONNECTING) {
                    g_connect_frame = (g_connect_frame + 1) % CUSM_CONNECT_FRAMES;
                    InvalidateRect(hwnd, &g_caption_rect, FALSE);
                }
            } else if (wParam == CUSM_TIMER_METER) {
                /* Level bars: the loudest sample since the last tick, on a
                 * square-root scale so speech reads mid-bar; fall back
                 * 10 per tick rather than dropping, as a meter does. */
                int peak, lvl, old = g_meter;
                EnterCriticalSection(&g_lock);
                peak = g_level_peak;
                g_level_peak = 0;
                LeaveCriticalSection(&g_lock);
                lvl = 0;
                while (lvl < 100 && (lvl + 1) * (lvl + 1) * 32767 / 10000 <= peak) lvl++;
                if (lvl > g_meter) g_meter = lvl;
                else g_meter = (g_meter > 10) ? g_meter - 10 : 0;
                if (g_meter != old) {
                    InvalidateRect(hwnd, &g_lay.b_meter, FALSE);
                    InvalidateRect(hwnd, &g_lay.a_meter, FALSE);
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
            /* Starting is the strip's Connect button; the desktop's own
             * controls are the eye, speaker, slider, Statistics, Version
             * and IP, the list's disclosure triangles and close boxes. */
            SetFocus(hwnd);
            if (desk_click((short)LOWORD(lParam), (short)HIWORD(lParam))) {
                if (g_drag_slider) SetCapture(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        case WM_MOUSEMOVE:
            if (g_drag_slider) {
                InterlockedExchange(&g_volume,
                    cmac_slider_value_at(&g_lay.a_slider, (short)HIWORD(lParam)));
                InvalidateRect(hwnd, &g_lay.audio, FALSE);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (g_drag_slider) {
                g_drag_slider = 0;
                if (msg == WM_LBUTTONUP) ReleaseCapture();
            }
            return 0;
        case WM_PAINT:
            render_paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;   /* painted in WM_PAINT */
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/* ============================ module entry points ============================ */

static void register_render_class(HINSTANCE hInst) {
    WNDCLASSEXA wc;
    if (g_class_reg) return;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    /* The Mac desktop is laid out from the window's size (centred), so a
     * resize must redraw all of it. Until 2026-11-12 a sibling's overpaint
     * did that by accident; with WS_CLIPSIBLINGS on every child
     * (suite_w31.c) Windows keeps the old pixels and repaints only the
     * uncovered strip, leaving the desktop drawn at its old size. */
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = render_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CUSM_RENDER_CLASS;
    RegisterClassExA(&wc);
    g_class_reg = 1;
}

void cuseeme_module_init(void) {
    WSADATA wsa;
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }
    /* WSAStartup is harmless if Navigator already started Winsock elsewhere. */
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

void cuseeme_module_shutdown(void) {
    worker_stop();
    waveout_close();
    if (g_wbuf_init) {
        int i;
        for (i = 0; i < CUSM_NWBUF; i++) { free(g_wbuf[i]); g_wbuf[i] = NULL; }
        g_wbuf_init = 0;
    }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; g_dibBits = NULL; }
    if (g_hCaptionFont) { DeleteObject(g_hCaptionFont); g_hCaptionFont = NULL; }
    cmac_shutdown();
    if (g_tp_scaled) { DeleteObject(g_tp_scaled); g_tp_scaled = NULL; }
    if (g_tp_src)    { DeleteObject(g_tp_src);    g_tp_src = NULL; }
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = NULL; }
}

void cuseeme_module_activate(HWND content) {
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;
    g_hContent = content;
    register_render_class(hInst);

    if (g_controls_created) {
        ShowWindow(g_hReflLbl,       SW_SHOW);
        ShowWindow(g_hReflEdit,      SW_SHOW);
        ShowWindow(g_hPortLbl,       SW_SHOW);
        ShowWindow(g_hPortEdit,      SW_SHOW);
        ShowWindow(g_hAudioLbl,      SW_SHOW);
        ShowWindow(g_hAudioCombo,    SW_SHOW);
        ShowWindow(g_hConnectBtn,    SW_SHOW);
        ShowWindow(g_hDisconnectBtn, SW_SHOW);
        ShowWindow(g_hRender,        SW_SHOW);
        ShowWindow(g_hStatus,        SW_SHOW);
    } else {
        char portbuf[16];
        /* Connection strip: WAIS/IRC-style labels + WS_EX_CLIENTEDGE boxes,
         * an "Audio Codec" dropdown, and right-anchored Connect/Disconnect. */
        g_hReflLbl = CreateWindowA("STATIC", "Reflector:",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 80, 22,
            content, NULL, hInst, NULL);
        g_hReflEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", CUSEEME_HOST,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 200, 26, content, (HMENU)(INT_PTR)IDC_CUSM_REFL_EDIT, hInst, NULL);
        g_hPortLbl = CreateWindowA("STATIC", "Port:",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 44, 22,
            content, NULL, hInst, NULL);
        snprintf(portbuf, sizeof(portbuf), "%d", CUSEEME_PORT);
        g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", portbuf,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
            0, 0, 55, 26, content, (HMENU)(INT_PTR)IDC_CUSM_PORT_EDIT, hInst, NULL);
        g_hAudioLbl = CreateWindowA("STATIC", "Audio Codec:",
            WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP, 0, 0, 100, 22,
            content, NULL, hInst, NULL);
        g_hAudioCombo = CreateWindowExA(WS_EX_CLIENTEDGE, "COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST,
            0, 0, 120, 140, content, (HMENU)(INT_PTR)IDC_CUSM_AUDIO_COMBO, hInst, NULL);
        SendMessageA(g_hAudioCombo, CB_ADDSTRING, 0, (LPARAM)"Intel DVI");
        SendMessageA(g_hAudioCombo, CB_ADDSTRING, 0, (LPARAM)"Delta-Mod");
        SendMessageA(g_hAudioCombo, CB_SETCURSEL, 0, 0);   /* default Intel DVI */
        g_hConnectBtn = CreateWindowA("BUTTON", "Connect",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            0, 0, W31_TOGGLE_W, W31_TOGGLE_H, content, (HMENU)(INT_PTR)IDC_CUSM_CONNECT_BTN, hInst, NULL);
        g_hDisconnectBtn = NULL;   /* merged into the Connect toggle, 2026-11-11 */

        g_hRender = CreateWindowExA(0, CUSM_RENDER_CLASS, "",
            WS_CHILD | WS_VISIBLE,
            0, 0, 100, 100, content, NULL, hInst, NULL);
        g_hStatus = CreateWindowA("STATIC", "Disconnected",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
            0, 0, 100, CUSM_STATUS_H, content,
            (HMENU)(INT_PTR)IDC_CUSM_STATUS, hInst, NULL);
        if (g_hFontUI) {
            SendMessageA(g_hReflLbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hReflEdit,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hPortLbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hPortEdit,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hAudioLbl,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hAudioCombo,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hConnectBtn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hDisconnectBtn, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
            SendMessageA(g_hStatus,        WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        }
        g_controls_created = 1;
    }

    GetClientRect(content, &rc);
    cuseeme_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    /* Manual connection model (matches WAIS/IRC): nothing connects on
     * activation. Start pre-connect and wait for the Connect button. Only
     * force the disconnected UI state when not already in a live session, so
     * switching away and back preserves an active stream. */
    g_ui_state = (g_connected && g_running) ? g_ui_state : CUSM_UI_SPLASH;
    if (!g_connected) cusm_set_connected_state(0);
    SetTimer(g_hRender, CUSM_TIMER_STATUS, 1000, NULL);
    update_status();
    InvalidateRect(g_hRender, NULL, FALSE);
}

void cuseeme_module_deactivate(HWND content) {
    (void)content;
    if (g_hRender) {
        KillTimer(g_hRender, CUSM_TIMER_STATUS);
        KillTimer(g_hRender, CUSM_TIMER_CONNECT);
        KillTimer(g_hRender, CUSM_TIMER_METER);
    }
    g_meter = 0;
    /* F7 carries live audio+video: stop the stream on a tab switch so it does
     * not keep playing audio behind another tab (a deliberate difference from
     * IRC's preserve-across-switch). Return to the disconnected state; the
     * strip's Connect restarts it. */
    worker_stop();
    g_frozen = 0;          /* the session is over: nothing is held */
    g_ui_state = CUSM_UI_SPLASH;
    if (g_controls_created) cusm_set_connected_state(0);
    if (g_hStatus) SetWindowTextA(g_hStatus, "Disconnected");
    if (!g_controls_created) return;
    ShowWindow(g_hReflLbl,       SW_HIDE);
    ShowWindow(g_hReflEdit,      SW_HIDE);
    ShowWindow(g_hPortLbl,       SW_HIDE);
    ShowWindow(g_hPortEdit,      SW_HIDE);
    ShowWindow(g_hAudioLbl,      SW_HIDE);
    ShowWindow(g_hAudioCombo,    SW_HIDE);
    ShowWindow(g_hConnectBtn,    SW_HIDE);
    ShowWindow(g_hDisconnectBtn, SW_HIDE);
    ShowWindow(g_hRender,        SW_HIDE);
    ShowWindow(g_hStatus,        SW_HIDE);
}

void cuseeme_module_resize(HWND content, int w, int h) {
    const int margin = 16;
    const int row_h  = 26;
    const int btn_h  = 30;
    int strip  = CUSM_STRIP_H;
    int status = CUSM_STATUS_H;
    int render_h;
    (void)content;
    if (!g_controls_created) return;

    /* Connection strip (row 1): Reflector / Port / Audio Codec, with
     * ~34px gaps between groups (mirrors IRC); Connect + Disconnect anchored
     * right so they hold their place when the window widens. */
    if (w >= 900) {
        MoveWindow(g_hReflLbl,   margin,       18, 80,  22,    TRUE);
        MoveWindow(g_hReflEdit,  margin + 84,  14, 195, row_h, TRUE);
        MoveWindow(g_hPortLbl,   margin + 310, 18, 44,  22,    TRUE);
        MoveWindow(g_hPortEdit,  margin + 358, 14, 55,  row_h, TRUE);
        MoveWindow(g_hAudioLbl,  margin + 443, 18, 100, 22,    TRUE);
        /* Combo height param is the dropped-open list height; the closed box
         * height is derived from the font. */
        MoveWindow(g_hAudioCombo, margin + 547, 14, 120, 160,  TRUE);
    } else {
        /* 2026-11-10: below 900 px the wide row put the right-anchored
         * Connect button UNDER the Audio Codec combo (at 800x600 a click on
         * Connect opened the list instead). Same order, tighter. */
        MoveWindow(g_hReflLbl,   margin,       18, 62,  22,    TRUE);
        MoveWindow(g_hReflEdit,  margin + 64,  14, 150, row_h, TRUE);
        MoveWindow(g_hPortLbl,   margin + 226, 18, 32,  22,    TRUE);
        MoveWindow(g_hPortEdit,  margin + 260, 14, 50,  row_h, TRUE);
        /* 80 px since 2026-11-11: the label is bold now (70 clipped its colon) */
        MoveWindow(g_hAudioLbl,  margin + 322, 18, 80,  22,    TRUE);
        MoveWindow(g_hAudioCombo, margin + 404, 14, 100, 160,  TRUE);
    }
    /* The Connect/Disconnect toggle: right end, centred on the field row. */
    MoveWindow(g_hConnectBtn, w - margin - W31_TOGGLE_W, 14 + (row_h - W31_TOGGLE_H) / 2,
               W31_TOGGLE_W, W31_TOGGLE_H, TRUE);
    (void)btn_h;

    /* Render fills between the strip and the status bar. */
    render_h = h - strip - status;
    if (render_h < 0) render_h = 0;
    if (g_hRender) MoveWindow(g_hRender, 0, strip, w, render_h, TRUE);
    if (g_hStatus) MoveWindow(g_hStatus, margin, h - status, w - 2 * margin, status, TRUE);
}

BOOL cuseeme_module_on_command(HWND content, WPARAM wParam, LPARAM lParam) {
    int id   = LOWORD(wParam);
    int code = HIWORD(wParam);
    (void)content; (void)lParam;
    if (code == BN_CLICKED) {
        if (id == IDC_CUSM_CONNECT_BTN) {     /* the toggle: its label says which */
            if (g_connected) cusm_disconnect();
            else             cusm_connect();
            return TRUE;
        }
        if (id == IDC_CUSM_DISCONNECT_BTN) { cusm_disconnect(); return TRUE; }
    }
    return FALSE;
}

BOOL cuseeme_module_has_unsaved(void) {
    return FALSE;   /* receive-only stream: nothing to lose on switch */
}
