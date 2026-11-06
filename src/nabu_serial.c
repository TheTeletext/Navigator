/*
 * nabu_serial.c - the serial leg. See nabu_serial.h for what and why.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "nabu_serial.h"
#include "nabu_channel.h"

/*
 * The per-socket keepalive and retransmission options, Windows 10 1709
 * and later. Defined here when the toolchain's headers predate them; the
 * numbers are the documented IPPROTO_TCP option values.
 */
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE   3      /* a.k.a. TCP_KEEPALIVE */
#endif
#ifndef TCP_MAXRT
#define TCP_MAXRT      5
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT   16
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 17
#endif

/* One session. There is only ever one NABU on the end of one cable, and
 * a second bridge would need a second tab, so this is file scope rather
 * than a handle the caller carries around. */
static HANDLE   g_com  = INVALID_HANDLE_VALUE;
static SOCKET   g_sock = INVALID_SOCKET;
static int      g_wsa  = 0;
static uint64_t g_to_nabu   = 0;
static uint64_t g_to_server = 0;

static nabu_serial_log_fn g_log = NULL;
static void              *g_log_user = NULL;

static void logf_(const char *fmt, ...)
{
    char    buf[320];
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    g_log(g_log_user, buf);
}

/* ------------------------------------------------------------------ */
/* The cable                                                           */
/* ------------------------------------------------------------------ */

/*
 * COM10 and above cannot be opened by their bare name; the DOS device
 * namespace only reaches COM1 to COM9 that way and everything above
 * needs the \\.\ prefix. Using the prefix for all of them is legal and
 * saves a special case, so that is what this does.
 */
static void com_device_path(const char *name, char *out, size_t cap)
{
    if (!name) { snprintf(out, cap, "%s", ""); return; }
    if (strncmp(name, "\\\\.\\", 4) == 0) snprintf(out, cap, "%s", name);
    else                                  snprintf(out, cap, "\\\\.\\%s", name);
}

static int com_open(const char *name, char *err, size_t errcap)
{
    char        path[64];
    DCB         dcb;
    COMMTIMEOUTS to;

    com_device_path(name, path, sizeof path);

    /* No FILE_FLAG_OVERLAPPED: the relay polls with timeouts that make
     * every read return at once, so synchronous handles are simpler and
     * cannot leave an I/O in flight at teardown. */
    g_com = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (g_com == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        snprintf(err, errcap,
                 (e == ERROR_FILE_NOT_FOUND)
                     ? "%s is not there"
                     : (e == ERROR_ACCESS_DENIED)
                         ? "%s is already in use by another program"
                         : "could not open %s (error %lu)",
                 name, (unsigned long)e);
        return -1;
    }

    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(g_com, &dcb)) {
        snprintf(err, errcap, "could not read the state of %s", name);
        goto fail;
    }

    dcb.BaudRate        = NABU_SERIAL_BAUD;
    dcb.ByteSize        = 8;
    dcb.Parity          = NOPARITY;
    dcb.StopBits        = (NABU_SERIAL_STOPBITS == 2) ? TWOSTOPBITS
                                                      : ONESTOPBIT;
    /*
     * No flow control of any kind. The HCCA is a bare asynchronous line
     * with no handshake wires in play, and letting the driver hold bytes
     * back for a CTS that will never come would be exactly the kind of
     * interference this bridge exists not to do.
     */
    dcb.fBinary         = TRUE;
    dcb.fParity         = FALSE;
    dcb.fOutxCtsFlow    = FALSE;
    dcb.fOutxDsrFlow    = FALSE;
    dcb.fDtrControl     = DTR_CONTROL_ENABLE;
    dcb.fRtsControl     = RTS_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX           = FALSE;
    dcb.fInX            = FALSE;
    dcb.fNull           = FALSE;      /* a NUL byte is data, not nothing */
    dcb.fAbortOnError   = FALSE;

    /*
     * 111860 IS NOT A STANDARD RATE, AND THAT IS THE INTERESTING PART.
     *
     * Win32 takes the literal number rather than an enumerated constant,
     * so whether it takes at all is up to the driver underneath.
     * FTDI-class adapters divide a 24 MHz clock and accept it happily;
     * some others round to the nearest rate they know, and a few refuse.
     *
     * A refusal is reported and the connection is abandoned. It is NOT
     * quietly retried at 115200. That would work, roughly, because 115200
     * is within about 3 percent of the real rate and asynchronous
     * receivers tolerate a few percent, and it is what nabud falls back
     * to. But a bridge that silently runs at the wrong speed produces
     * corruption that looks like a bad cable or a bad server, and the
     * operator deserves to be told which of those it is.
     */
    if (!SetCommState(g_com, &dcb)) {
        snprintf(err, errcap,
                 "%s will not run at %d baud, 8N%d. This is the NABU's "
                 "native rate and not a standard one, so the port's driver "
                 "has to support it; an FTDI-based adapter does",
                 name, NABU_SERIAL_BAUD, NABU_SERIAL_STOPBITS);
        goto fail;
    }

    /*
     * Reads return immediately with whatever has arrived, including
     * nothing. That is what makes the relay non-blocking without
     * overlapped I/O: ReadIntervalTimeout of MAXDWORD with both total
     * timeouts at zero is the documented way to ask for exactly that.
     */
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout         = MAXDWORD;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 0;
    /* Writes are small and the line is fast; a bounded wait keeps a
     * wedged driver from hanging the tab. */
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 2000;
    if (!SetCommTimeouts(g_com, &to)) {
        snprintf(err, errcap, "could not set the timeouts on %s", name);
        goto fail;
    }

    /* Anything the driver buffered before we were configured predates
     * this session and is not ours to forward. */
    PurgeComm(g_com, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return 0;

fail:
    CloseHandle(g_com);
    g_com = INVALID_HANDLE_VALUE;
    return -1;
}

