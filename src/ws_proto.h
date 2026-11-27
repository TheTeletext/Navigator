/*
 * ws_proto.h - a plain WebSocket (ws://) client on the WinHTTP WebSocket
 *              API, with the same event contract as telnet_proto.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Navigator 2.0, for the Minitel format of the Videotex tab
 * (docs/2026-11-24_NAVIGATOR_SITE_LISTS_WS_BUILD.md).
 *
 * PLAIN ws ONLY. There are no secure connections in Navigator 2.0
 * (operator, 2026-11-24): the request is never opened with
 * WINHTTP_FLAG_SECURE, and wss:// is refused before it gets here.
 *
 * SAME CONTRACT AS telnet_proto, so the Videotex tab can use either
 * through one path: a worker thread posts TELNET_EVT_CONNECTED, then
 * TELNET_EVT_DATA with a TelnetDataChunk (allocated exactly as
 * telnet_proto allocates it, so telnet_proto_free_chunk and
 * ws_proto_free_chunk are interchangeable), then TELNET_EVT_CLOSED; a
 * failure before or during the handshake posts TELNET_EVT_ERROR
 * (LPARAM = a static message, never freed) followed by TELNET_EVT_CLOSED.
 *
 * TEXT, AS THE GATEWAY'S OWN WEB CLIENT DOES IT. Videotex gateways send
 * text frames whose characters are the terminal's bytes. Received text is
 * decoded from UTF-8 and every code point below 256 is delivered as one
 * byte; anything above is dropped and counted. Binary frames are
 * delivered as they are. Sending, each byte becomes one character of a
 * text frame (bytes from 0x80 up are UTF-8 encoded, as their code point).
 *
 * WinHTTP does the handshake (RFC 6455 section 4), the masking of client
 * frames (5.3), the control frames and the close handshake.
 */

#ifndef WS_PROTO_H
#define WS_PROTO_H

#include <stddef.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "telnet_proto.h"   /* TELNET_EVT_* and TelnetDataChunk: the shared contract */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WsProto WsProto;

/* Open ws://host:port<path>. path "" means "/". Returns 0 and writes *out
 * once the worker is started; events follow on notify_msg. Non-zero (and
 * *out NULL) only if the worker could not be started. */
int  ws_proto_open(const char *host, int port, const char *path,
                   HWND notify_hwnd, UINT notify_msg, WsProto **out);

/* Send bytes as ONE text frame. Returns 0 on success. Safe from the UI
 * thread while the worker receives (WinHTTP allows one send and one
 * receive to be outstanding together). */
int  ws_proto_send(WsProto *w, const void *buf, size_t len);

/* Tear down: send a close frame if connected, abort the worker's pending
 * call, wait for it briefly and drop the owner's reference. Events the
 * worker posted before it stopped may still be queued; the caller drains
 * them as it does for telnet_proto. The handle is invalid afterwards. */
void ws_proto_close(WsProto *w);

/* Free a chunk delivered with TELNET_EVT_DATA. */
void ws_proto_free_chunk(TelnetDataChunk *chunk);

/* Counters for a status line or a test: frames received, code points
 * dropped because they do not fit a byte. */
unsigned long ws_proto_frames_in(WsProto *w);
unsigned long ws_proto_dropped(WsProto *w);

#ifdef __cplusplus
}
#endif

#endif /* WS_PROTO_H */
