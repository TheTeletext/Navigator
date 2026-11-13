/*
 * wais_module.c - WAIS Search module: GUI + Z39.50-1988 orchestration.
 *
 * UI matches the standalone MGTWAIS x64 client (docs/wais2.png and
 * docs/wais3.png) per user editorial direction: 3 control rows above a
 * single multi-line output pane. This deviates from BLUEPRINT 4.2 (no
 * separate list + view pane) and BLUEPRINT 4.3 (server/port/db are
 * editable with a Connect button, not hardwired).
 *
 * The orchestration drives the preserved freeWAIS-sf 2.2.14 protocol
 * code in src/wais/ directly inline (Navigator has no subprocess split
 * per BLUEPRINT 2 "Backend binary model"), mirroring the cli_search /
 * cli_fetch pattern from MGTWAIS/source/src/wais_cli_engine.c.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

/* win_platform.h MUST come first: it includes winsock2.h before windows.h. */
#include "win_platform.h"

#include "wais_module.h"
#include "suite_w31.h"   /* the Connect/Disconnect toggle */
#include "suite_shell.h"

#include "wais/cdialect.h"
#include "wais/cutil.h"
#include "wais/zutil.h"
#include "wais/wprot.h"
#include "wais/wutil.h"
#include "wais/wmessage.h"
#include "wais/sockets.h"
#include "wais/ui.h"

/* ztype1.h (via ui.h) does `#define WORD "sw"` for the Z39.50-1988 type-code
 * system, which clobbers the Windows WORD typedef and breaks LOWORD/HIWORD
 * macros used in WM_COMMAND handling. Restore Windows definition; the
 * protocol .c files are compiled separately and never see windows.h. */
#undef WORD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* freeWAIS-sf transport uses g_wais_socket as the active socket. The
 * background probe writes to this directly so init_connection and
 * interpret_message can recv on it. */
extern SOCKET g_wais_socket;

/* Server bindings: defaults at startup. Editable via the Server/Port/
 * Database fields and the Connect button. */
#define WAIS_DEFAULT_HOST    "wais.teletext.ca"
#define WAIS_DEFAULT_PORT    "210"
#define WAIS_DEFAULT_DB      "teletext"
#define WAIS_MAX_DOCS        40
#define WAIS_CHARS_PER_PAGE  10000
#define WAIS_DOC_MAX_BYTES   (1024 * 1024)

/* Module-local control IDs (1000+). */
#define IDC_WAIS_SEARCH_EDIT     1001
#define IDC_WAIS_SEARCH_BTN      1002
#define IDC_WAIS_SERVER_EDIT     1003
#define IDC_WAIS_PORT_EDIT       1004
#define IDC_WAIS_DB_EDIT         1005
#define IDC_WAIS_CONNECT_BTN     1006
#define IDC_WAIS_BACK_BTN        1007
#define IDC_WAIS_FWD_BTN         1008
#define IDC_WAIS_DOC_EDIT        1009
#define IDC_WAIS_VIEW_BTN        1010
#define IDC_WAIS_OUTPUT          1011
#define IDC_WAIS_DISCONNECT_BTN  1012

/* Live control handles. */
static HWND g_w_search_lbl, g_w_search_edit, g_w_search_btn;
static HWND g_w_server_lbl, g_w_server_edit;
static HWND g_w_port_lbl,   g_w_port_edit;
static HWND g_w_db_lbl,     g_w_db_edit;
static HWND g_w_connect_btn, g_w_disconnect_btn;   /* 2026-11-11: Disconnect
                                                     retired (one toggle); NULL */
static HWND g_w_proto_lbl;
static HWND g_w_back_btn, g_w_fwd_btn;
static HWND g_w_doc_lbl, g_w_doc_edit;
static HWND g_w_view_btn;
static HWND g_w_output;

/* UI session gate. The Z39.50-1988 wire is stateless (each Search/View
 * opens its own connection), but the UI exposes a Connect/Disconnect
 * session like ARPANET FTP-Mail. Pre-connect: only Server/Port/Database
 * /Connect reachable. Connected: Search/Doc/buttons/Disconnect reach-
 * able, Server/Port/Database/Connect locked. */
static BOOL g_w_connected = FALSE;

/* Subclasses. */
static WNDPROC g_orig_search_proc = NULL;
static WNDPROC g_orig_doc_proc    = NULL;
static WNDPROC g_orig_output_proc = NULL;
static WNDPROC g_orig_field_proc  = NULL;   /* shared by server/port/db */

/* True once the controls have been created. On subsequent activations we
 * just ShowWindow them back so all in-progress state (search query,
 * cached results, doc body, server/port/db values, scroll position)
 * survives module switches. */
static BOOL g_w_controls_created = FALSE;

/* Active server bindings (committed by Connect, used by Search/View). */
static char g_w_active_host[256];
static char g_w_active_port[16];
static char g_w_active_db[128];

/* Cached views and search response. */
static char *g_w_results_text  = NULL;   /* formatted results listing */
static char *g_w_document_text = NULL;   /* formatted document body  */
static int   g_w_current_view  = 0;      /* 1=results, 2=document */

static SearchResponseAPDU *g_w_response = NULL;
static WAISSearchResponse *g_w_info     = NULL;
static int                 g_w_count    = 0;

/* Forward declarations. */
static void wais_on_search(HWND content);
static void wais_on_view(HWND content);
static void wais_on_connect(HWND content);
static void wais_on_disconnect(HWND content);
static void wais_show_view(int which);
static void wais_set_output_text(const char *utf8);
static void wais_clear_results(void);
static void wais_status(HWND content, const char *text);
static void wais_set_connected_state(BOOL connected);
static void wais_cancel_job(BOOL discard);
static int  wais_job_running(void);

/* ------------------------------------------------------------------ */
/* Status line helper: set and flush so the user sees the transition. */
/* ------------------------------------------------------------------ */

static void wais_status(HWND content, const char *text)
{
    HWND main_window;
    suite_set_status(text);
    main_window = GetParent(content);
    if (main_window) UpdateWindow(main_window);
}

