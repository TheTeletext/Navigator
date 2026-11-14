/*
 * arpamail_module.c - ARPANET FTP-Mail module.
 *
 * Speaks the RFC 765 (Postel, IEN 149, June 1980) FTP-Mail dialect over
 * TCP against arpanet.teletext.ca on port 2121 by default. The wire
 * contract is the MAIL inline path with no data channel and no auth:
 *
 *   TCP control connect       -> 220 service ready greeting (banner)
 *   MAIL <recipient>          -> any preliminary 1xx (152 for an unknown
 *                                well-formed recipient routed to the
 *                                editorial catch-all) then 354 to start
 *                                the inline body, else 4xx/5xx terminal
 *   <body bytes><CRLF>.<CRLF> -> strict lone-period terminator, no
 *                                dot-stuffing, no transparency
 *   final reply               -> 250 on success; 550 bad recipient;
 *                                552 body over 2048-byte cap; etc.
 *   QUIT (best-effort)        -> server replies 221, we do not read it
 *
 * Per the RFC 765 three-digit reply scheme, first-digit dispatch is the
 * primary branch: 1xx preliminary, 2xx positive completion, 3xx
 * positive intermediate, 4xx transient negative, 5xx permanent
 * negative. The helper arpa_class() returns 1/2/3/4/5 or 0 on a parse
 * error. Exact-code parsing is used only for 220 (greeting recognition),
 * 152 (preliminary forward-to-EDITOR success; must not be misread as an
 * error), and 354 (mail-input start; must trigger the body-send state).
 * RFC 765 codes are stable; the prior dialect's number-shifting
 * language is retired.
 *
 * OPEN POSTURE: RFC 765 allows mail to be entered before any USER or
 * PASS, and the live server takes that path: no login is required. The
 * client does not send USER or PASS. The prior NETML credential is
 * dropped; the server still accepts it but grants no UI-visible
 * elevation, so it serves no functional purpose here.
 *
 * VOCABULARY: the status line surfaces the server's literal RFC 765
 * reply line verbatim (trailing CRLF stripped) for every step, via
 * arpa_status_from_line(). No paraphrase table is maintained: the
 * server text is the authentic 1980 vocabulary and is the
 * lowest-maintenance choice. The only transient is "Connecting..."
 * before the 220 banner arrives, since the server has not yet spoken.
 * Local-only errors (DNS, connect) also keep short English messages
 * because the server has not yet spoken.
 *
 * RECIPIENT FORMAT: LOCAL@HOST or LOCAL AT HOST. Both forms are
 * accepted on the wire per the locked server decision. The at-sign
 * form is the canonical or preferred rendering: RFC 765's own 151
 * reply example uses it ("User not local; Will forward to
 * <user>@<host>.") and the BNF defines <ident> ::= <string> as an
 * opaque string. The default To: is "INFO@TELETEXT". INFO is
 * server-side always-valid and bypasses the allowlist.
 *
 * LONE-PERIOD LIMITATION: per RFC 765, the inline body ends at the
 * first line whose entire content is exactly one period. There is no
 * dot-stuffing and no transparency. A user body line of exactly one
 * period cannot be transmitted; the server reads it as the terminator
 * and any text after it is read as FTP commands. The client does NOT
 * silently rewrite, escape, or strip such a line. The limitation is
 * documented-only; no UI affordance surfaces it in the F2 module.
 *
 * BODY SANITIZATION: CRLF normalization (lone CR or LF promoted to
 * CRLF), ASCII control stripping (CR, LF, TAB preserved), ANSI escape
 * removal. 2048-byte soft cap (matches the server's MAX_MSG_SIZE).
 * The cap is soft in the UI byte counter; Send is not blocked on
 * over-cap and the server emits the authentic 552.
 *
 * UI SESSION GATE: the wire is stateless (Connect, Send, and
 * Disconnect each open and close their own socket). The g_a_connected
 * flag is the UI's session state and controls which fields are
 * reachable. Pre-connect: only Server/Port/Connect enabled. Connected:
 * To/Body/Clear/Disconnect enabled, Server/Port/Connect locked. Send
 * additionally gated on the connected flag.
 *
 * CONNECT PROBE: banner-only. TCP connect, read the 220 greeting,
 * close socket without QUIT and without any further command. The 220
 * greeting alone is the connected-state proof; UI flips to connected
 * on a 2xx first-digit on a fresh control connection.
 *
 * DISCONNECT PROBE: TCP connect, read the 220 greeting, send QUIT,
 * read 221, close. UI returns to pre-connect; To: is restored to the
 * default ARPA_DEFAULT_TO ("INFO@TELETEXT"), Message is cleared.
 * Pre-auth QUIT is legal under the open posture.
 *
 * Module state (To: field, Message: body) is preserved across module
 * switches per [[feedback-module-state-preserved]] using the hide/show
 * pattern.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "win_platform.h"

#include "arpamail_module.h"
#include "suite_shell.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "suite_w31.h"
#include "ftp_ctrl.h"

/* Server defaults (editable; user can change via Server/Port fields +
 * Connect). The default port 2121 is the live FTP-Mail control port,
 * a server-side deviation from the RFC 765 assigned port 21 so the
 * mail service coexists with the existing port-21 file FTP. The
 * default To: uses the at-sign form per RFC 765's 151 reply example
 * and the BNF <ident> ::= <string>. */