static void com_close(void)
{
    if (g_com != INVALID_HANDLE_VALUE) {
        CloseHandle(g_com);
        g_com = INVALID_HANDLE_VALUE;
    }
}

/* ------------------------------------------------------------------ */
/* The socket                                                          */
/* ------------------------------------------------------------------ */

static void sock_close(void)
{
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
}

/*
 * KEEPALIVE, AND WHY IT IS THE POINT OF THIS PASS.
 *
 * A NABU idle on its main menu sends nothing at all, so a connection
 * whose path has died (venue WiFi, a NAT that forgot it) looks exactly
 * like a healthy idle one until a visitor presses a key, and then the
 * request goes nowhere. Keepalive makes the socket itself notice, so the
 * bridge reconnects before anyone asks it for anything.
 *
 * The per-socket options are Windows 10 1709 and later. Where they are
 * refused, SIO_KEEPALIVE_VALS sets the idle time and interval (the probe
 * count is then the system's own, 10 on Windows 10). Whatever is in
 * force is read back and logged once, so the log says what is true
 * rather than what was asked for.
 */
static void sock_keepalive(void)
{
    DWORD on = 1, idle = NABU_SERIAL_KA_IDLE_S, intvl = NABU_SERIAL_KA_INTVL_S;
    DWORD cnt = NABU_SERIAL_KA_COUNT, maxrt = NABU_SERIAL_MAXRT_S;
    DWORD v1 = 0, v2 = 0, v3 = 0, v4 = 0;
    int   len, ok = 1;

    if (setsockopt(g_sock, SOL_SOCKET, SO_KEEPALIVE,
                   (const char *)&on, sizeof on) != 0) {
        logf_("Warning: keepalive could not be enabled (error %d). A "
              "silently dead network will not be noticed until the NABU "
              "asks for something.", WSAGetLastError());
        return;
    }

    if (setsockopt(g_sock, IPPROTO_TCP, TCP_KEEPIDLE,
                   (const char *)&idle, sizeof idle) != 0 ||
        setsockopt(g_sock, IPPROTO_TCP, TCP_KEEPINTVL,
                   (const char *)&intvl, sizeof intvl) != 0 ||
        setsockopt(g_sock, IPPROTO_TCP, TCP_KEEPCNT,
                   (const char *)&cnt, sizeof cnt) != 0)
        ok = 0;

    if (!ok) {
        struct tcp_keepalive ka;
        DWORD ret = 0;
        ka.onoff             = 1;
        ka.keepalivetime     = NABU_SERIAL_KA_IDLE_S * 1000;
        ka.keepaliveinterval = NABU_SERIAL_KA_INTVL_S * 1000;
        if (WSAIoctl(g_sock, SIO_KEEPALIVE_VALS, &ka, sizeof ka,
                     NULL, 0, &ret, NULL, NULL) == 0)
            logf_("Keepalive on (older Windows): idle %d s, every %d s, "
                  "the system's own probe count.",
                  NABU_SERIAL_KA_IDLE_S, NABU_SERIAL_KA_INTVL_S);
        else
            logf_("Warning: keepalive timing could not be set (error %d).",
                  WSAGetLastError());
    }

    setsockopt(g_sock, IPPROTO_TCP, TCP_MAXRT,
               (const char *)&maxrt, sizeof maxrt);

    if (ok) {
        len = sizeof v1; getsockopt(g_sock, IPPROTO_TCP, TCP_KEEPIDLE,  (char *)&v1, &len);
        len = sizeof v2; getsockopt(g_sock, IPPROTO_TCP, TCP_KEEPINTVL, (char *)&v2, &len);
        len = sizeof v3; getsockopt(g_sock, IPPROTO_TCP, TCP_KEEPCNT,   (char *)&v3, &len);
        len = sizeof v4; getsockopt(g_sock, IPPROTO_TCP, TCP_MAXRT,     (char *)&v4, &len);
        logf_("Keepalive on: idle %lu s, every %lu s, %lu probes; unacknowledged "
              "data given up after %lu s.", (unsigned long)v1,
              (unsigned long)v2, (unsigned long)v3, (unsigned long)v4);
    }
}

