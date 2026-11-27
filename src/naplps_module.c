/*
 * naplps_module.c - the Videotex tab: NAPLPS and Minitel.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The tab is a private container (MGTNapPanel) holding a strip of
 * controls over a render canvas (MGTNapCanvasV1). Everything below the
 * wire is in naplps_decode.[ch] and naplps_surface.[ch], which are
 * clean-room from ANSI X3.110-1983 / CSA T500-1983 and contain no Win32
 * code; the full provenance statement lives at the top of
 * naplps_decode.h.
 *
 * TRANSPORT. The tab reuses telnet_proto.[ch], exactly as the RIPscrip
 * and Terminal tabs do, rather than opening a second socket layer. That
 * is safe here for a reason worth stating: NAPLPS is a SEVEN-BIT format.
 * Opcodes are 0x20-0x3F, operands 0x40-0x7F and text is ASCII, so the
 * byte 0xFF that telnet uses as IAC cannot occur in valid NAPLPS data.
 * The IAC filter is therefore a pass-through on a NAPLPS stream, and in
 * exchange the tab gets RFC 854 option negotiation for free if the far
 * end happens to be a telnet service or a serial-to-TCP bridge that
 * negotiates on its own behalf. A frame that arrives 8-bit is a local
 * file, not a wire stream, and files do not go through this path.
 *
 * THERE IS NO HANDSHAKE, and that is a real difference from the RIPscrip
 * tab next door. RIPscrip 1.54 defines an auto-sensing query and a
 * version answer; NAPLPS defines no terminal identification, no
 * capability query and no status report, and the transmission-control
 * characters are reserved to lower layers and ignored at the
 * presentation layer (6.1.4). So there is nothing to send on connect and
 * nothing to wait for. A caller declares its capability by choosing the
 * port.
 *
 * TWO FORMATS, ONE TAB (operator decision, 2026-09-27). The tab is
 * labelled "Videotex" and carries a Format dropdown, NAPLPS or Minitel,
 * which can only be changed while disconnected. Both formats use one
 * session and the same status line; the host and the port follow the
 * format (naplps.teletext.ca 1983, minitel.teletext.ca 516; one host per
 * format since Navigator 2.0). Minitel mode is a complete terminal, screen and keyboard,
 * built by minitel_view.[ch] over the clean-room minitel_decode.[ch] and
 * minitel_screen.[ch]. The NAPLPS paths below are the Phase 2a code,
 * unchanged: the Minitel paths branch off before they reach them.
 *
 * KEYBOARD. Upstream keys follow the ALEXtel Subset profile documented in
 * README.PP3 section 7.2: printable ASCII, CR, BS, DEL, TAB and ESC go up
 * as themselves, and F1 to F9 send the Subset default sequences
 * ESC # <c> CR. Characters are derived from the virtual key rather than
 * from WM_CHAR, because WM_CHAR is layout-translated and a non-Latin
 * keyboard layout would otherwise produce something other than ASCII.
 * That lesson is recorded in the F6 Telnet work and applies unchanged.
 *
 * SITES AND WEBSOCKET (Navigator 2.0, operator 2026-11-24,
 * docs/2026-11-24_NAVIGATOR_SITE_LISTS_WS_BUILD.md). The Minitel format
 * has a second strip row: Site (the built-in list of suite_sites),
 * Protocol (Telnet or WebSocket (ws)) and Path. Picking a site only fills
 * the fields; it never connects. A full telnet:// or ws:// address pasted
 * into Host fills Protocol, Host, Port and Path; a gateway path written
 * "/url=" is rewritten to the working "/?url=" form, with a note. wss://
 * is refused: there are no secure connections in this version. Over
 * WebSocket the session runs on ws_proto, which posts the same events as
 * telnet_proto, so everything below the connect is shared and the
 * Minitel decoder never knows which transport fed it. NAPLPS keeps one
 * row and Telnet only.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>

#include "naplps_module.h"
#include "suite_shell.h"
#include "telnet_proto.h"
#include "naplps_decode.h"
#include "naplps_surface.h"
#include "minitel_view.h"
#include "ws_proto.h"
#include "suite_sites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "suite_w31.h"

/* Module-local control IDs (6600+, clear of the RIPscrip 6400 block). */
#define IDC_NAP_HOST_EDIT   6601
#define IDC_NAP_PORT_EDIT   6602
#define IDC_NAP_CONNECT_BTN 6603
#define IDC_NAP_DISCONN_BTN 6604
/* 6605 was the Open frame / Open page button, removed 2026-09-27: local
 * files are for testing only and open with Ctrl+O on the screen. */
#define IDC_NAP_MONO_CHK    6606
#define IDC_NAP_CANVAS      6607
#define IDC_NAP_FORMAT_CMB  6608
#define IDC_NAP_SCAN_CHK    6609
/* Navigator 2.0: the Minitel second row (6620 up is the Minitel view's). */
#define IDC_NAP_SITE_CMB    6610
#define IDC_NAP_PROTO_CMB   6611
#define IDC_NAP_PATH_EDIT   6612

/* The two formats of the Format dropdown, in its order. */
#define FMT_NAPLPS   0
#define FMT_MINITEL  1

/* The two protocols of the Protocol dropdown (Minitel only), in its order. */
#define PROTO_TELNET 0
#define PROTO_WS     1

/* Posted to the panel by the Host field after a paste (Navigator 2.0). */
#define WM_NAP_PASTED  (WM_USER + 44)

#define NAP_PANEL_CLASS   "MGTNapPanel"
#define NAP_CANVAS_CLASS  "MGTNapCanvasV1"

#define WM_NAP_NOTIFY  (WM_USER + 43)

/* The WAIT control's timer. 5.3.2.8 gives the interval in tenths. */
#define IDT_NAP_WAIT   1

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

static HWND g_hContent  = NULL;
static HWND g_hPanel    = NULL;
static HWND g_hCanvas   = NULL;
static HWND g_hHostLbl  = NULL, g_hHostEdit = NULL;
static HWND g_hPortLbl  = NULL, g_hPortEdit = NULL;
static HWND g_hConnBtn  = NULL, g_hDiscBtn  = NULL;   /* 2026-11-11: Disconnect
                                                        retired (one toggle); NULL */
static HWND g_hMonoChk  = NULL;
static HWND g_hStatus   = NULL;
static HWND g_hFmtLbl   = NULL, g_hFmtCmb   = NULL;
static HWND g_hScanChk  = NULL;
/* Navigator 2.0: the Minitel second row. */
static HWND g_hSiteLbl  = NULL, g_hSiteCmb  = NULL;
static HWND g_hProtoLbl = NULL, g_hProtoCmb = NULL;
static HWND g_hPathLbl  = NULL, g_hPathEdit = NULL;
static int  g_proto     = PROTO_TELNET;  /* the dropdown (Minitel only) */
static BOOL g_filling   = FALSE;         /* the tab is writing the fields:
                                            not the user, so the Site
                                            list keeps its choice */
static WNDPROC g_host_old_proc = NULL;   /* the Host field, for paste */

/* MINITEL IS THE DEFAULT FORMAT (operator, 2026-09-27); NAPLPS stays one
 * click away in the dropdown. */
static int  g_format         = FMT_MINITEL; /* the dropdown           */
static int  g_session_format = FMT_MINITEL; /* the format of the live
                                               or last session         */
static BOOL g_mono_mtl       = FALSE;       /* Minitel: COLOUR by default
                                               (operator, 2026-10-20);
                                               the 1B's grey tube is
                                               one tick away. Not
                                               persisted, so every
                                               launch starts in colour */
static BOOL g_scan_mtl       = TRUE;        /* Minitel: scan lines by
                                               default (operator)      */

static BOOL g_controls_created = FALSE;
static BOOL g_panel_class_reg  = FALSE;
static BOOL g_canvas_class_reg = FALSE;

static TelnetProto *g_telnet    = NULL;
static WsProto     *g_ws        = NULL;   /* Minitel over ws (2.0); at most
                                             one of g_telnet / g_ws is set */
static BOOL         g_connected = FALSE;
static char         g_active_host[256] = MINITEL_DEFAULT_HOST;
static int          g_active_port      = MINITEL_DEFAULT_PORT;

/* A session exists, live or still connecting, on either transport. */
#define NAP_LINK_BUSY() (g_telnet != NULL || g_ws != NULL)

static BOOL nap_apply_address(void);

static naplps_decoder_t *g_dec = NULL;
static BOOL              g_mono = FALSE;
static char              g_loaded_name[MAX_PATH] = "";

static HBITMAP g_hDib     = NULL;
static void   *g_dib_bits = NULL;
static HDC     g_hMemDC   = NULL;
static HBITMAP g_hOldDib  = NULL;

/* ------------------------------------------------------------------ */
/* Status line                                                         */
/* ------------------------------------------------------------------ */

static void nap_status(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof buf - 1, fmt, ap);
    buf[sizeof buf - 1] = '\0';
    va_end(ap);
    /* ONE status line (operator, 2026-09-27): the tab's own, which also
     * carries the counters. It is no longer mirrored into the window's
     * status bar, which the tab blanks while it is shown, so nothing is
     * ever printed twice. */
    if (g_hStatus) SetWindowTextA(g_hStatus, buf);
}

/*
 * The counter line.
 *
 * This exists because of the counter-honesty rule the project adopted
 * during the RIPscrip work: a feature REGISTERED is not a feature
 * RENDERED. The decoder deliberately skips macros, DRCS, mosaics,
 * unprotected fields, BLINK and the INCREMENTAL encodings, and it would
 * be easy for a frame that leans on one of them to look merely sparse
 * rather than incompletely drawn. Putting the skip count on screen means
 * the operator can tell those two apart without reading a log.
 */