/* ------------------------------------------------------------------ */
/* Subclass: search edit. Enter triggers Search.                       */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK SearchEditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_orig_search_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN &&
            (pmsg->wParam == VK_RETURN ||
             pmsg->wParam == VK_UP    || pmsg->wParam == VK_DOWN ||
             pmsg->wParam == VK_PRIOR || pmsg->wParam == VK_NEXT ||
             (pmsg->wParam == VK_ESCAPE && wais_job_running())))
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_RETURN: wais_on_search(GetParent(h)); return 0;
        case VK_ESCAPE: if (wais_job_running()) { wais_cancel_job(FALSE); wais_status(GetParent(h), "Cancelling..."); } return 0;
        /* Up/Down/PgUp/PgDn forward to the output pane so the user can
         * scroll the body without first clicking inside it. Left/Right
         * stay normal so the caret can move within the text field. */
        case VK_UP:    if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEUP,   0); return 0;
        case VK_DOWN:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
        case VK_PRIOR: if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEUP,   0); return 0;
        case VK_NEXT:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
        }
    }
    if (m == WM_CHAR && w == '\r') return 0;
    return CallWindowProcA(g_orig_search_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Subclass: output edit. Arrow / page / home / end keys scroll the   */
/* view instead of moving the caret; caret is hidden on focus so the  */
/* output pane reads as a read-only viewer, not an editor.            */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK OutputSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_GETDLGCODE: {
        /* Claim arrow keys so the (now-bypassed) IsDialogMessage path
         * would never steal them even if the shell's focus check ever
         * fails. */
        LRESULT base = CallWindowProcA(g_orig_output_proc, h, m, w, l);
        return base | DLGC_WANTARROWS;
    }
    case WM_SETFOCUS: {
        LRESULT r = CallWindowProcA(g_orig_output_proc, h, m, w, l);
        HideCaret(h);
        return r;
    }
    case WM_LBUTTONDOWN: {
        LRESULT r = CallWindowProcA(g_orig_output_proc, h, m, w, l);
        HideCaret(h);
        return r;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_UP:    SendMessageA(h, WM_VSCROLL, SB_LINEUP,    0); return 0;
        case VK_DOWN:  SendMessageA(h, WM_VSCROLL, SB_LINEDOWN,  0); return 0;
        case VK_PRIOR: SendMessageA(h, WM_VSCROLL, SB_PAGEUP,    0); return 0;
        case VK_NEXT:  SendMessageA(h, WM_VSCROLL, SB_PAGEDOWN,  0); return 0;
        case VK_HOME:  SendMessageA(h, WM_VSCROLL, SB_TOP,       0); return 0;
        case VK_END:   SendMessageA(h, WM_VSCROLL, SB_BOTTOM,    0); return 0;
        case VK_LEFT:  SendMessageA(h, WM_HSCROLL, SB_LINELEFT,  0); return 0;
        case VK_ESCAPE: if (wais_job_running()) { wais_cancel_job(FALSE); wais_status(GetParent(h), "Cancelling..."); } return 0;
        case VK_RIGHT: SendMessageA(h, WM_HSCROLL, SB_LINERIGHT, 0); return 0;
        }
        break;
    case WM_CONTEXTMENU: {
        /* Replace the default edit context menu (which lists Cut/Copy/
         * Paste/Delete/Select All, with Cut/Paste/Delete grayed out on
         * a read-only edit) with a clean two-item viewer menu. Paste is
         * never offered, even as a disabled item. */
        HMENU menu = CreatePopupMenu();
        DWORD sel_start = 0, sel_end = 0;
        BOOL has_sel;
        POINT pt;
        int cmd;

        SendMessageA(h, EM_GETSEL, (WPARAM)&sel_start, (LPARAM)&sel_end);
        has_sel = (sel_end > sel_start);

        AppendMenuA(menu, MF_STRING | (has_sel ? 0 : MF_GRAYED), 1, "Copy\tCtrl+C");
        AppendMenuA(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuA(menu, MF_STRING, 2, "Select All\tCtrl+A");

        pt.x = (int)(short)LOWORD(l);
        pt.y = (int)(short)HIWORD(l);
        if (pt.x == -1 && pt.y == -1) {
            /* Keyboard-invoked (Shift+F10): position near the control. */
            RECT rc;
            GetWindowRect(h, &rc);
            pt.x = rc.left + 40;
            pt.y = rc.top + 40;
        }
        cmd = TrackPopupMenu(menu,
                             TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN,
                             pt.x, pt.y, 0, h, NULL);
        DestroyMenu(menu);

        if (cmd == 1) SendMessageA(h, WM_COPY, 0, 0);
        else if (cmd == 2) SendMessageA(h, EM_SETSEL, 0, (LPARAM)-1);
        return 0;
    }
    }
    return CallWindowProcA(g_orig_output_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Subclass: generic field edit. Up/Down/PgUp/PgDn forward to the      */
/* output pane's vertical scroll so the user can scroll the body       */
/* without leaving a Server/Port/Database field. Left/Right pass       */
/* through to the default proc for in-field cursor movement.           */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK FieldScrollSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_orig_field_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN &&
            (pmsg->wParam == VK_UP    || pmsg->wParam == VK_DOWN ||
             pmsg->wParam == VK_PRIOR || pmsg->wParam == VK_NEXT))
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_UP:    if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEUP,   0); return 0;
        case VK_DOWN:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
        case VK_PRIOR: if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEUP,   0); return 0;
        case VK_NEXT:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
        }
    }
    return CallWindowProcA(g_orig_field_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Subclass: doc # edit. Enter triggers View.                          */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK DocEditSub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETDLGCODE) {
        MSG *pmsg = (MSG *)l;
        LRESULT base = CallWindowProcA(g_orig_doc_proc, h, m, w, l);
        if (pmsg && pmsg->message == WM_KEYDOWN &&
            (pmsg->wParam == VK_RETURN ||
             pmsg->wParam == VK_UP    || pmsg->wParam == VK_DOWN ||
             pmsg->wParam == VK_PRIOR || pmsg->wParam == VK_NEXT ||
             (pmsg->wParam == VK_ESCAPE && wais_job_running())))
            return base | DLGC_WANTMESSAGE;
        return base;
    }
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_RETURN: wais_on_view(GetParent(h)); return 0;
        case VK_ESCAPE: if (wais_job_running()) { wais_cancel_job(FALSE); wais_status(GetParent(h), "Cancelling..."); } return 0;
        case VK_UP:    if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEUP,   0); return 0;
        case VK_DOWN:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
        case VK_PRIOR: if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEUP,   0); return 0;
        case VK_NEXT:  if (g_w_output) SendMessageA(g_w_output, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
        }
    }
    if (m == WM_CHAR && w == '\r') return 0;
    return CallWindowProcA(g_orig_doc_proc, h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Free cached state.                                                  */
/* ------------------------------------------------------------------ */

static void wais_clear_results(void)
{
    if (g_w_response) {
        freeWAISSearchResponse(g_w_response->DatabaseDiagnosticRecords);
        freeSearchResponseAPDU(g_w_response);
        g_w_response = NULL;
        g_w_info     = NULL;
    }
    g_w_count = 0;
    if (g_w_results_text)  { free(g_w_results_text);  g_w_results_text  = NULL; }
    if (g_w_document_text) { free(g_w_document_text); g_w_document_text = NULL; }
    g_w_current_view = 0;
    if (g_w_output) SetWindowTextA(g_w_output, "");
}

/* ------------------------------------------------------------------ */
/* Render the formatted output: convert UTF-8 to wide, replace \x01    */
/* markers with U+2588 (FULL BLOCK), and SetWindowTextW.               */
/* ------------------------------------------------------------------ */

static void wais_set_output_text(const char *utf8)
{
    int      src_len, wide_len;
    wchar_t *wide;
    int      i;

    if (!utf8 || !*utf8) { SetWindowTextW(g_w_output, L""); return; }

    src_len = (int)strlen(utf8);
    wide_len = MultiByteToWideChar(CP_UTF8, 0, utf8, src_len, NULL, 0);
    if (wide_len <= 0) { SetWindowTextA(g_w_output, utf8); return; }

    /* Reserve 2 extra wchars for the leading blank line ("\r\n") that
     * gives the body one full line of breathing room from the top edge
     * of the output pane, plus 1 for the NUL terminator. */
    wide = (wchar_t *)malloc((size_t)(wide_len + 3) * sizeof(wchar_t));
    if (!wide) { SetWindowTextA(g_w_output, utf8); return; }

    wide[0] = L'\r';
    wide[1] = L'\n';
    MultiByteToWideChar(CP_UTF8, 0, utf8, src_len, wide + 2, wide_len);
    wide[wide_len + 2] = L'\0';

    /* Replace \x01 markers with U+2588 FULL BLOCK for score bar render. */
    for (i = 2; i < wide_len + 2; i++) {
        if (wide[i] == 0x01) wide[i] = 0x2588;
    }

    SetWindowTextW(g_w_output, wide);
    free(wide);

    /* Scroll the view to the top so the first line is visible after a
     * fresh text load (defaults to bottom or partial first-line clip
     * under some themes). EM_SETSEL + EM_SCROLLCARET puts the caret at
     * offset 0 and scrolls accordingly. */
    SendMessageA(g_w_output, EM_SETSEL, 0, 0);
    SendMessageA(g_w_output, EM_SCROLLCARET, 0, 0);
}

/* ------------------------------------------------------------------ */
/* Switch the output pane between cached Results and Document views.   */
/* ------------------------------------------------------------------ */

static void wais_show_view(int which)
{
    if (which == 1 && g_w_results_text) {
        wais_set_output_text(g_w_results_text);
        g_w_current_view = 1;
    } else if (which == 2 && g_w_document_text) {
        wais_set_output_text(g_w_document_text);
        g_w_current_view = 2;
    }
}

/* ------------------------------------------------------------------ */
/* Connect: commit edits from server/port/db fields to active state.  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Toggle the UI between pre-connect and connected states. Mirrors    */
/* the ARPANET FTP-Mail session gate. Pre-connect: only Server, Port, */
/* Database, Connect reachable. Connected: everything else reachable, */
/* Server/Port/Database/Connect locked.                                */
/* ------------------------------------------------------------------ */

static void wais_set_connected_state(BOOL connected)
{
    g_w_connected = connected;

    EnableWindow(g_w_server_edit,    !connected);
    EnableWindow(g_w_port_edit,      !connected);
    EnableWindow(g_w_db_edit,        !connected);
    /* 2026-11-11: one Connect/Disconnect toggle (docs/2026-11-11_W31_BUTTONS.md). */
    w31_toggle_set(g_w_connect_btn, connected);
    EnableWindow(g_w_search_edit,     connected);
    EnableWindow(g_w_search_btn,      connected);
    EnableWindow(g_w_doc_edit,        connected);
    EnableWindow(g_w_view_btn,        connected);
    EnableWindow(g_w_back_btn,        connected);
    EnableWindow(g_w_fwd_btn,         connected);
}

/* ------------------------------------------------------------------ */
/* Backgrounded Connect probe - implementation infrastructure.        */
/*                                                                     */
/* The freeWAIS-sf 2.2.x recv path is blocking with no timeouts. If a */
/* non-WAIS service (e.g. Google's HTTP front end on port 80) accepts */
/* the TCP connection but never speaks Z39.50-1988, the UI thread     */
/* would hang for minutes on the Init recv. The fix is twofold:        */
/*  (1) all probe network I/O runs on a worker thread, never the UI   */
/*      thread; the UI stays responsive throughout.                    */
/*  (2) every socket operation has a bounded timeout:                  */
/*      - TCP connect: WAIS_TIMEOUT_SECONDS (non-blocking + select).  */
/*      - Init recv:   WAIS_TIMEOUT_SECONDS (via SO_RCVTIMEO).         */
/*      - Search recv: WAIS_TIMEOUT_SECONDS (via SO_RCVTIMEO).         */
/* Cumulative max ~9s, well under the "about 8 seconds" backstop.     */
/*                                                                     */
/* Communication: worker writes outcome + status_msg into a heap-     */
/* allocated WAISProbeCtx, then InterlockedExchange's the `done` flag */
/* to 1. The UI thread polls via a SetTimer callback at 100ms and     */
/* processes the result when done == 1, then frees the ctx.            */
/* ------------------------------------------------------------------ */

#define WAIS_TIMEOUT_SECONDS  3
#define WAIS_PROBE_TIMER_ID   0xA001

typedef struct WAISProbeCtx {
    HWND          content;
    char          host[256];
    int           port;
    char          db[128];

    /* Worker writes these, then InterlockedExchange's `done` to 1.   */
    volatile LONG done;
    int           outcome;        /* 0 = connected; nonzero = failure */
    char          status_msg[400];
} WAISProbeCtx;

static WAISProbeCtx *g_w_probe          = NULL;
static UINT_PTR      g_w_probe_timer_id = 0;

/* TCP connect with bounded timeout. Returns INVALID_SOCKET on any
 * failure (refused, timed out, DNS). Same shape as arpa_connect_timeout
 * in the ARPANET module. */
static SOCKET wais_connect_timeout(const char *host, int port, int seconds)
{
    SOCKET             s;
    struct sockaddr_in addr;
    struct hostent    *he;
    unsigned long      a;
    u_long             mode;
    int                rc;
    fd_set             wfds, efds;
    struct timeval     tv;
    int                err = 0, errlen = sizeof(err), sel;

    memset(&addr, 0, sizeof(addr));
    a = inet_addr(host);
    if (a != INADDR_NONE) {
        addr.sin_addr.s_addr = a;
        addr.sin_family      = AF_INET;
    } else {
        he = gethostbyname(host);
        if (!he) return INVALID_SOCKET;
        addr.sin_family = he->h_addrtype;
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    addr.sin_port = htons((unsigned short)port);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }

    rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (rc != 0) {
        FD_ZERO(&wfds); FD_SET(s, &wfds);
        FD_ZERO(&efds); FD_SET(s, &efds);
        tv.tv_sec  = seconds;
        tv.tv_usec = 0;
        sel = select(0, NULL, &wfds, &efds, &tv);
        if (sel <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) != 0 || err != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    mode = 0;
    ioctlsocket(s, FIONBIO, &mode);
    return s;
}

/* Worker thread: runs the three-stage probe. ZERO UI access from here;
 * results go through ctx, polled by the UI thread. */
static DWORD WINAPI wais_probe_worker(LPVOID arg)
{
    WAISProbeCtx       *ctx        = (WAISProbeCtx *)arg;
    SOCKET              s          = INVALID_SOCKET;
    char               *req        = NULL, *resp = NULL;
    long                msglen     = BUFSZ, reqlen;
    long                init_result;
    char                userInfo[500], hostname[80];
    DWORD               tmo_ms     = WAIS_TIMEOUT_SECONDS * 1000;
    SearchResponseAPDU *response   = NULL;

    /* Stage (a): TCP connect with bounded timeout. */
    s = wais_connect_timeout(ctx->host, ctx->port, WAIS_TIMEOUT_SECONDS);
    if (s == INVALID_SOCKET) {
        ctx->outcome = 1;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Cannot connect to %s:%d.", ctx->host, ctx->port);
        goto done;
    }

    /* All recvs from here on are bounded by SO_RCVTIMEO. */
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo_ms, sizeof(tmo_ms));

    /* Wire into freeWAIS-sf: init_connection and interpret_message read
     * g_wais_socket internally. The FILE* arg is ignored by the Win
     * port (see sockets_win.c). */
    g_wais_socket = s;

    req  = (char *)s_malloc((size_t)msglen);
    resp = (char *)s_malloc((size_t)msglen);
    if (!req || !resp) {
        ctx->outcome = 4;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg), "Out of memory.");
        goto cleanup;
    }

    mygethostname(hostname, 80);
    _snprintf(userInfo, sizeof(userInfo),
              SUITE_UA_PRODUCT "/" SUITE_VERSION_STR " %s", hostname);

    /* Stage (b): Init exchange. recv is bounded by SO_RCVTIMEO; if the
     * peer never speaks (HTTP front end, etc.), init_connection returns
     * a non-positive result after ~WAIS_TIMEOUT_SECONDS. */
    init_result = init_connection(req, resp, msglen, stderr, userInfo);
    if (init_result <= 0) {
        ctx->outcome = 2;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "%s:%d does not speak WAIS Z39.50-1988.",
                  ctx->host, ctx->port);
        goto cleanup;
    }

    /* Stage (c): probe Search against the typed database. Single-letter
     * benign query, 1-record limit. Branch on SearchResponseAPDU
     * .SearchStatus per zprot.h: TRUE = db accepted (regardless of hit
     * count); FALSE / no-response / parse-fail = db invalid. */
    reqlen = msglen;
    if (!generate_search_apdu(req + HEADER_LENGTH, &reqlen,
                              "x", ctx->db, NULL, (long)1)) {
        ctx->outcome = 4;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Failed to build probe Search Request.");
        goto cleanup;
    }
    if (interpret_message(req, msglen - reqlen, resp, msglen,
                          stderr, (boolean)false) == 0) {
        ctx->outcome = 3;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Database \"%s\" not found on %s:%d.",
                  ctx->db, ctx->host, ctx->port);
        goto cleanup;
    }

    readSearchResponseAPDU(&response, resp + HEADER_LENGTH);
    if (response && response->SearchStatus) {
        ctx->outcome = 0;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Connected to %s:%d, database \"%s\".",
                  ctx->host, ctx->port, ctx->db);
    } else {
        ctx->outcome = 3;
        _snprintf(ctx->status_msg, sizeof(ctx->status_msg),
                  "Database \"%s\" not found on %s:%d.",
                  ctx->db, ctx->host, ctx->port);
    }
    if (response) freeSearchResponseAPDU(response);