#define ARPA_DEFAULT_HOST    "arpanet.teletext.ca"
#define ARPA_DEFAULT_PORT    "2121"
#define ARPA_DEFAULT_TO      "INFO@TELETEXT"
#define ARPA_MAX_BODY        2048
#define ARPA_NET_TIMEOUT     10   /* seconds for every connect and recv */

#define IDC_ARPA_SERVER_EDIT    2001
#define IDC_ARPA_PORT_EDIT      2002
#define IDC_ARPA_CONNECT_BTN    2003
#define IDC_ARPA_TO_EDIT        2004
#define IDC_ARPA_BODY_EDIT      2005
#define IDC_ARPA_SEND_BTN       2006
#define IDC_ARPA_CLEAR_BTN      2007
#define IDC_ARPA_DISCONNECT_BTN 2008

static HWND g_a_server_lbl, g_a_server_edit;
static HWND g_a_port_lbl,   g_a_port_edit;
static HWND g_a_connect_btn, g_a_disconnect_btn;   /* 2026-11-11: Disconnect
                                                     retired (one toggle); NULL */
static HWND g_a_proto_lbl;
static HWND g_a_to_lbl,     g_a_to_edit;
static HWND g_a_body_lbl,   g_a_body_edit;
static HWND g_a_count_lbl;
static HWND g_a_send_btn,   g_a_clear_btn;

static WNDPROC g_orig_to_proc   = NULL;
static WNDPROC g_orig_body_proc = NULL;

static char g_a_active_host[256];
static char g_a_active_port[16];

static BOOL g_a_controls_created = FALSE;

/* UI session gate. The wire is stateless (Connect/Send/Disconnect each
 * open and close their own socket); this flag is the UI's "connected"
 * state and controls which fields are reachable. Pre-connect: only
 * Server/Port/Connect enabled; connected: To/Body/Clear/Disconnect
 * enabled, Server/Port/Connect locked, Send additionally gated on the
 * connected flag. */
static BOOL g_a_connected = FALSE;

/* Post-submit lock per blueprint 4.3. Set TRUE on a 2xx final reply
 * for either submit path; cleared on Reset (Clear while locked) and
 * on Disconnect. When TRUE the Send-enable expression in the counter
 * helper goes FALSE and the Message edit is EM_SETREADONLY TRUE. */
static BOOL g_a_locked = FALSE;

/* ------------------------------------------------------------------ */
/* Status helper.                                                      */
/* ------------------------------------------------------------------ */

static void arpa_status(HWND content, const char *text)
{
    HWND main_window;
    suite_set_status(text);
    main_window = GetParent(content);
    if (main_window) UpdateWindow(main_window);
}

/* ------------------------------------------------------------------ */
/* Body sanitization per BLUEPRINT 5.5.                                */
/* Output buffer caller-allocated, size >= 2 * src_len + 1.            */
/* Returns sanitized length.                                           */
/* ------------------------------------------------------------------ */

static size_t arpa_sanitize_body(const char *src, size_t src_len,
                                 char *dst, size_t dst_cap)
{
    size_t i = 0, j = 0;

    while (i < src_len && j + 2 < dst_cap) {
        unsigned char c = (unsigned char)src[i];

        /* ANSI escape sequence: ESC + '[' + parameters + final byte. */
        if (c == 0x1B && i + 1 < src_len && (unsigned char)src[i + 1] == '[') {
            i += 2;
            while (i < src_len) {
                unsigned char p = (unsigned char)src[i];
                i++;
                if (p >= 0x40 && p <= 0x7E) break;
            }
            continue;
        }
        /* Standalone ESC. */
        if (c == 0x1B) { i++; continue; }

        /* CRLF normalization: lone CR -> CRLF, lone LF -> CRLF. */
        if (c == '\r') {
            dst[j++] = '\r';
            dst[j++] = '\n';
            i++;
            if (i < src_len && (unsigned char)src[i] == '\n') i++;
            continue;
        }
        if (c == '\n') {
            dst[j++] = '\r';
            dst[j++] = '\n';
            i++;
            continue;
        }

        /* Strip ASCII controls < 0x20 except TAB (preserved). */
        if (c < 0x20 && c != '\t') { i++; continue; }

        dst[j++] = (char)c;
        i++;
    }
    dst[j] = '\0';
    return j;
}

/* ------------------------------------------------------------------ */
/* Live byte counter: update label as user types in body. Send is      */
/* enabled iff the UI is in the connected state. We do NOT gate Send   */
/* on body length: clicking Send with an empty or over-cap body runs   */
/* the full MAIL handshake and lets the server speak its authentic     */
/* reply (e.g. 552 for over-cap). No local pre-emption of wire         */
/* outcomes.                                                           */
/* ------------------------------------------------------------------ */

static void arpa_update_byte_counter(void)
{
    int len = GetWindowTextLengthA(g_a_body_edit);
    char buf[64];
    _snprintf(buf, sizeof(buf), "Bytes used: %d of %d%s",
              len, ARPA_MAX_BODY,
              len > ARPA_MAX_BODY ? "  (over cap)" : "");
    SetWindowTextA(g_a_count_lbl, buf);

    EnableWindow(g_a_send_btn, (g_a_connected && !g_a_locked) ? TRUE : FALSE);
}

