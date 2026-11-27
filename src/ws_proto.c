/*
 * ws_proto.c - a plain WebSocket (ws://) client on the WinHTTP WebSocket
 *              API, with telnet_proto's event contract.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See ws_proto.h. Sources for the API behaviour relied on here, all
 * Microsoft Learn (WinHTTP reference), checked 2026-11-24:
 *   - WinHttpWebSocketCompleteUpgrade: the request must be marked with
 *     WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET before it is sent, and the
 *     caller checks the status is 101 before completing the upgrade.
 *   - WinHttpWebSocketReceive fails with ERROR_INVALID_OPERATION only when
 *     "a close or receive is pending", WinHttpWebSocketSend only when "a
 *     close or send is pending": one send may run while one receive waits.
 *   - WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT is in milliseconds, default
 *     10 seconds; WinHttpWebSocketShutdown sends a close frame and leaves
 *     the receive channel open.
 * Protocol: RFC 6455 (text frames are UTF-8, section 5.6; close codes,
 * section 7.4.1: 1000 is a normal closure).
 *
 * LIFETIME follows telnet_proto (2026-11-12): two references, the
 * owner's and the worker's; the last release frees. The handles the
 * worker is waiting on are published under a lock, and ws_proto_close
 * takes them and closes them, which ends the worker's pending call; the
 * worker closes only what it still holds, so nothing is closed twice.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include "ws_proto.h"
#include "suite_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_RECV_BUF        8192
#define WS_TIMEOUT_MS      15000    /* resolve, connect, send, handshake */
#define WS_CLOSE_WAIT_MS   2000     /* our close: how long to wait for the
                                       server's close frame */
#define WS_JOIN_MS         4000     /* ws_proto_close: how long to wait for
                                       the worker before leaving it to free
                                       itself */

struct WsProto {
    volatile LONG    refs;
    volatile LONG    stop;          /* ws_proto_close was called */
    volatile LONG    connected;     /* upgraded and not yet closed */
    HANDLE           thread;
    CRITICAL_SECTION lock;          /* guards req, ws and sending */
    HINTERNET        ses, con;      /* the worker's to close */
    HINTERNET        req;           /* published while the handshake runs */
    HINTERNET        ws;            /* published while connected */
    HWND             notify_hwnd;
    UINT             notify_msg;
    char             host[256];
    int              port;
    char             path[1024];
    /* UTF-8 decoder state across receives */
    unsigned long    cp;
    int              need;
    volatile LONG    frames_in;
    volatile LONG    dropped;
};

static void ws_release(WsProto *w)
{
    if (InterlockedDecrement(&w->refs) == 0) {
        DeleteCriticalSection(&w->lock);
        if (w->thread) CloseHandle(w->thread);
        free(w);
    }
}

static void post(WsProto *w, WPARAM evt, LPARAM lp)
{
    PostMessageA(w->notify_hwnd, w->notify_msg, evt, lp);
}

/* An error before CONNECTED: the message, then CLOSED, unless the owner
 * has already closed (then nobody wants either). */
static void post_fail(WsProto *w, const char *msg)
{
    if (InterlockedCompareExchange(&w->stop, 0, 0)) return;
    post(w, TELNET_EVT_ERROR, (LPARAM)msg);
    post(w, TELNET_EVT_CLOSED, 1);
}

static const char *winhttp_reason(DWORD e)
{
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:  return "Host not found";
    case ERROR_WINHTTP_CANNOT_CONNECT:     return "The host refused the connection or cannot be reached";
    case ERROR_WINHTTP_TIMEOUT:            return "The host did not answer in time";
    case ERROR_WINHTTP_CONNECTION_ERROR:   return "The connection was reset";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "The host's answer was not HTTP";
    default:                               return "The connection failed";
    }
}

/* UTF-8 text to bytes: each code point below 256 is one byte. Invalid
 * sequences pass their bytes through as they are. Returns the length. */
static int utf8_to_bytes(WsProto *w, const unsigned char *in, DWORD n,
                         unsigned char *out)
{
    DWORD i;
    int o = 0;
    for (i = 0; i < n; i++) {
        unsigned char b = in[i];
        if (w->need > 0) {
            if ((b & 0xC0) == 0x80) {
                w->cp = (w->cp << 6) | (b & 0x3F);
                if (--w->need == 0) {
                    if (w->cp < 256) out[o++] = (unsigned char)w->cp;
                    else InterlockedIncrement(&w->dropped);
                }
                continue;
            }
            w->need = 0;            /* broken sequence: fall through */
        }
        if (b < 0x80)                { out[o++] = b; }
        else if ((b & 0xE0) == 0xC0) { w->cp = b & 0x1F; w->need = 1; }
        else if ((b & 0xF0) == 0xE0) { w->cp = b & 0x0F; w->need = 2; }
        else if ((b & 0xF8) == 0xF0) { w->cp = b & 0x07; w->need = 3; }
        else                         { out[o++] = b; }
    }
    return o;
}