cleanup:
    if (req)  s_free(req);
    if (resp) s_free(resp);
    if (s != INVALID_SOCKET) closesocket(s);
    g_wais_socket = INVALID_SOCKET;

done:
    InterlockedExchange(&ctx->done, 1);
    return 0;
}

/* UI-thread polling timer: triggers on a 100ms cadence while a probe
 * is in flight. When the worker has set done = 1, this fires the UI
 * transition exactly once and frees the ctx. */
static void CALLBACK wais_probe_timer_proc(HWND hwnd, UINT msg,
                                           UINT_PTR id, DWORD time)
{
    WAISProbeCtx *ctx;
    (void)msg; (void)time;

    if (!g_w_probe || !g_w_probe->done) return;

    KillTimer(hwnd, id);
    g_w_probe_timer_id = 0;

    ctx = g_w_probe;
    g_w_probe = NULL;

    wais_status(ctx->content, ctx->status_msg);

    if (ctx->outcome == 0) {
        wais_set_connected_state(TRUE);
        SetFocus(g_w_search_edit);
    } else {
        /* Failure path: stay pre-connect, the toggle back to Connect. */
        w31_toggle_set(g_w_connect_btn, FALSE);
    }

    free(ctx);
}

/* ------------------------------------------------------------------ */
/* Connect: three-stage reachability + validity probe.                */
/*                                                                     */
/*  (a) TCP connect to server:port. On failure: stay pre-connect,     */
/*      local message "Cannot connect to <server>:<port>."             */
/*  (b) Z39.50-1988 Init exchange. On failure: stay pre-connect,      */
/*      local message "<server>:<port> does not speak WAIS            */
/*      Z39.50-1988."                                                  */
/*  (c) Probe Search Request against the typed database with a benign */
/*      single-letter query and a 1-record limit. If the server       */
/*      returns no response, returns SearchStatus = FALSE, or fails    */
/*      to parse, the database is invalid: stay pre-connect, local    */
/*      message "Database \"<db>\" not found on <server>:<port>."     */
/*      Z39.50-1988 carries no database name in Init, so a real        */
/*      Search exchange is the only honest way to validate the db.    */
/*      A valid database with zero hits still reads as connected      */
/*      because the server returns a well-formed SearchResponse with  */
/*      SearchStatus = TRUE and ResultCount = 0.                       */
/*                                                                     */
/* Only after all three stages pass does the UI flip to connected.    */
/* The probe socket closes immediately; Search and View still open    */
/* their own per-operation sockets, same stateless pattern as the     */
/* ARPANET FTP-Mail module.                                            */
/* ------------------------------------------------------------------ */