/* ------------------------------------------------------------------ */
/* Switch the UI between pre-connect and connected states. Both        */
/* transitions toggle a specific set of fields. The connected flag is  */
/* the source of truth.                                                */
/* ------------------------------------------------------------------ */

static void arpa_set_connected_state(BOOL connected)
{
    g_a_connected = connected;

    EnableWindow(g_a_server_edit,    !connected);
    EnableWindow(g_a_port_edit,      !connected);
    /* 2026-11-11: one Connect/Disconnect toggle (docs/2026-11-11_W31_BUTTONS.md). */
    w31_toggle_set(g_a_connect_btn, connected);
    EnableWindow(g_a_to_edit,         connected);
    EnableWindow(g_a_body_edit,       connected);
    EnableWindow(g_a_clear_btn,       connected);

    /* Send is gated on the connected flag (set inside the counter
     * helper for consistency). */
    arpa_update_byte_counter();
}

/* ------------------------------------------------------------------ */
/* Socket helpers.                                                     */
/* ------------------------------------------------------------------ */

/* Read up to a CRLF (or LF) terminated line, or until buffer full. */
/* First-digit reply class per RFC 765 Section 4.2: 1xx preliminary,
 * 2xx positive completion, 3xx positive intermediate, 4xx transient
 * negative, 5xx permanent negative. Returns 1/2/3/4/5, or 0 on a
 * parse error or out-of-range code. The client branches on this
 * helper rather than on exact numeric codes, except where the
 * docblock identifies an exact-code exception (220, 152, 354). */
static int arpa_class(int code)
{
    if (code < 100 || code > 599) return 0;
    return code / 100;
}

/* ------------------------------------------------------------------ */
/* Surface the server's literal reply line as the status, stripping    */
/* only the trailing CRLF. The RFC 765 reply text IS the authentic     */
/* 1980 wording and cannot drift from the server.                      */
/* ------------------------------------------------------------------ */

static void arpa_status_from_line(HWND content, const char *line)
{
    char buf[1024];
    int  i, j = 0;
    for (i = 0; line[i] && j < (int)sizeof(buf) - 1; i++) {
        if (line[i] == '\r' || line[i] == '\n') break;
        buf[j++] = line[i];
    }
    buf[j] = '\0';
    arpa_status(content, buf);
}

/* ------------------------------------------------------------------ */
/* The wire, since 2026-11-12, is the shared core src/ftp_ctrl.c (the  */
/* FTP tab and Archie's Download use the same one): a MAIL session's   */
/* allowlist is MAIL and QUIT, nothing else can be sent; replies are   */
/* RFC 959 multi-line aware; the connect runs in 200 ms slices. The   */
/* bytes this client sends are unchanged: the before/after transcript  */
/* diff is in docs/2026-11-12_FTP_TAB.md. Every server line still    */
/* becomes the status verbatim, through the core's log callback.      */
/* ------------------------------------------------------------------ */

static void arpa_log(void *user, int kind, const char *line)
{
    if (kind == FTPC_LOG_SERVER) arpa_status_from_line((HWND)user, line);
}

/* Connect to the active host:port and read the greeting. NULL when the
 * connect failed (status set). Otherwise the session, with *greet the
 * greeting's code, or below zero when none came ("No FTP-Mail banner"
 * status set; an unparseable line is already the status). */
static ftp_ctrl_t *arpa_open(HWND content, int *greet)
{
    ftp_ctrl_t *c;
    char        line[1024];
    int         port_int, rc;

    port_int = atoi(g_a_active_port);
    if (port_int <= 0 || port_int > 65535) port_int = 2121;

    c = ftpc_new(FTPC_VERBS_MAIL, arpa_log, content);
    if (!c) { arpa_status(content, "Out of memory."); return NULL; }
    ftpc_set_reply_timeout(c, ARPA_NET_TIMEOUT * 1000);

    arpa_status(content, "Connecting...");
    rc = ftpc_connect(c, g_a_active_host, port_int, ARPA_NET_TIMEOUT * 1000);
    if (rc != 0) {
        char m[300];
        if (rc == FTPC_ERR_RESOLVE)
            _snprintf(m, sizeof(m), "Cannot resolve %s.", g_a_active_host);
        else
            _snprintf(m, sizeof(m), "Cannot connect to %s:%s.",
                      g_a_active_host, g_a_active_port);
        m[sizeof(m) - 1] = '\0';
        arpa_status(content, m);
        ftpc_release(c);
        return NULL;
    }
    *greet = ftpc_reply(c, line, sizeof(line));
    if (*greet < 0 && *greet != FTPC_ERR_PROTO) {
        char m[300];
        _snprintf(m, sizeof(m), "No FTP-Mail banner from %s:%s.",
                  g_a_active_host, g_a_active_port);
        m[sizeof(m) - 1] = '\0';
        arpa_status(content, m);
    }
    return c;
}