/*
 * Connect without ever parking for long: non-blocking, then wait for the
 * result one second at a time, checking `stop` between, for at most 10 s.
 * A blocking connect to an unreachable address takes about 21 s on
 * Windows, and Disconnect must never wait that long.
 */
static int sock_open(const char *host, int port, HANDLE stop,
                     char *err, size_t errcap)
{
    struct addrinfo hints, *res = NULL;
    char   portstr[16];
    int    nodelay = 1, i, soerr = 0, len;
    u_long nb = 1;

    snprintf(portstr, sizeof portstr, "%d", port);
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        snprintf(err, errcap, "cannot resolve %s", host);
        return -1;
    }

    g_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        snprintf(err, errcap, "could not create a socket");
        return -1;
    }

    /* Non-blocking from the start: the connect below, and then the relay,
     * which must never park in recv while a byte is waiting on the
     * cable. */
    ioctlsocket(g_sock, FIONBIO, &nb);

    if (connect(g_sock, res->ai_addr, (int)res->ai_addrlen) != 0 &&
        WSAGetLastError() != WSAEWOULDBLOCK) {
        freeaddrinfo(res);
        sock_close();
        snprintf(err, errcap, "could not reach %s port %d", host, port);
        return -1;
    }
    freeaddrinfo(res);

    for (i = 0; i < 10; i++) {
        fd_set w, e;
        struct timeval tv = { 1, 0 };
        int r;
        FD_ZERO(&w); FD_SET(g_sock, &w);
        FD_ZERO(&e); FD_SET(g_sock, &e);
        r = select(0, NULL, &w, &e, &tv);
        if (r > 0) break;
        if (r < 0 || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            sock_close();
            snprintf(err, errcap, "connection to %s port %d abandoned", host, port);
            return -1;
        }
    }
    len = sizeof soerr;
    if (i == 10 ||
        getsockopt(g_sock, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len) != 0 ||
        soerr != 0) {
        sock_close();
        snprintf(err, errcap, "could not reach %s port %d", host, port);
        return -1;
    }

    /* The same reasoning as the emulator path, and for a stronger
     * reason here: a real NABU's request is a handful of single bytes
     * and Nagle would hold every one of them behind an acknowledgement.
     * See docs/2026-08-07_NABU_NODELAY.md. */
    if (setsockopt(g_sock, IPPROTO_TCP, TCP_NODELAY,
                   (const char *)&nodelay, sizeof nodelay) != 0)
        logf_("Warning: could not disable Nagle on the socket. "
              "The link will work but will be slower than it should be.");

    sock_keepalive();
    return 0;
}

/* The transport nabu_channel wants, on the socket: the channel exchange
 * happens before the relay and is the only place Navigator speaks for
 * itself. */