static void nap_update_counts(void)
{
    const naplps_stats_t *s;
    char buf[256];

    if (!g_dec || !g_hStatus) return;
    s = naplps_get_stats(g_dec);

    _snprintf(buf, sizeof buf - 1,
              "%lu bytes  %lu drawn  %lu chars  %lu skipped  "
              "%lu esc?  %lu esc-bad  %lu short%s",
              s->bytes_in, s->pdi_executed, s->chars_drawn, s->pdi_skipped,
              s->esc_unknown, s->esc_malformed, s->operand_short,
              naplps_get_mode(g_dec) == NAPLPS_MODE_ASCII ? "  [ASCII mode]" : "");
    buf[sizeof buf - 1] = '\0';
    SetWindowTextA(g_hStatus, buf);
}

/* ------------------------------------------------------------------ */
/* DIB                                                                 */
/* ------------------------------------------------------------------ */

static void nap_dib_create(void)
{
    BITMAPINFO bi;
    HDC screen;

    if (g_hDib) return;

    ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = NAPLPS_W;
    bi.bmiHeader.biHeight      = -NAPLPS_H;    /* top-down */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    g_hDib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_dib_bits,
                              NULL, 0);
    if (g_hDib) {
        g_hMemDC  = CreateCompatibleDC(screen);
        g_hOldDib = (HBITMAP)SelectObject(g_hMemDC, g_hDib);
    }
    ReleaseDC(NULL, screen);
}

static void nap_dib_refresh(void)
{
    if (!g_dib_bits || !g_dec) return;
    naplps_surface_to_bgra(naplps_get_surface(g_dec),
                           (unsigned char *)g_dib_bits, g_mono ? 1 : 0);
}

/*
 * Where the 256x200 surface lands in the canvas.
 *
 * INTEGER SCALING, like the NABU tab and unlike the RIPscrip tab, and the
 * reason is in the raster rather than in taste. The videotex SRM puts the
 * visible unit screen at X 0 to 1 and Y 0 to 0.78125 over 256 by 200
 * pixels, and 200/256 is exactly 0.78125, so one pixel is 1/256 of the
 * unit screen on BOTH axes: the SRM's pixels are square. An integer scale
 * therefore preserves the specified geometry exactly and keeps the 5x7
 * glyphs crisp. A fractional scale would give uneven pixel blocks and
 * make 40-column text wobble, which is the same finding the NABU integer
 * aspect fit was written for.
 *
 * The RIPscrip tab does the opposite because its raster is different:
 * 640x350 is NOT square-pixel, it was displayed on a 4:3 monitor, so that
 * tab has to declare a presentation aspect and stretch to it.
 */
static void nap_dest_rect(int cw, int ch, RECT *out)
{
    int scale, w, h;

    scale = cw / NAPLPS_W;
    if (ch / NAPLPS_H < scale) scale = ch / NAPLPS_H;
    if (scale < 1) scale = 1;

    w = NAPLPS_W * scale;
    h = NAPLPS_H * scale;
    out->left   = (cw - w) / 2;
    out->top    = (ch - h) / 2;
    out->right  = out->left + w;
    out->bottom = out->top + h;
}

/* ------------------------------------------------------------------ */
/* Session                                                             */
/* ------------------------------------------------------------------ */

static void nap_set_session_ui(BOOL connected)
{
    HWND had_focus;

    g_connected = connected;
    if (!g_controls_created) return;
    had_focus = GetFocus();
    /* 2026-11-11: one Connect/Disconnect toggle in NAPLPS format (Minitel
     * keeps its own CONNEXION/FIN key, so there the toggle stays hidden). */
    w31_toggle_set(g_hConnBtn, connected);
    EnableWindow(g_hHostEdit, !connected);
    EnableWindow(g_hPortEdit, !connected);
    /* The format can only change while disconnected. */
    EnableWindow(g_hFmtCmb, !connected);
    /* Navigator 2.0: the second row likewise; Path only means something
     * over WebSocket, so under Telnet it stays grayed. */
    EnableWindow(g_hSiteCmb,  !connected);
    EnableWindow(g_hProtoCmb, !connected);
    EnableWindow(g_hPathEdit, !connected && g_proto == PROTO_WS);
    if (g_session_format == FMT_MINITEL) minitel_view_set_connected(connected);

    /*
     * THE DISCONNECT FOCUS FIX (found in the 2026-09-27 live gate). A
     * click on Disconnect gives that button the keyboard focus, and the
     * line above then disables it, which leaves NO window with the focus:
     * every key went nowhere until the screen was clicked. Give the focus
     * back to the screen of the current format instead.
     */
    if (!connected && (had_focus == g_hDiscBtn || GetFocus() == NULL)) {
        if (g_format == FMT_MINITEL) minitel_view_focus();
        else if (g_hCanvas) SetFocus(g_hCanvas);
    }
}

/*
 * Throw away transport events still queued for the panel. After
 * telnet_proto_close the worker thread has been joined, so nothing new
 * can arrive, but what it posted before it stopped is still in the
 * queue. Left there, a stale TELNET_EVT_CLOSED made a LOCAL disconnect
 * end on "Host closed the connection" (the 2026-09-27 quirk), and could
 * have closed a session opened again straight afterwards. Data chunks
 * are freed; the error text is static.
 */
static void nap_drain_events(void)
{
    MSG m;
    if (!g_hPanel) return;
    while (PeekMessageA(&m, g_hPanel, WM_NAP_NOTIFY, WM_NAP_NOTIFY, PM_REMOVE)) {
        if (m.wParam == TELNET_EVT_DATA && m.lParam)
            telnet_proto_free_chunk((TelnetDataChunk *)m.lParam);
    }
}

static void nap_close_session(void)
{
    if (g_telnet) {
        telnet_proto_close(g_telnet);
        g_telnet = NULL;
        nap_drain_events();
    }
    if (g_ws) {
        /* ws_proto posts the same events and allocates its chunks as
         * telnet_proto does, so the same drain frees them. */
        ws_proto_close(g_ws);
        g_ws = NULL;
        nap_drain_events();
    }
    if (g_connected) {
        /* A PDI sequence is terminated by the next opcode or by a
         * non-numeric byte (5.3.1.1), so the last command of a session
         * has nothing to terminate it. Flush it rather than leaving the
         * final shape undrawn. */
        if (g_session_format == FMT_NAPLPS) {
            if (g_dec) { naplps_flush(g_dec); nap_dib_refresh(); }
            if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
        }
    }
    nap_set_session_ui(FALSE);
}

/* When the user last sent something, and whether the host has drawn
 * anything since: the test for "the server's own goodbye was the last
 * thing drawn". */
static DWORD g_last_rx_tick = 0;
static BOOL  g_rx_since_tx  = FALSE;