static void wais_on_connect(HWND content)
{
    char          host[256], port[16], db[128];
    int           port_int;
    HANDLE        thr;
    WAISProbeCtx *ctx;

    /* Single probe in flight at a time. The Connect button is also */
    /* disabled below; this is the belt to that suspenders.          */
    if (g_w_probe) return;
    if (wais_job_running()) {
        wais_status(content, "WAIS is busy: press Esc to cancel.");
        return;
    }

    GetWindowTextA(g_w_server_edit, host, sizeof(host));
    GetWindowTextA(g_w_port_edit,   port, sizeof(port));
    GetWindowTextA(g_w_db_edit,     db,   sizeof(db));

    if (!host[0] || !port[0] || !db[0]) {
        wais_status(content, "Server, port, and database are required.");
        return;
    }
    port_int = atoi(port);
    if (port_int <= 0 || port_int > 65535) {
        wais_status(content, "Port must be 1-65535.");
        return;
    }

    strncpy(g_w_active_host, host, sizeof(g_w_active_host) - 1);
    strncpy(g_w_active_port, port, sizeof(g_w_active_port) - 1);
    strncpy(g_w_active_db,   db,   sizeof(g_w_active_db)   - 1);
    g_w_active_host[sizeof(g_w_active_host)-1] = '\0';
    g_w_active_port[sizeof(g_w_active_port)-1] = '\0';
    g_w_active_db[sizeof(g_w_active_db)-1]     = '\0';

    ctx = (WAISProbeCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        wais_status(content, "Out of memory.");
        return;
    }
    ctx->content = content;
    ctx->port    = port_int;
    strncpy(ctx->host, g_w_active_host, sizeof(ctx->host) - 1);
    strncpy(ctx->db,   g_w_active_db,   sizeof(ctx->db)   - 1);

    g_w_probe = ctx;

    /* The toggle reads Disconnect but stays grayed while the probe runs:
     * the EXCEPTION to cancel-while-connecting (2026-11-11). The probe is
     * bounded (about 9 s) but runs freeWAIS on the process-global
     * g_wais_socket, so abandoning it would let a second probe race the
     * first on that socket. The UI stays responsive (worker thread). */
    w31_toggle_busy(g_w_connect_btn);
    wais_status(content, "Connecting...");

    thr = CreateThread(NULL, 0, wais_probe_worker, ctx, 0, NULL);
    if (!thr) {
        wais_status(content, "Cannot start probe thread.");
        w31_toggle_set(g_w_connect_btn, FALSE);
        free(ctx);
        g_w_probe = NULL;
        return;
    }
    CloseHandle(thr);   /* detach; worker runs to completion on its own */

    /* Poll the worker's done flag at 100ms; the timer proc handles UI
     * transition exactly once when the worker finishes. */
    g_w_probe_timer_id = SetTimer(content, WAIS_PROBE_TIMER_ID,
                                  100, wais_probe_timer_proc);
}

/* ------------------------------------------------------------------ */
/* Disconnect: stateless wire (nothing to QUIT in Z39.50-1988), so    */
/* this is purely a UI reset. Clears search/Doc#/output pane and any  */
/* cached search/document state. Returns the UI to pre-connect with   */
/* Server/Port/Database editable again.                                */
/* ------------------------------------------------------------------ */

static void wais_on_disconnect(HWND content)
{
    /* a search or view in flight: cancel it and drop its result */
    wais_cancel_job(TRUE);
    SetWindowTextA(g_w_search_edit, "");
    SetWindowTextA(g_w_doc_edit,    "");
    SetWindowTextA(g_w_output,      "");

    wais_clear_results();
    g_w_current_view = 0;

    wais_status(content, "Disconnected.");
    wais_set_connected_state(FALSE);
    SetFocus(g_w_server_edit);
}