/* ------------------------------------------------------------------ */
/* The RFC 765 MAIL inline send sequence. Open posture, no USER/PASS.  */
/* One control connection per Send. Server replies surface verbatim    */
/* via the reply-loop helper.                                          */
/*                                                                     */
/*   TCP connect, expect 220 greeting (first-digit 2).                 */
/*   MAIL <recipient>, expect any preliminary 152 then 354.            */
/*   Body bytes, then CRLF dot CRLF strict terminator.                 */
/*   Final 250 (or any 2xx) on success; otherwise Send fails.          */
/*   Best-effort QUIT (reply not read), close.                         */
/* ------------------------------------------------------------------ */

static BOOL arpa_send_mail(HWND content,
                           const char *recipient,
                           const char *raw_body, size_t raw_len)
{
    ftp_ctrl_t        *c = NULL;
    char               line[1024];
    char              *clean = NULL;
    size_t             clean_len = 0;
    int                code;
    BOOL               crlf_ended;
    const char        *term;
    int                term_len;
    size_t             period_off = 0;
    BOOL               has_period = FALSE;
    BOOL               ok = FALSE;

    /* Greeting: 220, first-digit 2. */
    c = arpa_open(content, &code);
    if (!c) goto cleanup;
    if (code < 0 || arpa_class(code) != 2) goto cleanup;

    /* MAIL <recipient>: expect any 1xx preliminary (152 for an unknown
     * well-formed recipient routed to the editorial catch-all) then
     * 354 (mail-input start). Any non-354 terminal reply ends the
     * send; 4xx/5xx surface verbatim via the reply-loop helper. */
    if (ftpc_send_cmd(c, "MAIL", recipient) < 0) goto cleanup;
    code = ftpc_reply_final(c, line, sizeof(line));
    if (code != 354) goto cleanup;

    /* Sanitize and frame the wire bytes after 354 per the 2026-05-18
     * authority: a lone-period line is the terminator wherever it
     * first appears in the sanitized body, and the no-period path
     * still appends the client terminator with the existing
     * crlf_ended choice. Strict no-dot-stuffing applies in both
     * cases; body lines before the terminator are written verbatim. */
    clean = (char *)malloc(raw_len * 2 + 8);
    if (!clean) { arpa_status(content, "Out of memory."); goto cleanup; }
    clean_len = arpa_sanitize_body(raw_body, raw_len, clean, raw_len * 2 + 8);

    /* Scan clean for the first lone-period line. A line is a maximal
     * byte run delimited by CRLF or the buffer start or end; the
     * lone-period line content is exactly one 0x2E with nothing else.
     * Per blueprint 3.4, this scan applies to both submit triggers. */
    {
        size_t scan_i = 0;
        period_off = clean_len;
        has_period = FALSE;
        while (scan_i < clean_len) {
            size_t scan_e = scan_i;
            while (scan_e < clean_len) {
                if (scan_e + 1 < clean_len &&
                    clean[scan_e] == '\r' && clean[scan_e + 1] == '\n') break;
                scan_e++;
            }
            if (scan_e - scan_i == 1 && clean[scan_i] == '.') {
                period_off = scan_i;
                has_period = TRUE;
                break;
            }
            if (scan_e >= clean_len) break;
            scan_i = scan_e + 2;
        }
    }

    if (has_period) {
        /* Trigger B path, or Send-button path with a lone-period in
         * the body. Send the prefix up to the period line as the
         * mail text (CRLF-framed by construction, since each line
         * start in the scan is at offset 0 or just past a CRLF),
         * then exactly ".\r\n" as the terminator. Empty-body case
         * (period at offset 0) sends only ".\r\n". Nothing after
         * the period line is transmitted. */
        if (period_off > 0) {
            if (ftpc_send_mail_text(c, clean, (int)period_off) < 0) goto cleanup;
        }
        if (ftpc_send_mail_text(c, ".\r\n", 3) < 0) goto cleanup;
    } else {
        /* No-period path: preserve the existing behavior byte for
         * byte. Send the sanitized body if non-empty, then the
         * crlf_ended choice of ".\r\n" or "\r\n.\r\n". */
        if (clean_len > 0) {
            if (ftpc_send_mail_text(c, clean, (int)clean_len) < 0) goto cleanup;
        }
        crlf_ended = (clean_len >= 2 &&
                      clean[clean_len - 2] == '\r' &&
                      clean[clean_len - 1] == '\n');
        term     = crlf_ended ? ".\r\n" : "\r\n.\r\n";
        term_len = crlf_ended ? 3       : 5;
        if (ftpc_send_mail_text(c, term, term_len) < 0) goto cleanup;
    }

    /* Final reply: 250 or another 2xx for success. On a 2xx the
     * post-submit lock per blueprint 4.3 engages: Send is disabled
     * via the counter helper (whose gate is connected && !locked),
     * the Message edit becomes read-only, and the byte counter
     * naturally freezes because the read-only edit emits no further
     * EN_CHANGE. On a non-2xx no lock is applied; the literal reply
     * stays in the status surface and the box remains editable. */
    code = ftpc_reply_final(c, line, sizeof(line));
    if (code < 0) goto cleanup;
    if (arpa_class(code) == 2) {
        ok = TRUE;
        g_a_locked = TRUE;
        SendMessageA(g_a_body_edit, EM_SETREADONLY, (WPARAM)TRUE, 0);
        arpa_update_byte_counter();
    }

cleanup:
    if (c) {
        /* Best-effort QUIT. Server emits 221; not read. */
        if (ftpc_connected(c)) ftpc_send_cmd(c, "QUIT", NULL);
        ftpc_release(c);
    }
    if (clean) free(clean);

    return ok;
}