static int ch_write(void *ctx, uint8_t b)
{
    ULONGLONG deadline = GetTickCount64() + 2000;
    (void)ctx;
    for (;;) {
        if (send(g_sock, (const char *)&b, 1, 0) == 1) return 1;
        if (WSAGetLastError() != WSAEWOULDBLOCK) return 0;
        if (GetTickCount64() >= deadline) return 0;
        Sleep(1);
    }
}

static int ch_read(void *ctx, uint8_t *b, unsigned ms)
{
    ULONGLONG deadline = GetTickCount64() + ms;
    (void)ctx;
    for (;;) {
        int n = recv(g_sock, (char *)b, 1, 0);
        if (n == 1) return 1;
        if (n == 0) return 0;                    /* server hung up */
        if (WSAGetLastError() != WSAEWOULDBLOCK) return 0;
        if (GetTickCount64() >= deadline) return 0;
        Sleep(1);
    }
}

/* ------------------------------------------------------------------ */
/* The relay                                                           */
/* ------------------------------------------------------------------ */

#define PUMP_NET_DOWN  (-2)
#define PUMP_PORT_DOWN (-3)

/*
 * Move whatever is waiting, in both directions, once. Returns the number
 * of bytes moved, PUMP_NET_DOWN when the socket has gone, PUMP_PORT_DOWN
 * when the serial port has. The two are told apart because they are
 * cured differently.
 */
static int pump(void)
{
    char  buf[4096];
    DWORD got = 0, wrote = 0;
    int   moved = 0;
    int   n;

    /*
     * CABLE TO SERVER.
     *
     * Read whatever has arrived and send exactly that, no more and no
     * less. Nothing here looks at a byte's value, so there is no way for
     * this to drop, duplicate or reinterpret one.
     */
    if (!ReadFile(g_com, buf, sizeof buf, &got, NULL)) return PUMP_PORT_DOWN;
    if (got > 0) {
        int off = 0;
        while (off < (int)got) {
            n = send(g_sock, buf + off, (int)got - off, 0);
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) { Sleep(1); continue; }
                return PUMP_NET_DOWN;
            }
            off += n;
        }
        g_to_server += got;
        moved += (int)got;
    }

    /* SERVER TO CABLE, the same way round. */
    n = recv(g_sock, buf, sizeof buf, 0);
    if (n > 0) {
        int off = 0;
        while (off < n) {
            if (!WriteFile(g_com, buf + off, (DWORD)(n - off), &wrote, NULL))
                return PUMP_PORT_DOWN;
            if (wrote == 0) return PUMP_PORT_DOWN;   /* write timeout expired */
            off += (int)wrote;
        }
        g_to_nabu += (uint64_t)n;
        moved += n;
    } else if (n == 0) {
        return PUMP_NET_DOWN;               /* server closed the connection */
    } else if (WSAGetLastError() != WSAEWOULDBLOCK) {
        return PUMP_NET_DOWN;               /* reset, or keepalive gave up */
    }

    return moved;
}

void nabu_serial_counts(uint64_t *to_nabu, uint64_t *to_server)
{
    if (to_nabu)   *to_nabu   = g_to_nabu;
    if (to_server) *to_server = g_to_server;
}

/* ------------------------------------------------------------------ */
/* The session                                                         */
/* ------------------------------------------------------------------ */

static nabu_link_info_t     g_info;
static nabu_serial_state_fn g_state_fn = NULL;

static void report(void)
{
    if (g_state_fn) g_state_fn(g_log_user, &g_info);
}

static void set_state(int st, const char *why)
{
    g_info.state = st;
    if (why) snprintf(g_info.why, sizeof g_info.why, "%s", why);
    report();
}

/* 1, 2, 4, 8, then 15 s for as long as it takes. */
static int backoff_s(int attempt)
{
    static const int steps[] = { 1, 2, 4, 8 };
    if (attempt >= 1 && attempt <= 4) return steps[attempt - 1];
    return 15;
}

/* Wait `secs` seconds, a second at a time so the display can count down.
 * Returns 1 if told to stop meanwhile. */
static int wait_or_stop(HANDLE stop, int secs)
{
    for (; secs > 0; secs--) {
        g_info.next_try_s = secs;
        report();
        if (WaitForSingleObject(stop, 1000) == WAIT_OBJECT_0) return 1;
    }
    g_info.next_try_s = 0;
    report();
    return 0;
}