static void nap_send(const void *buf, int len)
{
    if (g_connected && len > 0) {
        if (g_telnet)  telnet_proto_send(g_telnet, buf, (size_t)len);
        else if (g_ws) ws_proto_send(g_ws, buf, (size_t)len);   /* a text frame */
        else return;
        g_rx_since_tx = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* Logoff (operator, 2026-09-27)                                       */
/* ------------------------------------------------------------------ */

/*
 * On every disconnect, local or by the host, the screen says so, and 5 s
 * later the format's default screen comes back:
 *
 *   - if the HOST closed the line and the last thing drawn was the
 *     host's own (bytes arrived after the user's last key, within 3 s of
 *     the close), that is the service's goodbye, and it is left alone;
 *   - otherwise (Disconnect, F10, CONNEXION/FIN twice, or a host that
 *     closed without a word) the tab draws its own logoff message, in
 *     the format's own style: NAPLPS in its text presentation (the one
 *     the service's banner uses), Minitel in videotex.
 *
 * Switching format or connecting again cancels the 5 s timer.
 */
#define IDT_NAP_LOGOFF   2
#define NAP_LOGOFF_MS    5000
#define NAP_GOODBYE_MS   3000

#define WHY_LOCAL 0
#define WHY_HOST  1

static void nap_cancel_logoff(void)
{
    if (g_hPanel) KillTimer(g_hPanel, IDT_NAP_LOGOFF);
}

/* The default (idle) screen of a format: power-on state, which brings
 * back the format's own guidance on the canvas. */
static void nap_default_screen(int fmt)
{
    if (fmt == FMT_MINITEL) {
        minitel_view_default_screen();
    } else {
        if (g_dec) naplps_reset(g_dec);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
    }
}

static void nap_draw_logoff(int fmt)
{
    if (fmt == FMT_MINITEL) {
        minitel_view_show_logoff(g_active_host);
    } else if (g_dec) {
        /* ESC % @ after a reset begins a fresh text presentation (the
         * G7 fix C), the same one the service's ASCII banner is drawn in.
         * NAPLPS text is proportional, so the centring is approximate. */
        char buf[512];
        int n, pad_a, pad_b;
        size_t hl = strlen(g_active_host);
        pad_a = (40 - 17) / 2;                        /* "Disconnected from" */
        pad_b = hl < 40 ? (int)(40 - hl) / 2 : 0;
        n = _snprintf(buf, sizeof buf - 1,
                      "\x1B%%@\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n"
                      "%*sDisconnected from\r\n\r\n%*s%.40s",
                      pad_a, "", pad_b, "", g_active_host);
        if (n < 0) n = (int)sizeof buf - 1;
        naplps_reset(g_dec);
        naplps_feed(g_dec, (const unsigned char *)buf, (size_t)n);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
    }
}

/* End a session, locally or because the host did, then the logoff. */
static void nap_close_session(void);
static void nap_end_session(int why)
{
    BOOL was_connected = g_connected;
    BOOL goodbye = (why == WHY_HOST) && g_rx_since_tx
                   && (GetTickCount() - g_last_rx_tick) <= NAP_GOODBYE_MS;

    nap_close_session();
    if (!was_connected) return;          /* a failed connect: no logoff */
    if (!goodbye) nap_draw_logoff(g_session_format);
    if (g_hPanel) SetTimer(g_hPanel, IDT_NAP_LOGOFF, NAP_LOGOFF_MS, NULL);
}

/* ------------------------------------------------------------------ */
/* Upstream keys                                                       */
/* ------------------------------------------------------------------ */

/*
 * The ALEXtel Subset function keys, README.PP3 section 7.2.
 *
 * The table there gives, for each key, a Subset MACRO NAME and a DEFAULT
 * SEQUENCE, with the rule "In the subset handshake, if the macro is
 * undefined, the default sequence is transmitted." The host may define
 * transmit-macros (DEFT MACRO, 6.2.2.3) at macro names 2/0 to 2/7 and
 * thereby choose what each key sends; if it defines none, the terminal
 * falls back to these fixed sequences. This tab defines nothing and
 * sends the defaults, which is what a service should expect.
 *
 * THE LABELS ARE BELL'S, NOT PP3's. The ALEX Magazine user guide of June
 * 1990, page VII, lists the terminal's eight function keys as Cancel,
 * Top, Previous, Help, Refresh, Menu, Next and Send, in exactly the order
 * of the Subset macro names 2/0 through 2/7. Six match README.PP3's
 * labels outright; the two that differ, BACK/Previous and
 * REDISPLAY/Refresh, carry identical descriptions. Bell's own words for
 * its own terminal are the better-sourced labels, so those are the ones
 * shown to the operator.
 *
 * F9 EXIT has no counterpart on the hardware: the magazine says "The
 * eight function keys" and lists eight. Leaving ALEX was done with the
 * connect/disconnect key. The sequence is still sent, because a host that
 * understands the Subset defaults understands the ninth, but it is
 * labelled as PP3 and VIA rather than as an ALEXTEL key.
 */
typedef struct { int vk; const char *seq; const char *label; } napkey_t;

static const napkey_t g_fkeys[] = {
    { VK_F1, "\x1B#4\r", "F1 Help"     },   /* macro 2/3 */
    { VK_F2, "\x1B#3\r", "F2 Refresh"  },   /* macro 2/4 */
    { VK_F3, "\x1B#?\r", "F3 Cancel"   },   /* macro 2/0 */
    { VK_F4, "\x1B#6\r", "F4 Top"      },   /* macro 2/1 */
    { VK_F5, "\x1B#2\r", "F5 Previous" },   /* macro 2/2 */
    { VK_F6, "\x1B#:\r", "F6 Menu"     },   /* macro 2/5 */
    { VK_F7, "\x1B#8\r", "F7 Next"     },   /* macro 2/6 */
    { VK_F8, "\x1B#1\r", "F8 Send"     },   /* macro 2/7 */
    { VK_F9, "\x1B#9\r", "F9 Exit (PP3/VIA, not an ALEXTEL key)" }
};
#define NAP_FKEY_COUNT ((int)(sizeof g_fkeys / sizeof g_fkeys[0]))

/*
 * Derive an ASCII byte from a virtual key and the shift state.
 *
 * WM_CHAR is layout-translated, so on a non-Latin layout it delivers
 * something that is not ASCII and the host sees mojibake. The signature
 * of getting this wrong is "numbers work, letters do not". Deriving from
 * the VK keeps the upstream 7-bit whatever the operator types on.
 */
static int nap_vk_to_ascii(int vk, BOOL shift)
{
    static const char *digits_shift = ")!@#$%^&*(";

    if (vk >= 'A' && vk <= 'Z') return shift ? vk : (vk - 'A' + 'a');
    if (vk >= '0' && vk <= '9')
        return shift ? digits_shift[vk - '0'] : vk;

    switch (vk) {
    case VK_SPACE:      return ' ';
    case VK_OEM_1:      return shift ? ':'  : ';';
    case VK_OEM_PLUS:   return shift ? '+'  : '=';
    case VK_OEM_COMMA:  return shift ? '<'  : ',';
    case VK_OEM_MINUS:  return shift ? '_'  : '-';
    case VK_OEM_PERIOD: return shift ? '>'  : '.';
    case VK_OEM_2:      return shift ? '?'  : '/';
    case VK_OEM_3:      return shift ? '~'  : '`';
    case VK_OEM_4:      return shift ? '{'  : '[';
    case VK_OEM_5:      return shift ? '|'  : '\\';
    case VK_OEM_6:      return shift ? '}'  : ']';
    case VK_OEM_7:      return shift ? '"'  : '\'';
    default: break;
    }
    return -1;
}

static void nap_key_down(int vk)
{
    BOOL shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int  i, c;
    char ch;

    if (!g_connected) return;

    for (i = 0; i < NAP_FKEY_COUNT; i++)
        if (g_fkeys[i].vk == vk) {
            nap_send(g_fkeys[i].seq, (int)strlen(g_fkeys[i].seq));
            nap_status("Sent %s", g_fkeys[i].label);
            return;
        }

    /*
     * F10 BREAK, and what it can and cannot mean over TCP.
     *
     * README.PP3 6.2 says "the key F10-BREAK is the same operation as the
     * phone key on an ALEXtel terminal", and its own table gives F10 as a
     * "250 ms space (break/attention) condition". The ALEX Magazine, page
     * VIII, says what that key does for the user: connect/disconnect, and
     * pressed twice it leaves the service and then ALEX.
     *
     * So on real hardware F10 is not a character at all. It is a
     * LINE-LEVEL signal: a break is a sustained space on the serial line,
     * below the data stream entirely, and the phone key physically
     * originates or terminates the call. Over TCP there is no line to
     * hold in a space condition and no call to drop. The only thing in
     * the neighbourhood is telnet's own IAC BRK, which exists but travels
     * as data on a connection the far end may not be treating as telnet.
     *
     * DECISION, recorded rather than guessed at: F10 is bound to the
     * user-visible behaviour the Bell guide documents, which is
     * disconnect. It does not attempt to synthesise a break. If Phase 2b's
     * service ever wants an attention signal, that is a protocol decision
     * for the service, not something this tab can invent.
     */
    if (vk == VK_F10) { nap_end_session(WHY_LOCAL); nap_status("Disconnected (F10)"); return; }

    switch (vk) {
    case VK_RETURN: ch = '\r';   nap_send(&ch, 1); return;
    case VK_BACK:   ch = '\b';   nap_send(&ch, 1); return;
    case VK_TAB:    ch = '\t';   nap_send(&ch, 1); return;
    case VK_ESCAPE: ch = '\x1B'; nap_send(&ch, 1); return;
    case VK_DELETE: ch = '\x7F'; nap_send(&ch, 1); return;
    default: break;
    }

    c = nap_vk_to_ascii(vk, shift);
    if (c > 0) { ch = (char)c; nap_send(&ch, 1); }
}

/* ------------------------------------------------------------------ */
/* Incoming data                                                       */
/* ------------------------------------------------------------------ */

static void nap_on_connected(void)
{
    nap_set_session_ui(TRUE);
    /*
     * Nothing is sent on connect. There is no NAPLPS handshake to answer
     * and no terminal identification to volunteer (4.8 of the Phase 1
     * report, and 6.1.4 of the standard, which reserves the transmission
     * control characters to lower layers). The host talks first.
     */
    if (g_ws) nap_status("Connected to ws://%s:%d", g_active_host, g_active_port);
    else      nap_status("Connected to %s:%d", g_active_host, g_active_port);
    if (g_session_format == FMT_MINITEL) minitel_view_focus();
    else SetFocus(g_hCanvas);
}

static void nap_on_data(TelnetDataChunk *chunk)
{
    int tenths;

    if (!chunk) return;
    if (chunk->data && chunk->len > 0) {
        g_last_rx_tick = GetTickCount();     /* the host drew something */
        g_rx_since_tx  = TRUE;
    }
    if (g_session_format == FMT_MINITEL) {
        if (chunk->data && chunk->len > 0)
            minitel_view_feed(chunk->data, (size_t)chunk->len);
        telnet_proto_free_chunk(chunk);
        return;
    }
    if (g_dec && chunk->data && chunk->len > 0) {
        naplps_feed(g_dec, chunk->data, (size_t)chunk->len);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
        nap_update_counts();

        /*
         * Honour WAIT, 5.3.2.8. Phase 1 section 5.2 records why this
         * matters more here than it looks: the host cannot know when a
         * frame has finished drawing, there is no acknowledgement and no
         * completion signal, so WAIT is the format's own and only answer
         * to "pause before the next thing". Executing it is the
         * terminal's job.
         */
        tenths = naplps_take_wait(g_dec);
        if (tenths > 0 && g_hCanvas) {
            UpdateWindow(g_hCanvas);
            SetTimer(g_hCanvas, IDT_NAP_WAIT, (UINT)(tenths * 100), NULL);
        }
    }
    telnet_proto_free_chunk(chunk);
}

static void nap_on_error(const char *msg)
{
    nap_status("Connection failed: %s", msg ? msg : "unknown error");
    nap_end_session(WHY_HOST);
}

static void nap_on_closed(void)
{
    /* Only a real host close gets here now: a local close drains the
     * transport's leftover events (nap_drain_events). */
    nap_end_session(WHY_HOST);
    nap_status("Host closed the connection");
}

/* ------------------------------------------------------------------ */
/* Open a local file                                                   */
/* ------------------------------------------------------------------ */

static void nap_open_file(void)
{
    OPENFILENAMEA ofn;
    char path[MAX_PATH] = "";
    FILE *f;
    unsigned char *buf;
    long len;
    size_t got;

    ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize  = sizeof ofn;
    ofn.hwndOwner    = GetAncestor(g_hPanel, GA_ROOT);
    ofn.lpstrFilter  = "NAPLPS frames (*.nap;*.naplps)\0*.nap;*.naplps\0"
                       "All files (*.*)\0*.*\0";
    ofn.lpstrFile    = path;
    ofn.nMaxFile     = sizeof path;
    ofn.lpstrTitle   = "Open a NAPLPS frame";
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;

    if (!W31_UNHOOKED(GetOpenFileNameA(&ofn))) return;

    f = fopen(path, "rb");
    if (!f) { nap_status("Cannot open %s", path); return; }

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (16L << 20)) {
        fclose(f);
        nap_status("%s is empty or implausibly large", path);
        return;
    }
    buf = (unsigned char *)malloc((size_t)len);
    if (!buf) { fclose(f); nap_status("Out of memory"); return; }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);

    /*
     * A file is a fresh presentation, so the decoder starts from the
     * power-on default state rather than inheriting whatever the previous
     * frame left set. This matters: DOMAIN lengths, the colour map, the
     * character field and the active field all persist until something
     * changes them, and a frame that relies on the defaults would decode
     * against the last frame's state instead.
     */
    naplps_reset(g_dec);
    naplps_feed(g_dec, buf, got);
    naplps_flush(g_dec);
    free(buf);

    nap_dib_refresh();
    if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);

    {
        const char *base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        _snprintf(g_loaded_name, sizeof g_loaded_name - 1, "%s", base);
        g_loaded_name[sizeof g_loaded_name - 1] = '\0';
    }
    nap_update_counts();
}