/* ------------------------------------------------------------------ */
/* Send button click handler. The server returns the authentic 550    */
/* for an empty or malformed recipient and 552 for an over-cap body.  */
/* No local pre-emption: the user reads the server's literal line.    */
/* ------------------------------------------------------------------ */

static void arpa_on_send(HWND content)
{
    char    recipient[256];
    char   *body;
    int     body_len;

    /* Unified submit guard per blueprint 2.2: every submit path
     * (Send button, Trigger B, and the Ctrl+Enter accelerator)
     * passes through this function, so a single early return here
     * is sufficient to honor the post-submit lock and the
     * connected precondition. Defense in depth: the Send button
     * is also greyed via the counter helper, and Trigger B
     * already checks the same flags before calling. */
    if (!g_a_connected || g_a_locked) return;

    GetWindowTextA(g_a_to_edit, recipient, sizeof(recipient));
    body_len = GetWindowTextLengthA(g_a_body_edit);

    body = (char *)malloc((size_t)body_len + 1);
    if (!body) { arpa_status(content, "Out of memory."); return; }
    if (body_len > 0)
        GetWindowTextA(g_a_body_edit, body, body_len + 1);
    else
        body[0] = '\0';

    /* No automatic field clearing after Send. Both To: and Message
     * persist; user clears them via Clear or backspace. This allows
     * the user to re-send the same message (e.g., to confirm delivery
     * on the server side) without retyping. */
    arpa_send_mail(content, recipient, body, (size_t)body_len);

    free(body);
    SetFocus(g_a_body_edit);
}

/* ------------------------------------------------------------------ */
/* Connect: banner-only probe per blueprint 3.10. TCP connect, read   */
/* the 220 greeting, close socket. No NOOP, no auth, no QUIT. The     */
/* 220 greeting alone is the connected-state proof; UI flips to       */
/* connected on a 2xx first-digit on a fresh control connection. The  */
/* literal greeting line is surfaced to status.                       */
/* ------------------------------------------------------------------ */

static void arpa_on_connect(HWND content)
{
    char               host[256], port[16];
    int                port_int;
    ftp_ctrl_t        *c;
    int                code;
    BOOL               ok_banner = FALSE;

    GetWindowTextA(g_a_server_edit, host, sizeof(host));
    GetWindowTextA(g_a_port_edit,   port, sizeof(port));

    if (!host[0] || !port[0]) {
        arpa_status(content, "Server and Port are required.");
        return;
    }
    port_int = atoi(port);
    if (port_int <= 0 || port_int > 65535) {
        arpa_status(content, "Port must be 1-65535.");
        return;
    }

    strncpy(g_a_active_host, host, sizeof(g_a_active_host) - 1);
    strncpy(g_a_active_port, port, sizeof(g_a_active_port) - 1);
    g_a_active_host[sizeof(g_a_active_host) - 1] = '\0';
    g_a_active_port[sizeof(g_a_active_port) - 1] = '\0';

    /* Banner: any 2xx first-digit on a fresh control connection
     * (220 from the live server) is the connected-state proof. */
    c = arpa_open(content, &code);
    if (!c) return;
    if (code >= 0 && arpa_class(code) == 2) ok_banner = TRUE;

    ftpc_release(c);     /* no QUIT, no NOOP */

    if (ok_banner) {
        arpa_set_connected_state(TRUE);
        SetFocus(g_a_to_edit);
    }
    /* else: stay pre-connect, fields stay locked. */
}

/* ------------------------------------------------------------------ */
/* Disconnect: TCP connect, read the 220 greeting, send QUIT, read    */
/* 221, close. Pre-auth QUIT is legal under the RFC 765 open posture. */
/* After the probe, the UI unconditionally returns to pre-connect.    */
/* The reset happens even if the probe failed, since the user's       */
/* intent was to disconnect.                                          */
/* ------------------------------------------------------------------ */

static void arpa_on_disconnect(HWND content)
{
    ftp_ctrl_t        *c = NULL;
    char               line[1024];
    int                code;

    c = arpa_open(content, &code);
    if (!c) goto reset;
    if (code < 0 || arpa_class(code) != 2) goto reset;

    /* QUIT. 2xx expected (221). */
    if (ftpc_send_cmd(c, "QUIT", NULL) < 0) goto reset;
    ftpc_reply(c, line, sizeof(line));

reset:
    if (c) ftpc_release(c);

    /* Clear the post-submit lock as part of the disconnected
     * baseline. Disconnect was never gated on the lock, so this is
     * reached from both the locked and the composable states. */
    g_a_locked = FALSE;
    SendMessageA(g_a_body_edit, EM_SETREADONLY, (WPARAM)FALSE, 0);

    /* Restore To: to the default, clear Message, return UI to pre-connect. */
    SetWindowTextA(g_a_to_edit, ARPA_DEFAULT_TO);
    SetWindowTextA(g_a_body_edit, "");
    arpa_set_connected_state(FALSE);
}

/* ------------------------------------------------------------------ */
/* Clear button. State-aware per the 2026-05-18 authority blueprint:  */
/* locked-state acts as Reset (clears the lock, removes read-only,    */
/* clears Message, refreshes counter to re-enable Send, status to     */
/* Ready); composable-state clears Message and refreshes the counter. */
/* Both paths leave To unchanged and focus the Message edit.          */
/* ------------------------------------------------------------------ */