/*
 * Make the network connection, retrying until it is made or `stop` is
 * set. `was_up` says whether this is a reconnection (it decides the state
 * word and the count). Returns 0 when connected, 1 when stopped.
 */
static int connect_until_up(const nabu_serial_config_t *cfg, HANDLE stop,
                            int was_up)
{
    char err[256];
    int  attempt = 0;

    for (;;) {
        attempt++;
        g_info.attempt = attempt;
        g_info.next_try_s = 0;
        set_state(was_up ? NABU_LINK_RECONNECTING : NABU_LINK_CONNECTING, NULL);

        /*
         * Whatever the NABU sent while there was no server to send it to
         * is half a conversation at best. Throw it away BEFORE connecting,
         * so nabud's first byte from us is the start of a request. The
         * request that was in flight is the pak's to give up on (its own
         * 12 s silence deadline), not ours to replay.
         */
        PurgeComm(g_com, PURGE_RXCLEAR);

        if (sock_open(cfg->host, cfg->port, stop, err, sizeof err) == 0) {
            logf_("Connected to %s port %d.", cfg->host, cfg->port);

            /*
             * THE CHANNEL, AND THE ONE RULE ABOUT IT.
             *
             * This goes out on the socket only, before a single byte of
             * this connection has been relayed in either direction, and
             * the cable never sees any of it. It is configuration aimed
             * at the server, redone for every new connection because
             * nabud keeps the selection per connection.
             *
             * A refusal is logged and the session continues on whatever
             * channel the server is configured for, which is exactly what
             * nabud does with a channel it does not recognise.
             */
            if (cfg->channel > 0) {
                char cherr[160];
                if (nabu_channel_select(cfg->channel, ch_write, ch_read, NULL,
                                        cherr, sizeof cherr))
                    logf_("Channel %d selected on the server.", cfg->channel);
                else
                    logf_("Channel %d not set: %s. The server will serve "
                          "whatever channel it is configured for.",
                          cfg->channel, cherr);
            }

            if (was_up) g_info.reconnects++;
            g_info.attempt = 0;
            g_info.next_try_s = 0;
            g_info.up_since_ms = GetTickCount64();
            set_state(NABU_LINK_UP, "");
            return 0;
        }

        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return 1;

        snprintf(g_info.why, sizeof g_info.why, "%s", err);
        logf_("%s. Trying again in %d s (attempt %d).",
              err, backoff_s(attempt), attempt);
        if (wait_or_stop(stop, backoff_s(attempt))) return 1;
    }
}

/* The serial port went away (a USB adapter pulled out): reopen it the
 * same patient way. Returns 0 when open again, 1 when stopped. */
static int reopen_port_until_up(const nabu_serial_config_t *cfg, HANDLE stop)
{
    char err[256];
    int  attempt = 0;

    for (;;) {
        attempt++;
        g_info.attempt = attempt;
        set_state(NABU_LINK_RECONNECTING, NULL);
        if (com_open(cfg->com_port, err, sizeof err) == 0) {
            logf_("Serial port %s open again.", cfg->com_port);
            return 0;
        }
        snprintf(g_info.why, sizeof g_info.why, "%s", err);
        logf_("%s. Trying again in %d s.", err, backoff_s(attempt));
        if (wait_or_stop(stop, backoff_s(attempt))) return 1;
    }
}