static int to_wide(const char *s, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

/* Take a published handle back under the lock, so only one side closes it. */
static HINTERNET take(WsProto *w, HINTERNET *slot)
{
    HINTERNET h;
    EnterCriticalSection(&w->lock);
    h = *slot;
    *slot = NULL;
    LeaveCriticalSection(&w->lock);
    return h;
}

static DWORD WINAPI ws_worker(LPVOID arg)
{
    WsProto *w = (WsProto *)arg;
    wchar_t  whost[256], wpath[1024], ua[96];
    DWORD    status = 0, sz = sizeof status, e;
    HINTERNET req = NULL, ws = NULL;
    int      server_closed = 0, failed = 0;
    unsigned char *in = NULL, *out = NULL;

    if (!to_wide(w->host, whost, 256) || !to_wide(w->path[0] ? w->path : "/", wpath, 1024)) {
        post_fail(w, "The address is not valid");
        goto done;
    }
    {
        char a[96];
        _snprintf(a, sizeof a - 1, "%s/%s", SUITE_UA_PRODUCT, SUITE_VERSION_STR);
        a[sizeof a - 1] = '\0';
        if (!to_wide(a, ua, 96)) wcscpy(ua, L"Teletext-Navigator");
    }

    /* A direct connection, as the Telnet transport makes: no proxy. */
    w->ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NO_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!w->ses) { post_fail(w, "The connection failed"); goto done; }
    WinHttpSetTimeouts(w->ses, WS_TIMEOUT_MS, WS_TIMEOUT_MS, WS_TIMEOUT_MS, WS_TIMEOUT_MS);

    w->con = WinHttpConnect(w->ses, whost, (INTERNET_PORT)w->port, 0);
    if (!w->con) { post_fail(w, winhttp_reason(GetLastError())); goto done; }

    /* Never WINHTTP_FLAG_SECURE: plain ws only in this version. */
    req = WinHttpOpenRequest(w->con, L"GET", wpath, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!req) { post_fail(w, "The connection failed"); goto done; }
    if (!WinHttpSetOption(req, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, NULL, 0)) {
        WinHttpCloseHandle(req);
        post_fail(w, "WebSocket is not available on this system");
        goto done;
    }
    EnterCriticalSection(&w->lock);
    if (w->stop) { LeaveCriticalSection(&w->lock); WinHttpCloseHandle(req); goto done; }
    w->req = req;                   /* ws_proto_close may now abort it */
    LeaveCriticalSection(&w->lock);

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(req, NULL)) {
        e = GetLastError();
        req = take(w, &w->req);
        if (req) WinHttpCloseHandle(req);
        post_fail(w, winhttp_reason(e));
        goto done;
    }
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz,
                             WINHTTP_NO_HEADER_INDEX))
        status = 0;
    if (status != 101) {
        req = take(w, &w->req);
        if (req) WinHttpCloseHandle(req);
        post_fail(w, "The host did not accept a WebSocket connection");
        goto done;
    }
    req = take(w, &w->req);
    if (!req) goto done;            /* closed by the owner meanwhile */
    ws = WinHttpWebSocketCompleteUpgrade(req, 0);
    WinHttpCloseHandle(req);        /* the WebSocket handle stands alone */
    req = NULL;
    if (!ws) { post_fail(w, "The WebSocket handshake failed"); goto done; }
    {
        DWORD ms = WS_CLOSE_WAIT_MS;
        WinHttpSetOption(ws, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT, &ms, sizeof ms);
    }

    EnterCriticalSection(&w->lock);
    if (w->stop) { LeaveCriticalSection(&w->lock); WinHttpCloseHandle(ws); goto done; }
    w->ws = ws;
    InterlockedExchange(&w->connected, 1);
    LeaveCriticalSection(&w->lock);
    post(w, TELNET_EVT_CONNECTED, 0);

    in  = (unsigned char *)malloc(WS_RECV_BUF);
    out = (unsigned char *)malloc(WS_RECV_BUF);
    if (!in || !out) failed = 1;

    while (!failed && !InterlockedCompareExchange(&w->stop, 0, 0)) {
        DWORD got = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        int n;
        TelnetDataChunk *c;

        e = WinHttpWebSocketReceive(ws, in, WS_RECV_BUF, &got, &type);
        if (e == ERROR_WINHTTP_TIMEOUT) continue;   /* idle: keep waiting */
        if (e != NO_ERROR) { failed = 1; break; }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { server_closed = 1; break; }
        InterlockedIncrement(&w->frames_in);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE
            || type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
            n = utf8_to_bytes(w, in, got, out);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) w->need = 0;
        } else {
            memcpy(out, in, got);
            n = (int)got;
        }
        if (n <= 0) continue;       /* an empty text frame: nothing to draw */
        c = (TelnetDataChunk *)malloc(sizeof *c);
        if (!c) continue;
        c->data = (unsigned char *)malloc((size_t)n);
        if (!c->data) { free(c); continue; }
        memcpy(c->data, out, (size_t)n);
        c->len = n;
        post(w, TELNET_EVT_DATA, (LPARAM)c);
    }

    InterlockedExchange(&w->connected, 0);
    ws = take(w, &w->ws);
    if (ws) {
        /* The server closed (answer its close frame) or the line failed. */
        if (server_closed) WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(ws);
    }
    if (!InterlockedCompareExchange(&w->stop, 0, 0))
        post(w, TELNET_EVT_CLOSED, (server_closed || !failed) ? 0 : 1);