static void arpa_on_clear(HWND content)
{
    if (g_a_locked) {
        /* Reset transition per blueprint 4.4: drop the lock and put
         * the Message edit back into the editable state, then fall
         * through to the composable cleanup below. */
        g_a_locked = FALSE;
        SendMessageA(g_a_body_edit, EM_SETREADONLY, (WPARAM)FALSE, 0);
    }
    /* Composable cleanup: clear Message, refresh the counter (which
     * re-enables Send because locked is now FALSE), status to Ready,
     * focus the Message edit. To is left unchanged per blueprint. */
    SetWindowTextA(g_a_body_edit, "");
    arpa_update_byte_counter();
    arpa_status(content, "Ready");
    SetFocus(g_a_body_edit);
}

/* ------------------------------------------------------------------ */
/* Subclass: To edit (Enter forwards focus to body) and Body edit.    */
/* Body edit handles: plain Enter on a lone-period line, which fires  */
/* Trigger B per blueprint 4.1 and submits via the Send path; and    */
/* the pre-existing Ctrl+Enter Send accelerator.                     */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK ToEditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_orig_to_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN && pmsg->wParam == VK_RETURN)
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        /* Enter in To: jumps focus to the body. */
        SetFocus(g_a_body_edit);
        return 0;
    }
    if (m == WM_CHAR && w == '\r') return 0;
    return CallWindowProcA(g_orig_to_proc, h, m, w, l);
}