int nabu_serial_run(const nabu_serial_config_t *cfg, void *stop_event,
                    nabu_serial_log_fn log_fn,
                    nabu_serial_state_fn state_fn, void *user)
{
    HANDLE    stop = (HANDLE)stop_event;
    WSADATA   wsad;
    char      err[256] = "";
    uint64_t  last_report = 0;
    ULONGLONG next_count, next_tick;
    int       was_up = 0;

    g_log = log_fn;
    g_log_user = user;
    g_state_fn = state_fn;
    g_to_nabu = g_to_server = 0;
    memset(&g_info, 0, sizeof g_info);

    if (!cfg || !cfg->com_port || !cfg->com_port[0]) {
        set_state(NABU_LINK_FAILED, "no serial port chosen");
        return -1;
    }

    if (!g_wsa) {
        if (WSAStartup(MAKEWORD(2, 2), &wsad) != 0) {
            set_state(NABU_LINK_FAILED, "could not start Winsock");
            return -1;
        }
        g_wsa = 1;
    }

    /* The cable first: a wrong port or a rate the driver will not take is
     * the likely mistake, it will not cure itself, and it is reported at
     * once. */
    if (com_open(cfg->com_port, err, sizeof err) != 0) {
        logf_("%s", err);
        set_state(NABU_LINK_FAILED, err);
        g_log = NULL; g_state_fn = NULL;
        return -1;
    }
    logf_("Serial port %s open at %d baud, 8N%d, no flow control.",
          cfg->com_port, NABU_SERIAL_BAUD, NABU_SERIAL_STOPBITS);

    for (;;) {
        int why_down;

        if (connect_until_up(cfg, stop, was_up)) break;
        if (!was_up)
            logf_("Relaying. Power on the NABU now if it is not already running.");
        else
            logf_("Relaying again (reconnection %u).", g_info.reconnects);
        was_up = 1;

        next_count = GetTickCount64() + 10000;
        next_tick  = GetTickCount64() + 1000;
        why_down   = 0;

        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
            int moved = pump();
            if (moved < 0) { why_down = moved; break; }
            if (moved == 0) Sleep(1);

            /* Once a second, so an uptime can tick on the display. */
            if (GetTickCount64() >= next_tick) {
                report();
                next_tick = GetTickCount64() + 1000;
            }

            /*
             * A byte counter every ten seconds, and only when it has
             * changed. Often enough to show the link is alive, rare
             * enough that the log stays readable over an evening.
             */
            if (GetTickCount64() >= next_count) {
                if (g_to_nabu + g_to_server != last_report) {
                    logf_("%llu bytes to the NABU, %llu bytes to the server.",
                          (unsigned long long)g_to_nabu,
                          (unsigned long long)g_to_server);
                    last_report = g_to_nabu + g_to_server;
                }
                next_count = GetTickCount64() + 10000;
            }
        }

        if (!why_down) break;                       /* asked to stop */

        sock_close();
        if (why_down == PUMP_PORT_DOWN) {
            snprintf(g_info.why, sizeof g_info.why,
                     "the serial port %s went away", cfg->com_port);
            logf_("The serial port %s went away. Reopening it.", cfg->com_port);
            com_close();
            if (reopen_port_until_up(cfg, stop)) break;
        } else {
            snprintf(g_info.why, sizeof g_info.why,
                     "the network link to %s dropped", cfg->host);
            logf_("The network link to %s dropped. Reconnecting.", cfg->host);
        }
    }

    sock_close();
    com_close();
    logf_("Serial port and connection closed.");
    set_state(NABU_LINK_STOPPED, "");
    /* WSACleanup is deliberately not called. Other tabs hold sockets in
     * this process and Winsock is reference counted per process, so
     * tearing it down here would be taking something that is not ours. */
    g_log = NULL;
    g_state_fn = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Which ports exist                                                   */
/* ------------------------------------------------------------------ */

/*
 * QueryDosDevice with a NULL name returns every device in the DOS
 * namespace, and the COM ports are the entries called COM followed by
 * digits. This is used rather than SetupAPI because it needs no extra
 * library, no device class GUID and no privileges, and because it lists
 * exactly what CreateFile can open, which is the question being asked.
 */
int nabu_serial_enumerate(char names[][16], int max)
{
    char  *buf;
    DWORD  cap = 65536, len;
    int    n = 0;
    const char *p;

    if (!names || max <= 0) return 0;
    buf = (char *)malloc(cap);
    if (!buf) return 0;

    len = QueryDosDeviceA(NULL, buf, cap);
    if (len == 0) { free(buf); return 0; }

    for (p = buf; *p && n < max; p += strlen(p) + 1) {
        int  i;
        if (strncmp(p, "COM", 3) != 0) continue;
        if (!p[3]) continue;
        for (i = 3; p[i]; i++)
            if (p[i] < '0' || p[i] > '9') break;
        if (p[i]) continue;                 /* COMsomething, not a port */
        if (strlen(p) >= 16) continue;
        snprintf(names[n], 16, "%s", p);
        n++;
    }

    free(buf);
    return n;
}