done:
    free(in);
    free(out);
    if (w->con) { WinHttpCloseHandle(w->con); w->con = NULL; }
    if (w->ses) { WinHttpCloseHandle(w->ses); w->ses = NULL; }
    ws_release(w);
    return 0;
}

int ws_proto_open(const char *host, int port, const char *path,
                  HWND notify_hwnd, UINT notify_msg, WsProto **out)
{
    WsProto *w;
    if (!out) return 1;
    *out = NULL;
    if (!host || !host[0] || port <= 0 || port > 65535) return 1;

    w = (WsProto *)calloc(1, sizeof *w);
    if (!w) return 2;
    InitializeCriticalSection(&w->lock);
    w->refs = 2;                    /* the owner's and the worker's */
    w->notify_hwnd = notify_hwnd;
    w->notify_msg  = notify_msg;
    w->port = port;
    strncpy(w->host, host, sizeof w->host - 1);
    strncpy(w->path, path ? path : "", sizeof w->path - 1);

    w->thread = CreateThread(NULL, 0, ws_worker, w, 0, NULL);
    if (!w->thread) {
        DeleteCriticalSection(&w->lock);
        free(w);
        return 3;
    }
    *out = w;
    return 0;
}

int ws_proto_send(WsProto *w, const void *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    unsigned char *u;
    size_t i, j;
    int rc = 0;

    if (!w || !buf || len == 0) return 1;
    u = (unsigned char *)malloc(len * 2);
    if (!u) return 2;
    for (i = 0, j = 0; i < len; i++) {
        if (p[i] < 0x80) u[j++] = p[i];
        else { u[j++] = (unsigned char)(0xC0 | (p[i] >> 6)); u[j++] = (unsigned char)(0x80 | (p[i] & 0x3F)); }
    }
    EnterCriticalSection(&w->lock);
    if (!w->ws || !w->connected) rc = 3;
    else if (WinHttpWebSocketSend(w->ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                  u, (DWORD)j) != NO_ERROR) rc = 4;
    LeaveCriticalSection(&w->lock);
    free(u);
    return rc;
}

void ws_proto_close(WsProto *w)
{
    HINTERNET req, ws;
    if (!w) return;
    InterlockedExchange(&w->stop, 1);
    req = take(w, &w->req);
    ws  = take(w, &w->ws);
    if (req) WinHttpCloseHandle(req);          /* ends a pending handshake */
    if (ws) {
        /* A close frame (RFC 6455 7.1.2), then the handle: closing it ends
         * the worker's pending receive without waiting for the server's
         * own close frame, which these gateways do not send promptly. */
        WinHttpWebSocketShutdown(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, NULL, 0);
        WinHttpCloseHandle(ws);
    }
    if (w->thread) WaitForSingleObject(w->thread, WS_JOIN_MS);
    ws_release(w);
}

void ws_proto_free_chunk(TelnetDataChunk *chunk)
{
    if (!chunk) return;
    free(chunk->data);
    free(chunk);
}

unsigned long ws_proto_frames_in(WsProto *w) { return w ? (unsigned long)w->frames_in : 0; }
unsigned long ws_proto_dropped(WsProto *w)   { return w ? (unsigned long)w->dropped : 0; }