static LRESULT CALLBACK BodyEditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    /* Trigger B per blueprint 4.1: a plain Enter (no Ctrl) that
     * completes a logical line whose entire content is exactly one
     * period byte (0x2E) and nothing else, including no spaces or
     * tabs, fires the same submit path the Send button uses.
     * Precondition per blueprint 4.1 and 4.3: connected and not
     * locked. Disjoint from the existing Ctrl+Enter branch below. */
    if (m == WM_KEYDOWN && w == VK_RETURN &&
        !(GetKeyState(VK_CONTROL) & 0x8000) &&
        g_a_connected && !g_a_locked) {
        DWORD sel_start = 0;
        int   line_idx, char_idx, line_len;
        SendMessageA(h, EM_GETSEL, (WPARAM)&sel_start, 0);
        line_idx = (int)SendMessageA(h, EM_LINEFROMCHAR, (WPARAM)sel_start, 0);
        char_idx = (int)SendMessageA(h, EM_LINEINDEX,    (WPARAM)line_idx,  0);
        line_len = (int)SendMessageA(h, EM_LINELENGTH,   (WPARAM)char_idx,  0);
        if (line_len == 1) {
            char lbuf[8];
            int  copied;
            *(WORD *)lbuf = sizeof(lbuf);
            copied = (int)SendMessageA(h, EM_GETLINE,
                                       (WPARAM)line_idx, (LPARAM)lbuf);
            if (copied == 1 && lbuf[0] == '.') {
                arpa_on_send(GetParent(h));
                return 0;
            }
        }
        /* Not a lone-period line: fall through to default newline. */
    }
    /* Multi-line: Enter inserts newline (default). Ctrl+Enter sends. */
    if (m == WM_KEYDOWN && w == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        arpa_on_send(GetParent(h));
        return 0;
    }
    return CallWindowProcA(g_orig_body_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Layout (per BLUEPRINT 5.1).                                         */
/* ------------------------------------------------------------------ */

void arpamail_module_resize(HWND content, int w, int h)
{
    const int margin     = 16;
    const int row_h      = 26;
    const int btn_w      = 100;
    const int connect_w  = 85;
    const int btn_h      = 30;
    int y, body_h, count_y, btn_y;

    if (!g_a_server_edit) return;

    /* Row 1: Server: + edit + Port: + edit + Connect button (right). */
    y = 14;
    MoveWindow(g_a_server_lbl,  margin,             y + 2, 55, 22, TRUE);
    MoveWindow(g_a_server_edit, margin + 60,        y, 280, row_h, TRUE);
    MoveWindow(g_a_port_lbl,    margin + 60 + 290,  y + 2, 40, 22, TRUE);
    MoveWindow(g_a_port_edit,   margin + 60 + 330,  y, 60, row_h, TRUE);
    /* The Connect/Disconnect toggle: right end of row 1, centred on it.
     * (Through 2026-11-11 a separate Disconnect sat below Connect.) */
    MoveWindow(g_a_connect_btn, w - margin - W31_TOGGLE_W, y + (row_h - W31_TOGGLE_H) / 2,
               W31_TOGGLE_W, W31_TOGGLE_H, TRUE);
    (void)connect_w;

    /* Row 2: Protocol label (left-aligned). Rows 2 on moved up 32 px
     * on 2026-11-11, when the Disconnect that stood below Connect was
     * merged into the toggle (they were at 82 / 110 / 144 / 168). */
    y = 50;
    MoveWindow(g_a_proto_lbl, margin, y, w - 2 * margin, 22, TRUE);

    /* Row 3: To: + edit. */
    y = 78;
    MoveWindow(g_a_to_lbl,  margin,        y + 2, 50, 22, TRUE);
    MoveWindow(g_a_to_edit, margin + 55,   y, w - margin - 55 - margin, row_h, TRUE);

    /* Body label. */
    y = 112;
    MoveWindow(g_a_body_lbl, margin, y, 80, 22, TRUE);

    /* Body edit fills the middle. */
    y = 136;
    btn_y   = h - margin - btn_h;
    count_y = btn_y + (btn_h - 22) / 2;
    body_h  = btn_y - 16 - y;
    if (body_h < 80) body_h = 80;
    MoveWindow(g_a_body_edit, margin, y, w - 2 * margin, body_h, TRUE);

    /* Bottom row: counter (left), Clear + Send (right). */
    MoveWindow(g_a_count_lbl,
               margin, count_y, w - margin - 2 * (btn_w + 8) - margin, 22, TRUE);
    MoveWindow(g_a_clear_btn,
               w - margin - 2 * btn_w - 8, btn_y, btn_w, btn_h, TRUE);
    MoveWindow(g_a_send_btn,
               w - margin - btn_w, btn_y, btn_w, btn_h, TRUE);
}

/* ------------------------------------------------------------------ */
/* Activate / deactivate with hide-show state preservation.            */
/* ------------------------------------------------------------------ */

void arpamail_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    if (g_a_controls_created) {
        ShowWindow(g_a_server_lbl,      SW_SHOW);
        ShowWindow(g_a_server_edit,     SW_SHOW);
        ShowWindow(g_a_port_lbl,        SW_SHOW);
        ShowWindow(g_a_port_edit,       SW_SHOW);
        ShowWindow(g_a_connect_btn,     SW_SHOW);
        ShowWindow(g_a_disconnect_btn,  SW_SHOW);
        ShowWindow(g_a_proto_lbl,       SW_SHOW);
        ShowWindow(g_a_to_lbl,          SW_SHOW);
        ShowWindow(g_a_to_edit,         SW_SHOW);
        ShowWindow(g_a_body_lbl,        SW_SHOW);
        ShowWindow(g_a_body_edit,       SW_SHOW);
        ShowWindow(g_a_count_lbl,       SW_SHOW);
        ShowWindow(g_a_send_btn,        SW_SHOW);
        ShowWindow(g_a_clear_btn,       SW_SHOW);
        /* Focus the field that's enabled in the current session state. */
        SetFocus(g_a_connected ? g_a_to_edit : g_a_server_edit);
        return;
    }

    /* Initialize active bindings to defaults on first activation. */
    strcpy(g_a_active_host, ARPA_DEFAULT_HOST);
    strcpy(g_a_active_port, ARPA_DEFAULT_PORT);

    /* Row 1: Server: + edit + Port: + edit + Connect. */
    g_a_server_lbl = CreateWindowA("STATIC", "Server:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 55, 22,
        content, NULL, hInst, NULL);
    g_a_server_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        ARPA_DEFAULT_HOST,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 280, 26, content, (HMENU)(INT_PTR)IDC_ARPA_SERVER_EDIT,
        hInst, NULL);
    g_a_port_lbl = CreateWindowA("STATIC", "Port:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 40, 22,
        content, NULL, hInst, NULL);
    g_a_port_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        ARPA_DEFAULT_PORT,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 60, 26, content, (HMENU)(INT_PTR)IDC_ARPA_PORT_EDIT,
        hInst, NULL);
    /* Port field accepts up to 4 digits per editorial direction. */
    SendMessageA(g_a_port_edit, EM_SETLIMITTEXT, (WPARAM)4, 0);
    g_a_connect_btn = CreateWindowA("BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, W31_TOGGLE_W, W31_TOGGLE_H, content, (HMENU)(INT_PTR)IDC_ARPA_CONNECT_BTN,
        hInst, NULL);
    g_a_disconnect_btn = NULL;   /* merged into the Connect toggle, 2026-11-11 */

    /* Row 2: Protocol label (hardwired). */
    g_a_proto_lbl = CreateWindowA("STATIC", "Protocol: FTP-Mail (RFC 765)",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 300, 22,
        content, NULL, hInst, NULL);

    /* Row 3: To: + edit (default INFO@TELETEXT, at-sign form per
     * RFC 765 reply example and BNF, Amendment 3). */
    g_a_to_lbl = CreateWindowA("STATIC", "To:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 50, 22,
        content, NULL, hInst, NULL);
    g_a_to_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
        ARPA_DEFAULT_TO,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 26, content, (HMENU)(INT_PTR)IDC_ARPA_TO_EDIT,
        hInst, NULL);

    /* Message label + body edit. */
    g_a_body_lbl = CreateWindowA("STATIC", "Message:",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 80, 22,
        content, NULL, hInst, NULL);
    g_a_body_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
        0, 0, 200, 200, content, (HMENU)(INT_PTR)IDC_ARPA_BODY_EDIT,
        hInst, NULL);
    SendMessageA(g_a_body_edit, EM_SETLIMITTEXT, (WPARAM)(ARPA_MAX_BODY * 2), 0);

    /* Bottom row: counter + Send + Clear. */
    g_a_count_lbl = CreateWindowA("STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 200, 22,
        content, NULL, hInst, NULL);
    g_a_send_btn = CreateWindowA("BUTTON", "Send",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 30, content, (HMENU)(INT_PTR)IDC_ARPA_SEND_BTN,
        hInst, NULL);
    g_a_clear_btn = CreateWindowA("BUTTON", "Clear",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 100, 30, content, (HMENU)(INT_PTR)IDC_ARPA_CLEAR_BTN,
        hInst, NULL);

    /* Fonts. */
    if (g_hFontUI) {
        SendMessageA(g_a_server_lbl,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_server_edit,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_port_lbl,        WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_port_edit,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_connect_btn,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_disconnect_btn,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_proto_lbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_to_lbl,          WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_to_edit,         WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_body_lbl,        WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_count_lbl,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_send_btn,        WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_a_clear_btn,       WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }
    if (g_hFontOutput)
        SendMessageA(g_a_body_edit, WM_SETFONT, (WPARAM)g_hFontOutput, TRUE);

    /* Navigator 1.5 (docs/2026-11-10_NAVIGATOR_WIN31_UX.md), operator
     * decision 2026-11-10: the tab keeps its terminal look. The To: line
     * and the message are white on black in fixed pitch, as in 1.0;
     * labels, buttons and the strip are the Windows 3.1 chrome. */
    w31_mark_terminal(g_a_to_edit);
    w31_mark_terminal(g_a_body_edit);
    if (g_w31_font_term)
        SendMessageA(g_a_to_edit, WM_SETFONT, (WPARAM)g_w31_font_term, TRUE);

    /* Subclass To: edit (Enter -> body) and body edit (Ctrl+Enter -> Send). */
    g_orig_to_proc   = (WNDPROC)SetWindowLongPtrA(
        g_a_to_edit, GWLP_WNDPROC, (LONG_PTR)ToEditSub);
    g_orig_body_proc = (WNDPROC)SetWindowLongPtrA(
        g_a_body_edit, GWLP_WNDPROC, (LONG_PTR)BodyEditSub);

    GetClientRect(content, &rc);
    arpamail_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    g_a_controls_created = TRUE;

    /* Initial UI state: pre-connect. Only Server/Port/Connect are
     * reachable; To/Body/Clear/Send/Disconnect are grayed until the
     * user successfully connects. */
    arpa_set_connected_state(FALSE);
    SetFocus(g_a_server_edit);
}