/* ------------------------------------------------------------------ */
/* Canvas                                                              */
/* ------------------------------------------------------------------ */

static void nap_canvas_paint(HWND hwnd, HDC hdc)
{
    RECT rc, dst;
    HDC  mem;
    HBITMAP bmp, oldbmp;
    int cw, ch;

    GetClientRect(hwnd, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    /* Composite off-screen and blit once: the letterbox fill and the
     * scaled surface must not appear as two separate paints. */
    mem = CreateCompatibleDC(hdc);
    bmp = CreateCompatibleBitmap(hdc, cw, ch);
    oldbmp = (HBITMAP)SelectObject(mem, bmp);

    FillRect(mem, &rc, g_hBrushBlack);

    if (g_dec && naplps_ever_drew(g_dec) && g_hMemDC) {
        nap_dest_rect(cw, ch, &dst);
        /* COLORONCOLOR is GDI's nearest neighbour: no smoothing, no
         * averaging, so scaled videotex pixels stay hard-edged blocks
         * and the 5x7 glyphs stay readable. */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchBlt(mem, dst.left, dst.top,
                   dst.right - dst.left, dst.bottom - dst.top,
                   g_hMemDC, 0, 0, NAPLPS_W, NAPLPS_H, SRCCOPY);
    } else {
        /*
         * The function key legend uses BELL'S OWN LABELS for the ALEXTEL,
         * from the ALEX Magazine user guide of June 1990, page VII:
         * Cancel, Top, Previous, Help, Refresh, Menu, Next, Send, printed
         * there in exactly the order of the Subset macro names 2/0 to
         * 2/7. Six of the eight match README.PP3's labels outright; the
         * two that differ, BACK/Previous and REDISPLAY/Refresh, carry
         * identical descriptions in both documents, and Bell's words for
         * Bell's own terminal are the better-sourced ones.
         *
         * F9 is shown apart because there is no ninth key on the
         * hardware. The magazine says "The eight function keys" and lists
         * eight; F9 EXIT belongs to Personality+III and to the 1991 VIA
         * recommendation it follows, not to the ALEXTEL.
         */
        static const char *hint[] = {
            "NAPLPS (videotex)",
            "",
            "Connect to a host (Ctrl+O opens a local .nap frame).",
            "",
            "F1 Help     F2 Refresh   F3 Cancel   F4 Top",
            "F5 Previous   F6 Menu   F7 Next   F8 Send",
            "F10 Disconnect",
            "",
            "(F9 Exit is a Personality+III and VIA extension,",
            "not a key the ALEXTEL had.)"
        };
        const int nlines = (int)(sizeof hint / sizeof hint[0]);
        int lh, k, y;
        SIZE ext;

        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        if (g_hFontUI) SelectObject(mem, g_hFontUI);
        GetTextExtentPoint32A(mem, "Wg", 2, &ext);
        lh = ext.cy + 2;
        y  = (ch - nlines * lh) / 2;
        if (!g_connected) {
            /* A1 (2026-11-23): the block starts below the button */
            int tt = w31_power_watermark_text_top(&rc);
            w31_draw_power_watermark(mem, &rc);   /* R1.2 */
            if (tt >= 0) y = tt;
        }
        for (k = 0; k < nlines; k++) {
            RECT lr;
            lr.left = 0; lr.right = cw;
            lr.top = y + k * lh; lr.bottom = lr.top + lh;
            DrawTextA(mem, hint[k], -1, &lr,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);

    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK NapCanvasProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;                       /* the paint owns the whole rect */

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        nap_canvas_paint(hwnd, hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_TIMER:
        if (wParam == IDT_NAP_WAIT) { KillTimer(hwnd, IDT_NAP_WAIT); return 0; }
        break;

    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        /* Navigator 2.0 part 7: a click connects while the "Connect to a
         * host" legend shows (nothing drawn yet: a local frame opened with
         * Ctrl+O keeps its clicks) */
        if (!(g_dec && naplps_ever_drew(g_dec))) w31_connect_click(g_hConnBtn);
        return 0;

    /* A custom-painted child must claim the keys or IsDialogMessage eats
     * them before they arrive. Without this the letters never come. */
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;

    case WM_KEYDOWN:
        /* Ctrl+O opens a local .nap frame. There is no button for it any
         * more (operator, 2026-09-27): local files are for testing. */
        if (wParam == 'O' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            nap_cancel_logoff();         /* keep the opened frame */
            nap_open_file();
            return 0;
        }
        nap_key_down((int)wParam);
        return 0;

    /*
     * F10 arrives as WM_SYSKEYDOWN, not WM_KEYDOWN: it is Windows' menu
     * key. So F10 "Disconnect" (the Bell phone key, see nap_key_down)
     * never reached nap_key_down and did nothing, from Phase 2a until the
     * logoff gate of 2026-09-27 found it. Take it here and swallow it, so
     * it does not also open the window menu.
     */
    case WM_SYSKEYDOWN:
        if (wParam == VK_F10) { nap_key_down(VK_F10); return 0; }
        break;

    default: break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK NapPanelProc(HWND hwnd, UINT msg,
                                     WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC  hdc = (HDC)wParam;
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_w31_br_face);
        return 1;
    }
    /* Navigator 1.5: Windows 3.1 chrome round the videotex screen, which
     * itself stays black, as a videotex terminal is, in a sunken frame. */
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return (LRESULT)w31_ctlcolor(msg, (HDC)wParam, (HWND)lParam);

    /*
     * The controls are children of THIS panel, so their WM_COMMAND
     * arrives here and not at the shared content panel. The content
     * panel does forward WM_COMMAND to the active module, but only for
     * the notifications it receives itself, which are the ones from its
     * own direct children. Leaving this case out gives buttons that
     * draw, highlight on hover and do absolutely nothing, which is the
     * same failure the shell's own tab dispatch has and is just as quiet.
     */
    case WM_COMMAND:
        if (naplps_module_on_command(hwnd, wParam, lParam)) return 0;
        break;

    /* Navigator 2.0: something was pasted into Host; a full address
     * fills the fields (D12). */
    case WM_NAP_PASTED: {
        char t[SITES_HOST_MAX + SITES_PATH_MAX + 32];
        if (g_hHostEdit && !NAP_LINK_BUSY()) {
            GetWindowTextA(g_hHostEdit, t, sizeof t);
            if (strstr(t, "://")) nap_apply_address();
        }
        return 0;
    }

    /* The Minitel function keys are owner-drawn buttons of this panel. */
    case WM_DRAWITEM:
        if (minitel_view_on_drawitem((const DRAWITEMSTRUCT *)lParam)) return TRUE;
        break;

    /* 5 s after a logoff: back to the current format's default screen,
     * unless a session has started since (connect cancels the timer
     * too; this is the second guard). */
    case WM_TIMER:
        if (wParam == IDT_NAP_LOGOFF) {
            KillTimer(hwnd, IDT_NAP_LOGOFF);
            if (!NAP_LINK_BUSY() && !g_connected) {
                nap_default_screen(g_format);
                nap_status("Back to the %s default screen",
                           g_format == FMT_MINITEL ? "Minitel" : "NAPLPS");
            }
            return 0;
        }
        break;

    case WM_NAP_NOTIFY:
        switch (wParam) {
        case TELNET_EVT_CONNECTED: nap_on_connected();                     return 0;
        case TELNET_EVT_DATA:      nap_on_data((TelnetDataChunk *)lParam); return 0;
        case TELNET_EVT_ERROR:     nap_on_error((const char *)lParam);     return 0;
        case TELNET_EVT_CLOSED:    nap_on_closed();                        return 0;
        default: return 0;
        }

    default: break;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

static void nap_register_classes(HINSTANCE hInst)
{
    WNDCLASSEXA wc;

    if (!g_panel_class_reg) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize        = sizeof wc;
        /* the whole panel repaints when its size changes (see the
         * RedrawWindow at the end of naplps_module_resize) */
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = NapPanelProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = g_w31_br_face;
        wc.lpszClassName = NAP_PANEL_CLASS;
        RegisterClassExA(&wc);
        g_panel_class_reg = TRUE;
    }
    if (!g_canvas_class_reg) {
        ZeroMemory(&wc, sizeof wc);
        wc.cbSize        = sizeof wc;
        /* Every pixel of the scaled surface depends on the client size,
         * so a grow must invalidate the whole canvas and not just the
         * newly exposed strips. */
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = NapCanvasProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = NULL;
        wc.lpszClassName = NAP_CANVAS_CLASS;
        RegisterClassExA(&wc);
        g_canvas_class_reg = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Navigator 2.0: the Minitel second row (Site, Protocol, Path)        */
/* ------------------------------------------------------------------ */

/* Write a field for the user: the Site list must not read it as a typed
 * change. Nested calls keep the outer guard. */
static void nap_set_text(HWND h, const char *s)
{
    BOOL was = g_filling;
    if (!h) return;
    g_filling = TRUE;
    SetWindowTextA(h, s);
    g_filling = was;
}

/* The Protocol dropdown and what follows from it. With rewrite_port, a
 * Port field still holding the other protocol's default takes this
 * one's (516 Telnet, 80 WebSocket), so a typed port is never lost. */
static void nap_set_proto(int proto, BOOL rewrite_port)
{
    char port[16];
    g_proto = proto;
    if (g_hProtoCmb) SendMessageA(g_hProtoCmb, CB_SETCURSEL, (WPARAM)proto, 0);
    if (rewrite_port && g_hPortEdit) {
        GetWindowTextA(g_hPortEdit, port, sizeof port);
        if (proto == PROTO_WS && strcmp(port, MINITEL_DEFAULT_PORT_STR) == 0)
            nap_set_text(g_hPortEdit, MINITEL_WS_DEFAULT_PORT_STR);
        else if (proto == PROTO_TELNET && strcmp(port, MINITEL_WS_DEFAULT_PORT_STR) == 0)
            nap_set_text(g_hPortEdit, MINITEL_DEFAULT_PORT_STR);
    }
    if (proto == PROTO_WS && g_hPathEdit && GetWindowTextLengthA(g_hPathEdit) == 0)
        nap_set_text(g_hPathEdit, "/");
    if (g_hPathEdit)
        EnableWindow(g_hPathEdit, !g_connected && !NAP_LINK_BUSY() && proto == PROTO_WS);
}

/* The Site list shows the entry the fields name now, or nothing: an
 * edited host, port, path or protocol is no longer that site. */
static void nap_site_sync(void)
{
    char host[256], port[16], path[SITES_PATH_MAX];
    int n, i, sel = -1;
    const suite_site_t *t;

    if (!g_hSiteCmb || g_filling) return;
    GetWindowTextA(g_hHostEdit, host, sizeof host);
    GetWindowTextA(g_hPortEdit, port, sizeof port);
    GetWindowTextA(g_hPathEdit, path, sizeof path);
    t = suite_sites_table(SITES_TAB_MINITEL, &n);
    for (i = 0; t && i < n; i++) {
        suite_site_addr_t a;
        int proto;
        if (suite_sites_parse(t[i].address, &a) != SITES_OK) continue;
        proto = (a.scheme == SITES_SCHEME_WS) ? PROTO_WS : PROTO_TELNET;
        if (proto != g_proto) continue;
        if (_stricmp(a.host, host) != 0 || a.port != atoi(port)) continue;
        if (proto == PROTO_WS
            && strcmp(a.path[0] ? a.path : "/", path[0] ? path : "/") != 0) continue;
        sel = i;
        break;
    }
    if ((int)SendMessageA(g_hSiteCmb, CB_GETCURSEL, 0, 0) != sel)
        SendMessageA(g_hSiteCmb, CB_SETCURSEL, (WPARAM)sel, 0);
}

/* Fill Protocol, Host, Port and Path from a telnet:// or ws:// address. */
static void nap_fill_from_addr(const suite_site_addr_t *a)
{
    char port[16];
    BOOL was = g_filling;
    g_filling = TRUE;
    nap_set_proto(a->scheme == SITES_SCHEME_WS ? PROTO_WS : PROTO_TELNET, FALSE);
    SetWindowTextA(g_hHostEdit, a->host);
    _snprintf(port, sizeof port - 1, "%d", a->port);
    port[sizeof port - 1] = '\0';
    SetWindowTextA(g_hPortEdit, port);
    SetWindowTextA(g_hPathEdit, a->scheme == SITES_SCHEME_WS
                                ? (a->path[0] ? a->path : "/") : "");
    g_filling = was;
    nap_set_proto(g_proto, FALSE);       /* Path's enabled state */
    nap_site_sync();
}

/*
 * The Host field holds a full address, pasted or typed (D12): act on it.
 * TRUE when the fields can be used to connect (a plain host name, or an
 * address that has filled them); FALSE when it is refused, in which case
 * nothing is filled, nothing connects and the status line says why.
 */
static BOOL nap_apply_address(void)
{
    char text[SITES_HOST_MAX + SITES_PATH_MAX + 32];
    suite_site_addr_t a;
    int rc;

    GetWindowTextA(g_hHostEdit, text, sizeof text);
    rc = suite_sites_parse(text, &a);
    if (rc == SITES_NOT_ADDRESS) return TRUE;
    /* wss only: since 2026-11-25 ircs and snews are also UNSUPPORTED, and
     * they fall to the general "takes a host name" text below, as before. */
    if (rc == SITES_UNSUPPORTED && a.scheme == SITES_SCHEME_WSS) {
        nap_status("Secure WebSocket (wss://) connections are not supported "
                   "in this version of Teletext Navigator.");
        return FALSE;
    }
    if (rc != SITES_OK
        || (a.scheme != SITES_SCHEME_TELNET && a.scheme != SITES_SCHEME_WS)) {
        nap_status("The Host box takes a host name, or a telnet:// or ws:// address.");
        return FALSE;
    }
    if (g_format == FMT_NAPLPS) {
        char port[16];
        if (a.scheme == SITES_SCHEME_WS) {
            nap_status("WebSocket is for the Minitel format; NAPLPS connects by Telnet.");
            return FALSE;
        }
        nap_set_text(g_hHostEdit, a.host);
        _snprintf(port, sizeof port - 1, "%d", a.port);
        port[sizeof port - 1] = '\0';
        nap_set_text(g_hPortEdit, port);
        nap_status("Address filled in: Telnet, %s port %d.", a.host, a.port);
        return TRUE;
    }
    nap_fill_from_addr(&a);
    if (a.path_rewritten)
        nap_status("Address filled in. The path /url= was changed to /?url=, "
                   "the form the gateway answers.");
    else if (a.scheme == SITES_SCHEME_WS)
        nap_status("Address filled in: WebSocket (ws), %s port %d, path %s.",
                   a.host, a.port, a.path[0] ? a.path : "/");
    else
        nap_status("Address filled in: Telnet, %s port %d.", a.host, a.port);
    return TRUE;
}

/* A site picked from the list: fill the fields, never connect (D5). */
static void nap_pick_site(int idx)
{
    int n;
    const suite_site_t *t = suite_sites_table(SITES_TAB_MINITEL, &n);
    suite_site_addr_t a;

    if (!t || idx < 0 || idx >= n) return;
    if (suite_sites_parse(t[idx].address, &a) != SITES_OK) return;
    nap_fill_from_addr(&a);
    SendMessageA(g_hSiteCmb, CB_SETCURSEL, (WPARAM)idx, 0);
    nap_status("%s: %s. CONNEXION/FIN (F9) connects.", t[idx].label, t[idx].address);
}

/* The Host field: after a paste (the context menu's WM_PASTE, Ctrl+V,
 * Shift+Insert), ask the panel to look for a full address. Posted, so
 * the field already holds the pasted text when the panel reads it. */
static LRESULT CALLBACK NapHostProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    LRESULT r = CallWindowProcA(g_host_old_proc, h, m, wp, lp);
    if (m == WM_PASTE
        || (m == WM_CHAR && wp == 0x16)
        || (m == WM_KEYDOWN && wp == VK_INSERT && (GetKeyState(VK_SHIFT) & 0x8000)))
        PostMessageA(g_hPanel, WM_NAP_PASTED, 0, 0);
    return r;
}

/* ------------------------------------------------------------------ */
/* Connecting, and the format                                          */
/* ------------------------------------------------------------------ */

/*
 * One connect path for both formats and for both ways in: the Connect
 * button, and Minitel's CONNEXION/FIN pressed while disconnected. The
 * NAPLPS branch is the Phase 2a code as it was. Navigator 2.0: a full
 * address in Host is applied first, and Minitel over WebSocket opens
 * ws_proto instead of telnet_proto; everything after the open is shared.
 */
static void nap_connect(void)
{
    char host[256], port[16], path[SITES_PATH_MAX];
    int  p;
    BOOL use_ws, rewrote = FALSE;

    if (NAP_LINK_BUSY()) return;   /* already connecting or connected */
    if (!nap_apply_address()) return;

    GetWindowTextA(g_hHostEdit, host, sizeof host);
    GetWindowTextA(g_hPortEdit, port, sizeof port);
    if (host[0] == '\0') { nap_status("Enter a host name"); return; }
    p = atoi(port);
    if (p <= 0 || p > 65535) { nap_status("Enter a port from 1 to 65535"); return; }

    use_ws = (g_format == FMT_MINITEL && g_proto == PROTO_WS);
    path[0] = '\0';
    if (use_ws) {
        /* The path goes exactly as entered (a service URL inside it is
         * case-sensitive), with a leading '/' if it lacks one, and the
         * gateway's "/url=" form rewritten as a pasted address is. */
        GetWindowTextA(g_hPathEdit, path + 1, sizeof path - 1);
        if (path[1] == '/') memmove(path, path + 1, strlen(path + 1) + 1);
        else path[0] = '/';
        if (strncmp(path, "/url=", 5) == 0 && strlen(path) + 1 < sizeof path) {
            memmove(path + 2, path + 1, strlen(path + 1) + 1);
            path[1] = '?';
            rewrote = TRUE;
        }
        nap_set_text(g_hPathEdit, path);
    }

    _snprintf(g_active_host, sizeof g_active_host - 1, "%s", host);
    g_active_host[sizeof g_active_host - 1] = '\0';
    g_active_port = p;
    g_session_format = g_format;

    /* Connecting again inside the 5 s after a logoff cancels its timer,
     * so the new session's screen is never wiped under it. */
    nap_cancel_logoff();
    g_rx_since_tx = FALSE;

    /* A new session is a new presentation: start from the power-on
     * default state so nothing leaks in from the previous one. */
    if (g_session_format == FMT_MINITEL) {
        minitel_view_reset();
    } else {
        if (g_dec) naplps_reset(g_dec);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
    }

    if (use_ws) {
        nap_status("Connecting to ws://%s:%d%s ...%s", g_active_host, g_active_port,
                   path, rewrote ? " (the path /url= was changed to /?url=)" : "");
        if (ws_proto_open(g_active_host, g_active_port, path,
                          g_hPanel, WM_NAP_NOTIFY, &g_ws) != 0 || !g_ws) {
            g_ws = NULL;
            nap_status("Cannot start a connection to ws://%s:%d",
                       g_active_host, g_active_port);
            nap_set_session_ui(FALSE);
            return;
        }
    } else {
        nap_status("Connecting to %s:%d ...", g_active_host, g_active_port);
        if (telnet_proto_open(g_active_host, g_active_port,
                              g_hPanel, WM_NAP_NOTIFY, &g_telnet) != 0
            || !g_telnet) {
            nap_status("Cannot start a connection to %s:%d",
                       g_active_host, g_active_port);
            nap_set_session_ui(FALSE);
            return;
        }
    }
    /* The second row stays put while a session is starting. */
    EnableWindow(g_hSiteCmb,  FALSE);
    EnableWindow(g_hProtoCmb, FALSE);
    EnableWindow(g_hPathEdit, FALSE);
    /* Until TELNET_EVT_CONNECTED the toggle reads Disconnect, grayed: the
     * EXCEPTION to cancel-while-connecting (2026-11-11), the same
     * telnet_proto handshake hazard as RIPscrip
     * (docs/2026-11-11_W31_BUTTONS.md, finding). It goes live on CONNECTED
     * and back to Connect on ERROR/CLOSED via nap_set_session_ui. */
    w31_toggle_busy(g_hConnBtn);
}

/* Show the screen of the chosen format and set the controls to match. */
static void nap_apply_format(void)
{
    BOOL mtl = (g_format == FMT_MINITEL);
    int  sw  = mtl ? SW_SHOW : SW_HIDE;

    ShowWindow(g_hCanvas, mtl ? SW_HIDE : SW_SHOW);
    minitel_view_show(mtl);
    /* Minitel has no Connect or Disconnect: CONNEXION/FIN does both, as
     * on the terminal. NAPLPS keeps them as they were. */
    ShowWindow(g_hConnBtn, mtl ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hDiscBtn, mtl ? SW_HIDE : SW_SHOW);
    ShowWindow(g_hScanChk, mtl ? SW_SHOW : SW_HIDE);
    /* Navigator 2.0: the second row is Minitel's only. */
    ShowWindow(g_hSiteLbl,  sw);
    ShowWindow(g_hSiteCmb,  sw);
    ShowWindow(g_hProtoLbl, sw);
    ShowWindow(g_hProtoCmb, sw);
    ShowWindow(g_hPathLbl,  sw);
    ShowWindow(g_hPathEdit, sw);
    SendMessageA(g_hMonoChk, BM_SETCHECK,
                 (mtl ? g_mono_mtl : g_mono) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageA(g_hScanChk, BM_SETCHECK, g_scan_mtl ? BST_CHECKED : BST_UNCHECKED, 0);
    minitel_view_set_mono(g_mono_mtl);
    minitel_view_set_scanlines(g_scan_mtl);
    /* The top row differs by format, so lay the tab out again. */
    if (g_hContent) {
        RECT rc;
        GetClientRect(g_hContent, &rc);
        naplps_module_resize(g_hContent, rc.right - rc.left, rc.bottom - rc.top);
    }
    if (mtl) minitel_view_focus();
    else if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
}

/* What the Minitel view asks of the tab. */
static void mtl_host_send(const unsigned char *buf, size_t len)
{
    if (g_session_format == FMT_MINITEL) nap_send(buf, (int)len);
}

static void mtl_host_connect(void)
{
    nap_connect();
}

static void mtl_host_disconnect(void)
{
    nap_end_session(WHY_LOCAL);
    nap_status("Disconnected (CONNEXION/FIN twice)");
}

static void mtl_host_status(const char *text)
{
    if (g_hStatus) SetWindowTextA(g_hStatus, text);
}

static void mtl_host_file_opened(void)
{
    nap_cancel_logoff();                 /* keep the opened page */
}

/* ------------------------------------------------------------------ */
/* Module entry points                                                 */
/* ------------------------------------------------------------------ */

/* A checkbox's width from its label in the font it uses: the box, the
 * gap after it, the text, and a few pixels so the last letter is never
 * the one that gets cropped. An empty label gives the box alone. */
static int nap_check_w(HWND chk, const char *label)
{
    HDC hdc;
    HGDIOBJ old = NULL;
    HFONT f;
    SIZE sz = { 0, 0 };
    int box = GetSystemMetrics(SM_CXMENUCHECK);

    if (!label[0]) return box + 4;
    hdc = GetDC(chk);
    f = (HFONT)SendMessageA(chk, WM_GETFONT, 0, 0);
    if (f) old = SelectObject(hdc, f);
    GetTextExtentPoint32A(hdc, label, (int)strlen(label), &sz);
    if (old) SelectObject(hdc, old);
    ReleaseDC(chk, hdc);
    return box + 6 + sz.cx + 6;
}

/* Scanlines with or without its label (narrow windows, see the resize). */
static void nap_set_scan_label(BOOL on)
{
    static int shown = -1;
    if (shown == (int)on) return;
    shown = (int)on;
    SetWindowTextA(g_hScanChk, on ? "Scanlines" : "");
}

void naplps_module_resize(HWND content, int w, int h)
{
    const int margin = 16;
    const int row_h  = 26;
    const int btn_h  = 30;
    int canvas_y, status_h = 20;

    (void)content;
    if (!g_controls_created || !g_hPanel) return;

    MoveWindow(g_hPanel, 0, 0, w, h, TRUE);

    /* Format first, then the address it will use. Minitel mode narrows
     * the Host field by 30 pixels to make room for its extra controls, so
     * the row fits at Navigator's default 980-pixel window. */
    {
        const int host_w = 200;
        const int port_lx = margin + 217 + host_w;
        MoveWindow(g_hFmtLbl,   margin,       18, 55,     22,    TRUE);
        MoveWindow(g_hFmtCmb,   margin + 58,  14, 95,     200,   TRUE);
        MoveWindow(g_hHostLbl,  margin + 163, 18, 40,     22,    TRUE);
        MoveWindow(g_hHostEdit, margin + 207, 14, host_w, row_h, TRUE);
        MoveWindow(g_hPortLbl,  port_lx,      18, 35,     22,    TRUE);
        MoveWindow(g_hPortEdit, port_lx + 38, 14, 55,     row_h, TRUE);
    }
    if (g_format == FMT_MINITEL) {
        /* Monochrome and Scanlines end at the screen box's right edge, so
         * the long light-red CONNEXION/FIN key, above the key column,
         * stands apart (operator, 2026-10-20). Only when the window is so
         * narrow that the Format/Host/Port fields leave no room does the
         * left clamp push them further right. */
        /*
         * CONNEXION/FIN is centred over the two function-key columns
         * below it (operator, 2026-10-20). The checkbox widths are measured
         * from their labels, so "Scanlines" is never cropped.
         *
         * Nothing may overlap. When the row runs short (Navigator's
         * minimum is 800 pixels), it gives way in this order: the
         * checkboxes leave the screen box's edge and move right; then
         * CONNEXION/FIN slides right, at most to the key columns' right
         * edge; then Scanlines drops its label and keeps its box; then
         * Scanlines is hidden; last, at the very narrowest, CONNEXION/FIN
         * narrows toward its own label's width. Monochrome never shrinks.
         */
        const int gap = 8, min_x = margin + 519;
        int cnx_w = 128;
        int span_l, span_w, cnx_x, cnx_max, screen_r, room;
        int mono_w = nap_check_w(g_hMonoChk, "Monochrome");
        int scan_full = nap_check_w(g_hScanChk, "Scanlines");
        int scan_box = nap_check_w(g_hScanChk, "");
        int scan_w, mono_x, scan_x;
        BOOL scan_label = TRUE, scan_show = TRUE;

        minitel_view_key_span(w - 2 * margin, &span_l, &span_w);
        cnx_x   = margin + span_l + (span_w - cnx_w) / 2;
        cnx_max = margin + span_l + span_w - cnx_w;
        screen_r = margin + minitel_view_screen_width(w - 2 * margin);

        /* the room the checkboxes need, right of the Port field */
        room = mono_w + gap + scan_full;
        if (min_x + room > cnx_x - gap) {
            cnx_x = min_x + room + gap;
            if (cnx_x > cnx_max) {
                cnx_x = cnx_max;
                if (min_x + mono_w + gap + scan_box <= cnx_x - gap) scan_label = FALSE;
                else scan_show = FALSE;
                if (!scan_show && min_x + mono_w + gap > cnx_x) {
                    /* the label measured as a checkbox's, less its box */
                    int cnx_min = nap_check_w(g_hMonoChk, "CONNEXION/FIN")
                                - GetSystemMetrics(SM_CXMENUCHECK);
                    int right = cnx_x + cnx_w;
                    cnx_w = right - (min_x + mono_w + gap);
                    if (cnx_w < cnx_min) cnx_w = cnx_min;
                    cnx_x = right - cnx_w;
                }
            }
        }
        scan_w = scan_label ? scan_full : scan_box;

        /* right-aligned to the screen box when there is room, else as far
         * right as CONNEXION/FIN allows, never left of the Port field */
        scan_x = screen_r - scan_w;
        if (scan_x + scan_w > cnx_x - gap) scan_x = cnx_x - gap - scan_w;
        mono_x = (scan_show ? scan_x - gap : scan_x + scan_w) - mono_w;
        if (mono_x < min_x) {
            mono_x = min_x;
            scan_x = mono_x + mono_w + gap;
        }

        nap_set_scan_label(scan_label);
        ShowWindow(g_hScanChk, scan_show ? SW_SHOW : SW_HIDE);
        MoveWindow(g_hMonoChk, mono_x, 16, mono_w, 22, TRUE);
        MoveWindow(g_hScanChk, scan_x, 16, scan_w, 22, TRUE);
        minitel_view_place_cnx(cnx_x, 12, cnx_w, btn_h);

        /* Navigator 2.0: the second row, on row one's columns: Site under
         * Format, Protocol ending where Host ends, Path starting under
         * Port and running to CONNEXION/FIN's right edge. */
        {
            const int r2 = 48;
            const int path_x = margin + 455;
            int path_w = cnx_x + cnx_w - path_x;
            if (path_w < 120) path_w = 120;
            MoveWindow(g_hSiteLbl,  margin,       r2 + 4, 40,  22,  TRUE);
            MoveWindow(g_hSiteCmb,  margin + 58,  r2,     140, 200, TRUE);
            MoveWindow(g_hProtoLbl, margin + 207, r2 + 4, 62,  22,  TRUE);
            MoveWindow(g_hProtoCmb, margin + 272, r2,     135, 200, TRUE);
            MoveWindow(g_hPathLbl,  margin + 417, r2 + 4, 35,  22,  TRUE);
            MoveWindow(g_hPathEdit, path_x,       r2,     path_w, row_h, TRUE);
        }
    } else {
        /* NAPLPS: the Phase 2a row, with the checkbox label shortened to
         * "Monochrome" so it is never clipped. */
        const int mono_w = 105;
        const int gap = 8;
        /* The Connect/Disconnect toggle at the right end, centred on the
         * row the 30 px buttons used (y 12..42); Monochrome before it. */
        int conn_x = w - margin - W31_TOGGLE_W;
        int mono_x = conn_x - gap - mono_w;
        if (mono_x < margin + 519) mono_x = margin + 519;
        MoveWindow(g_hMonoChk, mono_x, 16, mono_w, 22,    TRUE);
        MoveWindow(g_hConnBtn, conn_x, 12 + (btn_h - W31_TOGGLE_H) / 2,
                   W31_TOGGLE_W, W31_TOGGLE_H, TRUE);
    }

    if (g_format == FMT_MINITEL) {
        /*
         * Minitel mode shares the area between its screen and its
         * keyboard, and takes 12 pixels less chrome than NAPLPS (a tighter
         * top and bottom): at Navigator's default 980 x 770 window that is
         * the difference between the 320 x 250 screen at 2x and at 1x.
         * Navigator 2.0: the second strip row (Site, Protocol, Path, at
         * y 48) moves the screen down to 82.
         */
        const int m_y = 82, m_bottom = 8;
        MoveWindow(g_hStatus, margin, h - m_bottom - status_h,
                   w - 2 * margin, status_h, TRUE);
        minitel_view_layout(margin, m_y, w - 2 * margin,
                            h - m_y - m_bottom - status_h - 4);
    } else {
        canvas_y = 52;
        MoveWindow(g_hCanvas, margin, canvas_y,
                   w - 2 * margin,
                   h - canvas_y - margin - status_h - 4, TRUE);
        MoveWindow(g_hStatus, margin, h - margin - status_h,
                   w - 2 * margin, status_h, TRUE);
    }

    /*
     * Repaint every child now. A child that MOVES without changing size
     * is not repainted: Windows copies its old pixels to the new place,
     * and during a drag those pixels can already be stale, which left
     * ghost letters on the Minitel keys and a garbled "Scanlines" (the
     * same remedy fixed the RIPscrip panel).
     */
    RedrawWindow(g_hPanel, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);

    if (g_hCanvas) {
        InvalidateRect(g_hCanvas, NULL, FALSE);
        UpdateWindow(g_hCanvas);
    }
}

void naplps_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    g_hContent = content;

    /* Re-activation preserves the session and the rendered frame: the
     * panel is hidden on deactivate, never destroyed. */
    if (g_controls_created) {
        ShowWindow(g_hPanel, SW_SHOW);
        suite_set_status("");        /* the tab's own line is the one */
        GetClientRect(content, &rc);
        naplps_module_resize(content, rc.right - rc.left,
                             rc.bottom - rc.top);
        /* Minitel's screen takes the focus so its keys, CONNEXION/FIN
         * (F9) included, work at once. */
        if (g_format == FMT_MINITEL) minitel_view_focus();
        else if (!g_connected) SetFocus(g_hHostEdit);
        return;
    }

    nap_register_classes(hInst);

    GetClientRect(content, &rc);
    /* No WS_EX_CONTROLPARENT: the shared content panel is not a control
     * parent, and the combination hangs IsDialogMessage. That cost the
     * Archie tab an afternoon and the note is worth carrying. */
    g_hPanel = CreateWindowExA(0, NAP_PANEL_CLASS, "",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        0, 0, rc.right - rc.left, rc.bottom - rc.top,
        content, NULL, hInst, NULL);

    g_hFmtLbl = CreateWindowA("STATIC", "Format:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 55, 22, g_hPanel, NULL, hInst, NULL);
    g_hFmtCmb = CreateWindowA("COMBOBOX", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 95, 200, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_FORMAT_CMB, hInst, NULL);
    SendMessageA(g_hFmtCmb, CB_ADDSTRING, 0, (LPARAM)"NAPLPS");
    SendMessageA(g_hFmtCmb, CB_ADDSTRING, 0, (LPARAM)"Minitel");
    SendMessageA(g_hFmtCmb, CB_SETCURSEL, g_format, 0);

    g_hHostLbl = CreateWindowA("STATIC", "Host:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 40, 22, g_hPanel, NULL, hInst, NULL);
    g_hHostEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        g_format == FMT_MINITEL ? MINITEL_DEFAULT_HOST : NAPLPS_DEFAULT_HOST,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 26, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_HOST_EDIT, hInst, NULL);
    g_hPortLbl = CreateWindowA("STATIC", "Port:",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 35, 22, g_hPanel, NULL, hInst, NULL);
    g_hPortEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        g_format == FMT_MINITEL ? MINITEL_DEFAULT_PORT_STR : NAPLPS_DEFAULT_PORT_STR,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 55, 26, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_PORT_EDIT, hInst, NULL);
    g_hMonoChk = CreateWindowA("BUTTON", "Monochrome",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 105, 22, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_MONO_CHK, hInst, NULL);
    g_hScanChk = CreateWindowA("BUTTON", "Scanlines",
        WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,
        0, 0, 92, 22, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_SCAN_CHK, hInst, NULL);
    g_hConnBtn = CreateWindowA("BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, W31_TOGGLE_W, W31_TOGGLE_H, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_CONNECT_BTN, hInst, NULL);
    g_hDiscBtn = NULL;   /* merged into the Connect toggle, 2026-11-11 */
    /* Navigator 2.0 part 7 (also in Minitel format: the hidden toggle's
     * command is the same nap_connect the CONNEXION key reaches) */
    w31_connect_on_enter(g_hHostEdit, g_hConnBtn);
    w31_connect_on_enter(g_hPortEdit, g_hConnBtn);

    /* Navigator 2.0: the Minitel second row (shown by nap_apply_format in
     * Minitel format only). Site and Protocol are drop-down LISTS, which
     * take the Windows 3.1 look from suite_w31 by themselves. */
    g_hSiteLbl = CreateWindowA("STATIC", "Site:",
        WS_CHILD | SS_LEFTNOWORDWRAP,
        0, 0, 40, 22, g_hPanel, NULL, hInst, NULL);
    g_hSiteCmb = CreateWindowA("COMBOBOX", "",
        WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 140, 200, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_SITE_CMB, hInst, NULL);
    suite_sites_fill_combo(g_hSiteCmb, SITES_TAB_MINITEL);
    g_hProtoLbl = CreateWindowA("STATIC", "Protocol:",
        WS_CHILD | SS_LEFTNOWORDWRAP,
        0, 0, 62, 22, g_hPanel, NULL, hInst, NULL);
    g_hProtoCmb = CreateWindowA("COMBOBOX", "",
        WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
        0, 0, 135, 200, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_PROTO_CMB, hInst, NULL);
    SendMessageA(g_hProtoCmb, CB_ADDSTRING, 0, (LPARAM)"Telnet");          /* PROTO_TELNET */
    SendMessageA(g_hProtoCmb, CB_ADDSTRING, 0, (LPARAM)"WebSocket (ws)");  /* PROTO_WS */
    SendMessageA(g_hProtoCmb, CB_SETCURSEL, (WPARAM)g_proto, 0);
    g_hPathLbl = CreateWindowA("STATIC", "Path:",
        WS_CHILD | SS_LEFTNOWORDWRAP,
        0, 0, 35, 22, g_hPanel, NULL, hInst, NULL);
    /* No ES_UPPERCASE / ES_LOWERCASE: the path goes exactly as entered. */
    g_hPathEdit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 26, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_PATH_EDIT, hInst, NULL);
    w31_connect_on_enter(g_hPathEdit, g_hConnBtn);
    /* Host: a paste of a full address fills the fields (D12). */
    g_host_old_proc = (WNDPROC)SetWindowLongPtrA(g_hHostEdit, GWLP_WNDPROC,
                                                 (LONG_PTR)NapHostProc);

    g_hCanvas = CreateWindowExA(WS_EX_CLIENTEDGE, NAP_CANVAS_CLASS, "",
        WS_CHILD | WS_VISIBLE,
        0, 0, 100, 100, g_hPanel,
        (HMENU)(INT_PTR)IDC_NAP_CANVAS, hInst, NULL);

    g_hStatus = CreateWindowA("STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 100, 20, g_hPanel, NULL, hInst, NULL);

    if (g_hFontUI) {
        HWND all[] = { g_hHostLbl, g_hHostEdit, g_hPortLbl, g_hPortEdit,
                       g_hMonoChk, g_hConnBtn, g_hDiscBtn,
                       g_hStatus, g_hFmtLbl, g_hFmtCmb, g_hScanChk,
                       g_hSiteLbl, g_hSiteCmb, g_hProtoLbl, g_hProtoCmb,
                       g_hPathLbl, g_hPathEdit };
        int k;
        for (k = 0; k < (int)(sizeof all / sizeof all[0]); k++)
            SendMessageA(all[k], WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }

    if (!g_dec) g_dec = naplps_create();
    nap_dib_create();
    nap_dib_refresh();

    {
        minitel_view_host_t host;
        host.send       = mtl_host_send;
        host.connect    = mtl_host_connect;
        host.disconnect = mtl_host_disconnect;
        host.status     = mtl_host_status;
        host.file_opened = mtl_host_file_opened;
        minitel_view_create(g_hPanel, hInst, &host);
    }

    g_controls_created = TRUE;
    nap_set_session_ui(FALSE);
    nap_apply_format();
    nap_set_proto(g_proto, FALSE);
    nap_site_sync();                 /* the defaults are "The Teletext" */
    suite_set_status("");            /* the tab's own line is the one */

    GetClientRect(content, &rc);
    naplps_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);
    if (g_format == FMT_MINITEL) minitel_view_focus();
    else SetFocus(g_hHostEdit);
}

void naplps_module_deactivate(HWND content)
{
    (void)content;
    /* Hide, never destroy: the session and the frame survive a tab
     * switch, matching every other Navigator module. */
    if (g_hPanel) ShowWindow(g_hPanel, SW_HIDE);
}

BOOL naplps_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id = LOWORD(wParam);
    (void)content;
    (void)lParam;

    /* The on-screen Minitel keyboard's buttons are children of this
     * panel too, so their clicks come through here. */
    if (minitel_view_on_command(wParam)) return TRUE;

    switch (id) {
    case IDC_NAP_CONNECT_BTN:       /* the toggle: its label says which */
        if (g_connected) {
            nap_end_session(WHY_LOCAL);
            nap_status("Disconnected");
        } else if (!NAP_LINK_BUSY()) {
            nap_connect();               /* mid-handshake it is grayed */
        }
        return TRUE;

    case IDC_NAP_FORMAT_CMB:
        /*
         * Act on the choice only once the list is CLOSED: on CBN_CLOSEUP,
         * or on a selection change made with the list closed. With the
         * list open, an arrow key already fires CBN_SELCHANGE, and moving
         * the focus to the screen at that moment cancels the drop-down,
         * which puts the old selection back: the switch undid itself
         * (found by the logoff gate, case N6, 2026-09-27).
         */
        if ((HIWORD(wParam) == CBN_SELCHANGE
             && !SendMessageA(g_hFmtCmb, CB_GETDROPPEDSTATE, 0, 0))
            || HIWORD(wParam) == CBN_CLOSEUP) {
            int sel = (int)SendMessageA(g_hFmtCmb, CB_GETCURSEL, 0, 0);
            if (sel == g_format) return TRUE;      /* nothing changed */
            if (g_connected || NAP_LINK_BUSY()) {
                /* Never while a session is live; the combo is disabled
                 * then, and this is the belt to that brace. */
                SendMessageA(g_hFmtCmb, CB_SETCURSEL, g_format, 0);
                return TRUE;
            }
            if (sel == FMT_NAPLPS || sel == FMT_MINITEL) {
                char hs[256];
                g_format = sel;
                /* Navigator 2.0 (D9): one default host per format. Host is
                 * rewritten only while it still holds the OTHER format's
                 * default, so a typed host is never lost. Both formats
                 * start on Telnet (NAPLPS has no other), on the format's
                 * Telnet port as before. */
                GetWindowTextA(g_hHostEdit, hs, sizeof hs);
                if (_stricmp(hs, sel == FMT_MINITEL ? NAPLPS_DEFAULT_HOST
                                                    : MINITEL_DEFAULT_HOST) == 0)
                    nap_set_text(g_hHostEdit, sel == FMT_MINITEL ? MINITEL_DEFAULT_HOST
                                                                 : NAPLPS_DEFAULT_HOST);
                nap_set_proto(PROTO_TELNET, FALSE);
                nap_set_text(g_hPortEdit, sel == FMT_MINITEL
                               ? MINITEL_DEFAULT_PORT_STR
                               : NAPLPS_DEFAULT_PORT_STR);
                nap_site_sync();
                /* The new format opens on its DEFAULT screen, at once,
                 * and any pending logoff timer is cancelled. */
                nap_cancel_logoff();
                nap_default_screen(sel);
                nap_apply_format();
                /* Leave the keyboard on the screen, not on the dropdown. */
                if (sel == FMT_MINITEL) minitel_view_focus();
                else if (g_hCanvas) SetFocus(g_hCanvas);
                nap_status(sel == FMT_MINITEL
                    ? "Minitel: port %d, in colour; F9 or "
                      "CONNEXION/FIN connects"
                    : "NAPLPS: port %d",
                    sel == FMT_MINITEL ? MINITEL_DEFAULT_PORT : NAPLPS_DEFAULT_PORT);
            }
        }
        return TRUE;

    case IDC_NAP_DISCONN_BTN:
        nap_end_session(WHY_LOCAL);
        nap_status("Disconnected");
        return TRUE;

    /* Navigator 2.0: the Minitel second row and the address fields. */
    case IDC_NAP_HOST_EDIT:
        if (HIWORD(wParam) == EN_CHANGE) {
            nap_site_sync();
        } else if (HIWORD(wParam) == EN_KILLFOCUS && !NAP_LINK_BUSY()) {
            /* A full address typed, not pasted: act on it on leaving. */
            char t[SITES_HOST_MAX + SITES_PATH_MAX + 32];
            GetWindowTextA(g_hHostEdit, t, sizeof t);
            if (strstr(t, "://")) nap_apply_address();
        }
        return TRUE;

    case IDC_NAP_PORT_EDIT:
    case IDC_NAP_PATH_EDIT:
        if (HIWORD(wParam) == EN_CHANGE) nap_site_sync();
        return TRUE;

    case IDC_NAP_SITE_CMB:
    case IDC_NAP_PROTO_CMB:
        /* As for Format: act once the list is closed (see above). */
        if ((HIWORD(wParam) == CBN_SELCHANGE
             && !SendMessageA((HWND)lParam, CB_GETDROPPEDSTATE, 0, 0))
            || HIWORD(wParam) == CBN_CLOSEUP) {
            int sel = (int)SendMessageA((HWND)lParam, CB_GETCURSEL, 0, 0);
            if (NAP_LINK_BUSY() || g_connected) return TRUE;
            if (id == IDC_NAP_SITE_CMB) {
                /* Fill only; CONNEXION/FIN connects (D5). The keyboard
                 * goes back to the screen so F9 works at once. */
                if (sel >= 0) { nap_pick_site(sel); minitel_view_focus(); }
            } else if (sel == PROTO_TELNET || sel == PROTO_WS) {
                if (sel != g_proto) {
                    char port[16];
                    nap_set_proto(sel, TRUE);
                    nap_site_sync();
                    GetWindowTextA(g_hPortEdit, port, sizeof port);
                    nap_status(sel == PROTO_WS
                        ? "Protocol: WebSocket (ws), port %s. Secure WebSocket (wss) is not supported in this version."
                        : "Protocol: Telnet, port %s.", port);
                }
            }
        }
        return TRUE;

    case IDC_NAP_SCAN_CHK:
        /* CRT scan lines, Minitel only, default on; reproduced from
         * MiniPavi's emulator by observation (minitel_screen.c). */
        g_scan_mtl = (SendMessageA(g_hScanChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
        minitel_view_set_scanlines(g_scan_mtl);
        nap_status(g_scan_mtl ? "Scan lines on" : "Scan lines off");
        minitel_view_focus();
        return TRUE;

    case IDC_NAP_MONO_CHK:
        if (g_format == FMT_MINITEL) {
            /* STUM1B p32's grey table, "le tube cathodique du Minitel
             * etant en noir et blanc". ON by default for Minitel. */
            g_mono_mtl = (SendMessageA(g_hMonoChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
            minitel_view_set_mono(g_mono_mtl);
            nap_status(g_mono_mtl
                ? "Monochrome view: STUM1B's grey levels, as the Minitel 1B tube"
                : "Colour view, as a colour Minitel or an emulator");
            minitel_view_focus();
            return TRUE;
        }
        /*
         * The monochrome view, and it is not decoration.
         *
         * The only confirmed real target for this format is a MONOCHROME
         * terminal: a dealer listing for a 1991 AlexTel describes it as a
         * monochrome terminal with a built-in 1200 baud modem. The default
         * colour map's first half is a grey ramp and its second half is
         * eight full-saturation hues (5.3.2.5.2), and the hues do NOT
         * collapse to distinct or ordered luminances. So a frame that
         * carries meaning in hue loses that meaning on the hardware it was
         * built for, and there is no way to notice without either owning
         * the terminal or rendering by luminance.
         *
         * This toggle is the cheap substitute for owning one.
         */
        g_mono = (SendMessageA(g_hMonoChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
        nap_dib_refresh();
        if (g_hCanvas) InvalidateRect(g_hCanvas, NULL, FALSE);
        nap_status(g_mono
            ? "Monochrome view: rendering by luminance, as a monochrome "
              "terminal would show it"
            : "Colour view");
        return TRUE;

    default: break;
    }
    return FALSE;
}

BOOL naplps_module_has_unsaved(void)
{
    /* A rendered frame is not unsaved work: there is nothing the operator
     * could lose by switching tabs, because the tab is hidden rather than
     * destroyed and the frame is still there on return. */
    return FALSE;
}

void naplps_module_shutdown(void)
{
    if (g_telnet) { telnet_proto_close(g_telnet); g_telnet = NULL; }
    if (g_ws)     { ws_proto_close(g_ws);         g_ws = NULL; }
    g_connected = FALSE;
    minitel_view_destroy();
    if (g_dec) { naplps_destroy(g_dec); g_dec = NULL; }
    if (g_hMemDC) {
        if (g_hOldDib) SelectObject(g_hMemDC, g_hOldDib);
        DeleteDC(g_hMemDC);
        g_hMemDC = NULL;
    }
    if (g_hDib) { DeleteObject(g_hDib); g_hDib = NULL; g_dib_bits = NULL; }
}