/* ------------------------------------------------------------------ */
/* Search: open Z39.50-1988, send Search APDU, parse, render results. */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Search and document view on a worker thread (2026-11-11,           */
/* docs/2026-11-11_STATUS_PROGRESS.md). They used to run on the UI    */
/* thread, freezing the window for the whole exchange. Now one WAIS   */
/* job at a time (search OR view; never while the Connect probe runs: */
/* all three use freeWAIS's single g_wais_socket) runs on a worker;   */
/* a 100 ms timer on the UI thread shows its stage and, for a view,   */
/* a true percentage against the document's DocumentLength, and       */
/* applies the result exactly once when it is done.                   */
/*   Cancel: Esc (search field, Doc # field, output pane), and        */
/*   Disconnect. The UI thread never closes the socket (the worker    */
/*   owns it): it sets the job's cancel flag and shutdown()s the      */
/*   socket, which makes the worker's blocked recv return; the worker */
/*   then closes it itself through close_connection as always.        */
/*   A view job works on its own copy of the document id, so a        */
/*   Disconnect that clears the results cannot free it under it.      */
/* ------------------------------------------------------------------ */

#define WAIS_JOB_TIMER_ID  0xA002
#define WAIS_JOB_SEARCH    1
#define WAIS_JOB_VIEW      2

typedef struct WAISJob {
    int           kind;
    HWND          content;
    char          host[256];
    int           port;
    char          db[128];
    char          query[1024];            /* search */
    any          *docID;                  /* view: the job's own copy */
    long          doc_size;               /* view: DocumentLength, 0 unknown */
    char          type[64];               /* view */

    volatile LONG done;
    volatile LONG cancel;
    volatile LONG stage;                  /* 1 connecting, 2 init, 3 searching / loading */
    volatile LONG got;                    /* view: bytes received so far */

    int           outcome;                /* 0 ok, 1 failed, 2 cancelled */
    char          status_msg[200];
    SearchResponseAPDU *response;         /* search result, for the UI */
    char         *body;                   /* view result, raw */
    size_t        body_len;

    BOOL          discard;                /* UI only: Disconnect came; drop the result */
    LONG          shown_stage, shown_got; /* UI only */
} WAISJob;

static WAISJob  *g_w_job          = NULL;
static UINT_PTR  g_w_job_timer_id = 0;

static int wais_job_running(void) { return g_w_job != NULL; }

static BOOL wais_tab_active(void) { return g_w_output && IsWindowVisible(g_w_output); }

/* Status text only while the WAIS tab is the one showing. */
static void wais_status_if_active(HWND content, const char *text)
{
    if (wais_tab_active()) wais_status(content, text);
}

static void wais_job_failed(WAISJob *j, const char *msg)
{
    j->outcome = InterlockedCompareExchange(&j->cancel, 0, 0) ? 2 : 1;
    strncpy(j->status_msg, msg, sizeof j->status_msg - 1);
    j->status_msg[sizeof j->status_msg - 1] = '\0';
}

/* Connect and Z39.50-1988 Init; NULL (and the job marked failed) on error. */
static FILE *wais_job_open(WAISJob *j, char *req, char *resp, long msglen)
{
    FILE *conn;
    char  userInfo[500], hostname[80], m[200];
    InterlockedExchange(&j->stage, 1);
    conn = connect_to_server(j->host, (long)j->port);
    if (!conn) {
        _snprintf(m, sizeof m, "Cannot connect to %s:%d.", j->host, j->port);
        m[sizeof m - 1] = '\0';
        wais_job_failed(j, m);
        return NULL;
    }
    if (InterlockedCompareExchange(&j->cancel, 0, 0)) {
        close_connection(conn);
        wais_job_failed(j, "Cancelled.");
        return NULL;
    }
    mygethostname(hostname, 80);
    _snprintf(userInfo, sizeof(userInfo),
              SUITE_UA_PRODUCT "/" SUITE_VERSION_STR " %s", hostname);
    InterlockedExchange(&j->stage, 2);
    if (init_connection(req, resp, msglen, conn, userInfo) <= 0) {
        close_connection(conn);
        wais_job_failed(j, "Z39.50-1988 Init failed.");
        return NULL;
    }
    return conn;
}

static void wais_job_search(WAISJob *j)
{
    long  msglen = BUFSZ, reqlen;
    char *req  = (char *)s_malloc((size_t)msglen);
    char *resp = (char *)s_malloc((size_t)msglen);
    FILE *conn;
    SearchResponseAPDU *response = NULL;
    if (!req || !resp) { wais_job_failed(j, "Out of memory."); goto out; }
    conn = wais_job_open(j, req, resp, msglen);
    if (!conn) goto out;
    InterlockedExchange(&j->stage, 3);
    reqlen = msglen;
    if (!generate_search_apdu(req + HEADER_LENGTH, &reqlen,
                              j->query, j->db, NULL, (long)WAIS_MAX_DOCS)) {
        wais_job_failed(j, "Failed to generate Search APDU.");
        close_connection(conn);
        goto out;
    }
    if (interpret_message(req, msglen - reqlen, resp, msglen,
                          conn, (boolean)false) == 0) {
        wais_job_failed(j, "No response from server.");
        close_connection(conn);
        goto out;
    }
    readSearchResponseAPDU(&response, resp + HEADER_LENGTH);
    close_connection(conn);
    j->response = response;
    j->outcome = InterlockedCompareExchange(&j->cancel, 0, 0) ? 2 : 0;
out:
    if (req)  s_free(req);
    if (resp) s_free(resp);
}

static void wais_job_view(WAISJob *j)
{
    long     msglen = BUFSZ, reqlen, count;
    char    *req  = (char *)s_malloc((size_t)msglen);
    char    *resp = (char *)s_malloc((size_t)msglen);
    FILE    *conn = NULL;
    char    *body = NULL;
    size_t   body_len = 0, body_cap;
    boolean  skipped_separator = (boolean)false;
    SearchResponseAPDU *rr = NULL;
    WAISSearchResponse *ri;
    long     doc_size = j->doc_size;

    if (!req || !resp) { wais_job_failed(j, "Out of memory."); goto out; }
    conn = wais_job_open(j, req, resp, msglen);
    if (!conn) goto out;
    InterlockedExchange(&j->stage, 3);

    body_cap = (doc_size > 0 && doc_size < WAIS_DOC_MAX_BYTES)
               ? (size_t)doc_size + 1024 : 16384;
    body = (char *)malloc(body_cap);
    if (!body) { wais_job_failed(j, "Out of memory."); close_connection(conn); goto out; }

    for (count = 0;
         (doc_size == 0) || (count * WAIS_CHARS_PER_PAGE < doc_size);
         count++)
    {
        long end_byte = (doc_size > 0)
                        ? MINIMUM((count + 1) * WAIS_CHARS_PER_PAGE, doc_size)
                        : (count + 1) * WAIS_CHARS_PER_PAGE;

        if (InterlockedCompareExchange(&j->cancel, 0, 0)) break;
        reqlen = msglen;
        if (!generate_retrieval_apdu(req + HEADER_LENGTH, &reqlen,
                                     j->docID, CT_byte,
                                     count * WAIS_CHARS_PER_PAGE, end_byte,
                                     j->type, j->db))
            break;

        if (interpret_message(req, msglen - reqlen, resp, msglen,
                              conn, (boolean)false) == 0)
            break;

        readSearchResponseAPDU(&rr, resp + HEADER_LENGTH);
        ri = rr ? (WAISSearchResponse *)rr->DatabaseDiagnosticRecords : NULL;

        if (ri && ri->Text) {
            WAISDocumentText *text = ri->Text[0];
            if (text && text->DocumentText) {
                char *bytes = text->DocumentText->bytes;
                long  size  = text->DocumentText->size;
                long  start = 0, k;

                if (count == 0 && !skipped_separator) {
                    /* Keep from the first "----" record-separator line, so
                     * the document's own header box renders as a frame
                     * (the standalone cli_engine pattern). */
                    for (k = 0; k < size - 4; k++) {
                        if (bytes[k]=='-' && bytes[k+1]=='-' &&
                            bytes[k+2]=='-' && bytes[k+3]=='-') {
                            start = k;
                            break;
                        }
                    }
                    skipped_separator = (boolean)true;
                }

                if (size > start) {
                    size_t add = (size_t)(size - start);
                    if (body_len + add + 1 > body_cap) {
                        size_t new_cap = body_cap * 2;
                        char *nb;
                        while (new_cap < body_len + add + 1) new_cap *= 2;
                        if (new_cap > WAIS_DOC_MAX_BYTES) new_cap = WAIS_DOC_MAX_BYTES;
                        if (body_len + add + 1 > new_cap) add = new_cap - body_len - 1;
                        nb = (char *)realloc(body, new_cap);
                        if (nb) { body = nb; body_cap = new_cap; } else break;
                    }
                    memcpy(body + body_len, bytes + start, add);
                    body_len += add;
                }
            }
        }

        if (rr) {
            freeWAISSearchResponse(rr->DatabaseDiagnosticRecords);
            freeSearchResponseAPDU(rr);
            rr = NULL;
        }

        /* progress: the document's bytes requested so far, against its
         * DocumentLength (a true total) */
        InterlockedExchange(&j->got, (LONG)end_byte);

        if (doc_size == 0) break;
        if (body_len >= WAIS_DOC_MAX_BYTES) break;
    }

    if (rr) {
        freeWAISSearchResponse(rr->DatabaseDiagnosticRecords);
        freeSearchResponseAPDU(rr);
    }
    close_connection(conn);
    if (InterlockedCompareExchange(&j->cancel, 0, 0)) {
        free(body);
        wais_job_failed(j, "Cancelled.");
        goto out;
    }
    j->body = body;
    j->body_len = body_len;
    j->outcome = 0;
out:
    if (req)  s_free(req);
    if (resp) s_free(resp);
}

static DWORD WINAPI wais_job_worker(LPVOID arg)
{
    WAISJob *j = (WAISJob *)arg;
    if (j->kind == WAIS_JOB_SEARCH) wais_job_search(j);
    else                            wais_job_view(j);
    InterlockedExchange(&j->done, 1);
    return 0;
}

/* Cancel the running job, if any: flag it and unblock its recv. */
static void wais_cancel_job(BOOL discard)
{
    if (!g_w_job) return;
    InterlockedExchange(&g_w_job->cancel, 1);
    if (discard) g_w_job->discard = TRUE;
    if (g_wais_socket != INVALID_SOCKET) shutdown(g_wais_socket, SD_BOTH);
}

static void wais_job_buttons(BOOL busy)
{
    if (g_w_search_btn) EnableWindow(g_w_search_btn, !busy && g_w_connected);
    if (g_w_view_btn)   EnableWindow(g_w_view_btn,   !busy && g_w_connected);
}

/* The formatted results listing (as the standalone MGTWAIS lays it out). */
static char *wais_format_results(SearchResponseAPDU *response, WAISSearchResponse *info, int *out_count)
{
    char   line[1024];
    char  *out_buf;
    size_t out_cap = 8192, out_len;
    long   i;
    out_buf = (char *)malloc(out_cap);
    if (!out_buf) return NULL;
    _snprintf(line, sizeof(line),
              "Search Response:\r\n  Number of Records Returned: %d\r\n\r\n",
              response ? response->NumberOfRecordsReturned : 0);
    line[sizeof line - 1] = '\0';
    strcpy(out_buf, line);
    out_len = strlen(out_buf);
    *out_count = 0;
    if (info && info->DocHeaders) {
        i = 0;
        while (info->DocHeaders[i] != 0) {
            char *headline = info->DocHeaders[i]->Headline
                             ? trim_junk(info->DocHeaders[i]->Headline)
                             : NULL;
            long score = info->DocHeaders[i]->Score;
            int bars, k;
            char bar[12];
            size_t need;
            /* the standalone's FormatResults clamp: raw score in [0,9] */
            if (score < 0) score = 0;
            bars = (int)score;
            if (bars > 9) bars = 9;
            for (k = 0; k < bars; k++)  bar[k] = '\x01';
            for (; k < 9; k++)          bar[k] = ' ';
            bar[9] = '\0';
            _snprintf(line, sizeof(line), "%2ld: [%s]  %s\r\n", i + 1, bar,
                      headline && *headline ? headline : "(no title)");
            line[sizeof line - 1] = '\0';
            need = out_len + strlen(line) + 1;
            if (need > out_cap) {
                size_t new_cap = out_cap * 2;
                char *nb;
                while (new_cap < need) new_cap *= 2;
                nb = (char *)realloc(out_buf, new_cap);
                if (!nb) break;
                out_buf = nb; out_cap = new_cap;
            }
            strcpy(out_buf + out_len, line);
            out_len += strlen(line);
            i++;
        }
        *out_count = (int)i;
    }
    return out_buf;
}

static void wais_job_free(WAISJob *j)
{
    if (!j) return;
    if (j->response) {
        if (j->response->DatabaseDiagnosticRecords)
            freeWAISSearchResponse(j->response->DatabaseDiagnosticRecords);
        freeSearchResponseAPDU(j->response);
    }
    if (j->docID) freeAny(j->docID);
    free(j->body);
    free(j);
}

static void CALLBACK wais_job_timer_proc(HWND hwnd, UINT msg, UINT_PTR id, DWORD t)
{
    WAISJob *j = g_w_job;
    char     status[200];
    (void)msg; (void)t;
    if (!j) { KillTimer(hwnd, id); g_w_job_timer_id = 0; return; }

    if (!InterlockedCompareExchange(&j->done, 0, 0)) {
        LONG st  = InterlockedCompareExchange(&j->stage, 0, 0);
        LONG got = InterlockedCompareExchange(&j->got, 0, 0);
        if (InterlockedCompareExchange(&j->cancel, 0, 0)) return;
        if (st != j->shown_stage || got != j->shown_got) {
            j->shown_stage = st; j->shown_got = got;
            if (st == 1)      wais_status_if_active(j->content, "Connecting...");
            else if (st == 2) wais_status_if_active(j->content, "Initializing Z39.50-1988...");
            else if (st == 3 && j->kind == WAIS_JOB_SEARCH)
                wais_status_if_active(j->content, "Searching...");
            else if (st == 3) {
                if (j->doc_size > 0) {
                    int pct = (int)((long long)got * 100 / j->doc_size);
                    _snprintf(status, sizeof status, "Loading document: %ld of %ld bytes",
                              (long)got, (long)j->doc_size);
                    suite_set_progress(SUITE_ID_BTN_WAIS, pct);
                } else {
                    _snprintf(status, sizeof status, "Loading document: %ld bytes", (long)got);
                }
                status[sizeof status - 1] = '\0';
                wais_status_if_active(j->content, status);
            }
        }
        return;
    }

    /* done: apply exactly once */
    KillTimer(hwnd, id);
    g_w_job_timer_id = 0;
    g_w_job = NULL;
    suite_set_progress(SUITE_ID_BTN_WAIS, -1);

    if (j->discard) {                         /* Disconnect came: nothing to show */
        wais_job_free(j);
        wais_job_buttons(FALSE);
        return;
    }
    if (j->outcome == 2) {
        wais_status_if_active(j->content, "Cancelled.");
    } else if (j->outcome != 0) {
        wais_status_if_active(j->content, j->status_msg);
    } else if (j->kind == WAIS_JOB_SEARCH) {
        int n = 0;
        g_w_response = j->response;
        j->response  = NULL;                  /* owned by the module now */
        g_w_info = g_w_response
                   ? (WAISSearchResponse *)g_w_response->DatabaseDiagnosticRecords
                   : NULL;
        g_w_results_text = wais_format_results(g_w_response, g_w_info, &n);
        g_w_count = n;
        wais_show_view(1);
        /* Doc# always stays blank; the user types the document number. */
        SetWindowTextA(g_w_doc_edit, "");
        _snprintf(status, sizeof status, "Search: Done (%d document%s)", g_w_count,
                  g_w_count == 1 ? "" : "s");
        status[sizeof status - 1] = '\0';
        wais_status_if_active(j->content, status);
        if (wais_tab_active()) SetFocus(g_w_search_edit);
    } else {
        /* Sanitize body: CRLF normalization, ESC strip. */
        char *clean = (char *)malloc(j->body_len * 2 + 1);
        size_t ci = 0, k;
        if (clean) {
            for (k = 0; k < j->body_len; k++) {
                unsigned char c = (unsigned char)j->body[k];
                if (c == 27) continue;
                if (c == '\n' && (k == 0 || j->body[k-1] != '\r')) {
                    clean[ci++] = '\r';
                    clean[ci++] = '\n';
                } else {
                    clean[ci++] = (char)c;
                }
            }
            clean[ci] = '\0';
            if (g_w_document_text) free(g_w_document_text);
            g_w_document_text = clean;
            wais_show_view(2);
        }
        wais_status_if_active(j->content, "Document: Done");
        /* focus the output pane so the keys scroll the document */
        if (wais_tab_active()) SetFocus(g_w_output);
    }
    wais_job_free(j);
    wais_job_buttons(FALSE);
}

static void wais_start_job(HWND content, WAISJob *j)
{
    HANDLE thr;
    int    port_int = atoi(g_w_active_port);
    if (port_int <= 0) port_int = 210;
    j->content = content;
    j->port = port_int;
    strncpy(j->host, g_w_active_host, sizeof j->host - 1);
    strncpy(j->db,   g_w_active_db,   sizeof j->db - 1);
    g_w_job = j;
    wais_job_buttons(TRUE);
    wais_status(content, "Connecting...");
    thr = CreateThread(NULL, 0, wais_job_worker, j, 0, NULL);
    if (!thr) {
        g_w_job = NULL;
        wais_job_free(j);
        wais_job_buttons(FALSE);
        wais_status(content, "Cannot start the WAIS thread.");
        return;
    }
    CloseHandle(thr);
    g_w_job_timer_id = SetTimer(content, WAIS_JOB_TIMER_ID, 100, wais_job_timer_proc);
}

/* Search: read the query, start the job; the timer applies the result. */
static void wais_on_search(HWND content)
{
    char     query[1024];
    WAISJob *j;
    if (g_w_job || g_w_probe) {
        wais_status(content, "WAIS is busy: press Esc to cancel.");
        return;
    }
    GetWindowTextA(g_w_search_edit, query, sizeof(query));
    if (!query[0]) {
        wais_status(content, "Enter a query and press Search.");
        return;
    }
    wais_clear_results();
    j = (WAISJob *)calloc(1, sizeof *j);
    if (!j) { wais_status(content, "Out of memory."); return; }
    j->kind = WAIS_JOB_SEARCH;
    strncpy(j->query, query, sizeof j->query - 1);
    j->shown_stage = -1;
    wais_start_job(content, j);
}

/* ------------------------------------------------------------------ */
/* View: read Doc # entry, retrieve that document, render in output.  */
/* ------------------------------------------------------------------ */

static void wais_on_view(HWND content)
{
    char     doc_str[16];
    int      doc_num;
    char     status[96];
    WAISJob *j;
    const char *type;

    if (g_w_job || g_w_probe) {
        wais_status(content, "WAIS is busy: press Esc to cancel.");
        return;
    }
    if (!g_w_info || !g_w_info->DocHeaders || g_w_count <= 0) {
        wais_status(content, "Run a search first.");
        return;
    }
    GetWindowTextA(g_w_doc_edit, doc_str, sizeof(doc_str));
    doc_num = atoi(doc_str);
    if (doc_num < 1 || doc_num > g_w_count) {
        _snprintf(status, sizeof(status),
                  "Enter a Doc # from 1 to %d.", g_w_count);
        wais_status(content, status);
        return;
    }
    j = (WAISJob *)calloc(1, sizeof *j);
    if (!j) { wais_status(content, "Out of memory."); return; }
    j->kind = WAIS_JOB_VIEW;
    j->docID = duplicateAny(g_w_info->DocHeaders[doc_num - 1]->DocumentID);
    j->doc_size = g_w_info->DocHeaders[doc_num - 1]->DocumentLength;
    type = g_w_info->DocHeaders[doc_num - 1]->Types
           ? g_w_info->DocHeaders[doc_num - 1]->Types[0] : "TEXT";
    strncpy(j->type, type ? type : "TEXT", sizeof j->type - 1);
    j->shown_stage = -1;
    if (!j->docID) { free(j); wais_status(content, "Out of memory."); return; }
    wais_start_job(content, j);
}

/* ------------------------------------------------------------------ */
/* Layout reflow: anchor right-side controls, stretch search edit and  */
/* output pane to fill width. Mirrors LayoutControls in MGTWAIS GUI.   */
/* ------------------------------------------------------------------ */

void wais_module_resize(HWND content, int w, int h)
{
    const int margin = 16;
    const int gap    = 10;
    const int btn_w  = 105;
    const int btn_h  = 30;
    const int row_h  = 26;
    const int doc_w  = 55;
    const int dlbl_w = 55;
    int view_x, doc_edit_x, doc_label_x;

    if (!g_w_output) return;

    /* Row 1: server/port/db fields on the left; the Connect/Disconnect
     * toggle anchored right so it stays visible when the window is wider
     * than the fixed field block. Connect first, then search, then
     * view - read top-to-bottom matches the actual workflow. */
    MoveWindow(g_w_server_lbl,  margin,      18, 55, 22, TRUE);
    MoveWindow(g_w_server_edit, margin + 60, 14, 240, row_h, TRUE);
    MoveWindow(g_w_port_lbl,    margin + 310, 18, 40, 22, TRUE);
    MoveWindow(g_w_port_edit,   margin + 350, 14, 50, row_h, TRUE);
    MoveWindow(g_w_db_lbl,      margin + 410, 18, 70, 22, TRUE);
    MoveWindow(g_w_db_edit,     margin + 485, 14, 150, row_h, TRUE);
    MoveWindow(g_w_connect_btn, w - margin - W31_TOGGLE_W, 14 + (row_h - W31_TOGGLE_H) / 2,
               W31_TOGGLE_W, W31_TOGGLE_H, TRUE);

    /* Row 2: Search edit stretches; Search button anchors right. */
    MoveWindow(g_w_search_lbl,
        margin, 53, 60, row_h, TRUE);
    MoveWindow(g_w_search_edit,
        margin + 60, 49, w - margin - 60 - gap - btn_w - margin, row_h, TRUE);
    MoveWindow(g_w_search_btn,
        w - margin - btn_w, 47, btn_w, btn_h, TRUE);

    /* Row 3: Protocol label + [<] [>] + Doc# + View. View anchors right. */
    MoveWindow(g_w_proto_lbl, margin, 90, 200, 22, TRUE);
    MoveWindow(g_w_back_btn,  margin + 220, 85, 40, btn_h, TRUE);
    MoveWindow(g_w_fwd_btn,   margin + 265, 85, 40, btn_h, TRUE);
    view_x      = w - margin - btn_w;
    doc_edit_x  = view_x - gap - doc_w;
    doc_label_x = doc_edit_x - 5 - dlbl_w;
    MoveWindow(g_w_doc_lbl,  doc_label_x, 90, dlbl_w, 22, TRUE);
    MoveWindow(g_w_doc_edit, doc_edit_x,  86, doc_w,  row_h, TRUE);
    MoveWindow(g_w_view_btn, view_x,      84, btn_w,  btn_h, TRUE);

    /* Output: fills the rest. */
    MoveWindow(g_w_output, margin, 120, w - 2 * margin, h - 120 - margin, TRUE);

    /* Add a small internal padding so the first line isn't clipped by
     * the EDIT control's top edge / client-edge border, and so text
     * doesn't hug the left/right edges either. EM_SETRECTNP sets the
     * formatting rectangle without forcing a paint. Called on every
     * resize because the formatting rect resets when the control moves. */
    {
        RECT fmt;
        GetClientRect(g_w_output, &fmt);
        fmt.top    += 8;
        fmt.left   += 6;
        fmt.right  -= 6;
        SendMessageA(g_w_output, EM_SETRECTNP, 0, (LPARAM)&fmt);
    }
}

/* ------------------------------------------------------------------ */
/* Activate: build all controls inside the content panel.              */
/* ------------------------------------------------------------------ */

void wais_module_activate(HWND content)
{
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrA(content, GWLP_HINSTANCE);
    RECT rc;

    /* Re-activation path: controls already exist, just show them and
     * preserve everything (search query, results, doc text, scroll,
     * connected/disconnected state). */
    if (g_w_controls_created) {
        ShowWindow(g_w_search_lbl,     SW_SHOW);
        ShowWindow(g_w_search_edit,    SW_SHOW);
        ShowWindow(g_w_search_btn,     SW_SHOW);
        ShowWindow(g_w_server_lbl,     SW_SHOW);
        ShowWindow(g_w_server_edit,    SW_SHOW);
        ShowWindow(g_w_port_lbl,       SW_SHOW);
        ShowWindow(g_w_port_edit,      SW_SHOW);
        ShowWindow(g_w_db_lbl,         SW_SHOW);
        ShowWindow(g_w_db_edit,        SW_SHOW);
        ShowWindow(g_w_connect_btn,    SW_SHOW);
        ShowWindow(g_w_disconnect_btn, SW_SHOW);
        ShowWindow(g_w_proto_lbl,      SW_SHOW);
        ShowWindow(g_w_back_btn,       SW_SHOW);
        ShowWindow(g_w_fwd_btn,        SW_SHOW);
        ShowWindow(g_w_doc_lbl,        SW_SHOW);
        ShowWindow(g_w_doc_edit,       SW_SHOW);
        ShowWindow(g_w_view_btn,       SW_SHOW);
        ShowWindow(g_w_output,         SW_SHOW);
        SetFocus(g_w_connected ? g_w_search_edit : g_w_server_edit);
        return;
    }

    /* Initialize active bindings on first activate. Preserved across
     * re-activations during a single session (Connect updates them). */
    if (!g_w_active_host[0]) {
        strcpy(g_w_active_host, WAIS_DEFAULT_HOST);
        strcpy(g_w_active_port, WAIS_DEFAULT_PORT);
        strcpy(g_w_active_db,   WAIS_DEFAULT_DB);
    }

    /* Row 1 */
    g_w_search_lbl = CreateWindowA("STATIC", "Search:",
        WS_CHILD | WS_VISIBLE, 0, 0, 60, 22, content, NULL, hInst, NULL);
    g_w_search_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 200, 26, content, (HMENU)(INT_PTR)IDC_WAIS_SEARCH_EDIT, hInst, NULL);
    g_w_search_btn = CreateWindowA("BUTTON", "Search",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 105, 30, content, (HMENU)(INT_PTR)IDC_WAIS_SEARCH_BTN, hInst, NULL);

    /* Row 2 */
    g_w_server_lbl = CreateWindowA("STATIC", "Server:",
        WS_CHILD | WS_VISIBLE, 0, 0, 55, 22, content, NULL, hInst, NULL);
    g_w_server_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_w_active_host,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 240, 26, content, (HMENU)(INT_PTR)IDC_WAIS_SERVER_EDIT, hInst, NULL);
    g_w_port_lbl = CreateWindowA("STATIC", "Port:",
        WS_CHILD | WS_VISIBLE, 0, 0, 40, 22, content, NULL, hInst, NULL);
    g_w_port_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_w_active_port,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 50, 26, content, (HMENU)(INT_PTR)IDC_WAIS_PORT_EDIT, hInst, NULL);
    g_w_db_lbl = CreateWindowA("STATIC", "Database:",
        WS_CHILD | WS_VISIBLE, 0, 0, 70, 22, content, NULL, hInst, NULL);
    g_w_db_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_w_active_db,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        0, 0, 150, 26, content, (HMENU)(INT_PTR)IDC_WAIS_DB_EDIT, hInst, NULL);
    g_w_connect_btn = CreateWindowA("BUTTON", "Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, W31_TOGGLE_W, W31_TOGGLE_H, content, (HMENU)(INT_PTR)IDC_WAIS_CONNECT_BTN, hInst, NULL);
    g_w_disconnect_btn = NULL;   /* merged into the Connect toggle, 2026-11-11 */

    /* Row 3 */
    g_w_proto_lbl = CreateWindowA("STATIC", "Protocol: Z39.50-1988",
        WS_CHILD | WS_VISIBLE, 0, 0, 200, 22, content, NULL, hInst, NULL);
    g_w_back_btn = CreateWindowA("BUTTON", "<",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 40, 30, content, (HMENU)(INT_PTR)IDC_WAIS_BACK_BTN, hInst, NULL);
    g_w_fwd_btn = CreateWindowA("BUTTON", ">",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 40, 30, content, (HMENU)(INT_PTR)IDC_WAIS_FWD_BTN, hInst, NULL);
    g_w_doc_lbl = CreateWindowA("STATIC", "Doc #:",
        WS_CHILD | WS_VISIBLE, 0, 0, 55, 22, content, NULL, hInst, NULL);
    g_w_doc_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
        0, 0, 55, 26, content, (HMENU)(INT_PTR)IDC_WAIS_DOC_EDIT, hInst, NULL);
    g_w_view_btn = CreateWindowA("BUTTON", "View",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
        0, 0, 105, 30, content, (HMENU)(INT_PTR)IDC_WAIS_VIEW_BTN, hInst, NULL);

    /* Output pane: multi-line, read-only, horizontal + vertical scroll. */
    g_w_output = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
        0, 0, 200, 200, content, (HMENU)(INT_PTR)IDC_WAIS_OUTPUT, hInst, NULL);
    SendMessageA(g_w_output, EM_SETLIMITTEXT, (WPARAM)262144, 0);

    /* Apply UI font to controls, output font to the output pane. */
    if (g_hFontUI) {
        SendMessageA(g_w_search_lbl,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_search_edit, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_search_btn,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_server_lbl,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_server_edit, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_port_lbl,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_port_edit,   WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_db_lbl,      WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_db_edit,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_connect_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_disconnect_btn, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_proto_lbl,   WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_back_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_fwd_btn,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_doc_lbl,     WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_doc_edit,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageA(g_w_view_btn,    WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }
    if (g_hFontOutput)
        SendMessageA(g_w_output, WM_SETFONT, (WPARAM)g_hFontOutput, TRUE);

    /* Subclass search edit and doc edit for Enter handling, the output
     * edit for caret-hide + arrow-as-scroll, and the server/port/db
     * edits with FieldScrollSub so Up/Down/PgUp/PgDn from any of those
     * fields forwards to the output pane's vertical scroll. */
    g_orig_search_proc = (WNDPROC)SetWindowLongPtrA(
        g_w_search_edit, GWLP_WNDPROC, (LONG_PTR)SearchEditSub);
    g_orig_doc_proc    = (WNDPROC)SetWindowLongPtrA(
        g_w_doc_edit,    GWLP_WNDPROC, (LONG_PTR)DocEditSub);
    g_orig_output_proc = (WNDPROC)SetWindowLongPtrA(
        g_w_output,      GWLP_WNDPROC, (LONG_PTR)OutputSub);
    /* All three field edits share the same default WndProc (Edit class),
     * so capturing it once from the server edit covers all three. */
    g_orig_field_proc = (WNDPROC)SetWindowLongPtrA(
        g_w_server_edit, GWLP_WNDPROC, (LONG_PTR)FieldScrollSub);
    SetWindowLongPtrA(g_w_port_edit, GWLP_WNDPROC, (LONG_PTR)FieldScrollSub);
    SetWindowLongPtrA(g_w_db_edit,   GWLP_WNDPROC, (LONG_PTR)FieldScrollSub);

    GetClientRect(content, &rc);
    wais_module_resize(content, rc.right - rc.left, rc.bottom - rc.top);

    g_w_controls_created = TRUE;

    /* Initial UI state: pre-connect. Only Server/Port/Database/Connect
     * reachable; Search/Doc/View/Back/Forward/Disconnect grayed until
     * Connect succeeds. Mirrors the ARPANET FTP-Mail session gate. */
    wais_set_connected_state(FALSE);
    SetFocus(g_w_server_edit);
}

/* ------------------------------------------------------------------ */
/* Deactivate: tear down. Destructive switch per BLUEPRINT 3.2.       */
/* ------------------------------------------------------------------ */

void wais_module_deactivate(HWND content)
{
    (void)content;

    /* Non-destructive deactivate: hide controls and preserve all state
     * (search query, results, doc text, server/port/db values, scroll
     * position). User can switch to another module and back without
     * losing in-progress work. See [[feedback-module-state-preserved]]. */
    if (!g_w_controls_created) return;

    ShowWindow(g_w_search_lbl,     SW_HIDE);
    ShowWindow(g_w_search_edit,    SW_HIDE);
    ShowWindow(g_w_search_btn,     SW_HIDE);
    ShowWindow(g_w_server_lbl,     SW_HIDE);
    ShowWindow(g_w_server_edit,    SW_HIDE);
    ShowWindow(g_w_port_lbl,       SW_HIDE);
    ShowWindow(g_w_port_edit,      SW_HIDE);
    ShowWindow(g_w_db_lbl,         SW_HIDE);
    ShowWindow(g_w_db_edit,        SW_HIDE);
    ShowWindow(g_w_connect_btn,    SW_HIDE);
    ShowWindow(g_w_disconnect_btn, SW_HIDE);
    ShowWindow(g_w_proto_lbl,      SW_HIDE);
    ShowWindow(g_w_back_btn,       SW_HIDE);
    ShowWindow(g_w_fwd_btn,        SW_HIDE);
    ShowWindow(g_w_doc_lbl,        SW_HIDE);
    ShowWindow(g_w_doc_edit,       SW_HIDE);
    ShowWindow(g_w_view_btn,       SW_HIDE);
    ShowWindow(g_w_output,         SW_HIDE);
}

/* ------------------------------------------------------------------ */
/* on_command dispatch.                                                */
/* ------------------------------------------------------------------ */

BOOL wais_module_on_command(HWND content, WPARAM wParam, LPARAM lParam)
{
    int id    = LOWORD(wParam);
    int notif = HIWORD(wParam);
    (void)lParam;

    switch (id) {
    case IDC_WAIS_SEARCH_BTN:
        if (notif == BN_CLICKED) { wais_on_search(content); return TRUE; }
        break;
    case IDC_WAIS_CONNECT_BTN:      /* the toggle: its label says which */
        if (notif == BN_CLICKED) {
            if (g_w_connected) wais_on_disconnect(content);
            else               wais_on_connect(content);
            return TRUE;
        }
        break;
    case IDC_WAIS_DISCONNECT_BTN:
        if (notif == BN_CLICKED) { wais_on_disconnect(content); return TRUE; }
        break;
    case IDC_WAIS_VIEW_BTN:
        if (notif == BN_CLICKED) { wais_on_view(content); return TRUE; }
        break;
    case IDC_WAIS_BACK_BTN:
        if (notif == BN_CLICKED) {
            if (g_w_current_view == 2 && g_w_results_text) wais_show_view(1);
            return TRUE;
        }
        break;
    case IDC_WAIS_FWD_BTN:
        if (notif == BN_CLICKED) {
            if (g_w_current_view == 1 && g_w_document_text) wais_show_view(2);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL wais_module_has_unsaved(void)
{
    return FALSE;
}