void arpamail_module_deactivate(HWND content)
{
    (void)content;
    if (!g_a_controls_created) return;
    ShowWindow(g_a_server_lbl,      SW_HIDE);
    ShowWindow(g_a_server_edit,     SW_HIDE);
    ShowWindow(g_a_port_lbl,        SW_HIDE);
    ShowWindow(g_a_port_edit,       SW_HIDE);
    ShowWindow(g_a_connect_btn,     SW_HIDE);
    ShowWindow(g_a_disconnect_btn,  SW_HIDE);
    ShowWindow(g_a_proto_lbl,       SW_HIDE);
    ShowWindow(g_a_to_lbl,          SW_HIDE);
    ShowWindow(g_a_to_edit,         SW_HIDE);
    ShowWindow(g_a_body_lbl,        SW_HIDE);
    ShowWindow(g_a_body_edit,       SW_HIDE);
    ShowWindow(g_a_count_lbl,       SW_HIDE);
    ShowWindow(g_a_send_btn,        SW_HIDE);
    ShowWindow(g_a_clear_btn,       SW_HIDE);
}

/* ------------------------------------------------------------------ */
/* WM_COMMAND dispatch from buttons / edits.                           */
/* ------------------------------------------------------------------ */

BOOL arpamail_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id    = LOWORD(wParam);
    int notif = HIWORD(wParam);
    (void)lParam;

    switch (id) {
    case IDC_ARPA_SEND_BTN:
        if (notif == BN_CLICKED) { arpa_on_send(content); return TRUE; }
        break;
    case IDC_ARPA_CLEAR_BTN:
        if (notif == BN_CLICKED) { arpa_on_clear(content); return TRUE; }
        break;
    case IDC_ARPA_CONNECT_BTN:      /* the toggle: its label says which */
        if (notif == BN_CLICKED) {
            if (g_a_connected) {
                arpa_on_disconnect(content);
            } else {
                /* The connect is a synchronous banner probe on this thread
                 * (bounded by ARPA_NET_TIMEOUT), so it cannot be cancelled
                 * mid-way: the EXCEPTION to cancel-while-connecting. The
                 * toggle shows Disconnect, grayed, while it runs, and goes
                 * back to Connect if the probe fails. */
                w31_toggle_busy(g_a_connect_btn);
                UpdateWindow(g_a_connect_btn);
                arpa_on_connect(content);
                if (!g_a_connected) w31_toggle_set(g_a_connect_btn, FALSE);
            }
            return TRUE;
        }
        break;
    case IDC_ARPA_DISCONNECT_BTN:
        if (notif == BN_CLICKED) { arpa_on_disconnect(content); return TRUE; }
        break;
    case IDC_ARPA_BODY_EDIT:
        if (notif == EN_CHANGE) { arpa_update_byte_counter(); return TRUE; }
        break;
    }
    return FALSE;
}

BOOL arpamail_module_has_unsaved(void)
{
    /* State is preserved across switches; no destructive-switch prompt. */
    return FALSE;
}
