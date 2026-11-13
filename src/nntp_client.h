/*
 * nntp_client.h - a READ-ONLY NNTP reader client (RFC 3977), on the
 * shared transport line_conn.c.
 *
 * NNTP's own grammar lives here: a single-line status reply, and for the
 * codes that carry one, a data block of lines ended by a lone "." with
 * dot-stuffing (RFC 3977 3.1.1). No ABOR exists, so a block is either read
 * to its dot or the connection is dropped.
 *
 * NEVER WRITE. The verb allowlist is MODE CAPABILITIES LIST GROUP
 * LISTGROUP OVER XOVER HDR XHDR ARTICLE HEAD BODY STAT DATE QUIT. POST,
 * IHAVE, TAKETHIS, CHECK, AUTHINFO, STARTTLS and NEWNEWS are on no list:
 * nntp_cmd refuses them before a byte is sent (NNTP_ERR_REFUSED), as it
 * refuses an argument holding CR or LF and a command over 510 characters.
 *
 * Threads and lifetime as line_conn: one driving thread; nntp_abort from
 * any thread; nntp_retain / nntp_release, the last release frees.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#ifndef NNTP_CLIENT_H
#define NNTP_CLIENT_H

#include "line_conn.h"

#define NNTP_ERR_IO       LC_ERR_IO
#define NNTP_ERR_REFUSED  LC_ERR_REFUSED
#define NNTP_ERR_STOPPED  LC_ERR_STOPPED
#define NNTP_ERR_TIMEOUT  LC_ERR_TIMEOUT
#define NNTP_ERR_RESOLVE  LC_ERR_RESOLVE
#define NNTP_ERR_CONNECT  LC_ERR_CONNECT
#define NNTP_ERR_PROTO    (-7)    /* a line that is not an NNTP status */

#define NNTP_LOG_SERVER LC_LOG_SERVER
#define NNTP_LOG_CLIENT LC_LOG_CLIENT
#define NNTP_LOG_NOTE   LC_LOG_NOTE

#define NNTP_STATUS_MAX 512

typedef struct nntp nntp_t;

nntp_t *nntp_new(lc_log_cb cb, void *user);
void nntp_retain(nntp_t *n);
void nntp_release(nntp_t *n);
void nntp_abort(nntp_t *n);
int  nntp_aborted(nntp_t *n);
void nntp_set_reply_timeout(nntp_t *n, int ms);   /* default 30000 */

/* Connect and read the greeting. Returns its code (200 posting allowed,
 * 201 not; 400 / 502 = service refused) or an NNTP_ERR. The greeting
 * line is copied to status. */
int  nntp_connect(nntp_t *n, const char *host, int port, int timeout_ms,
                  char *status, int status_sz);
void nntp_disconnect(nntp_t *n);
int  nntp_connected(nntp_t *n);

/* Send "VERB arg" and read the status line. Returns the code or an
 * NNTP_ERR; status gets the line. If the code opens a data block
 * (nntp_code_has_block) the caller MUST read it with nntp_block_line
 * to its end (or drain it, or disconnect) before the next command. */
int  nntp_cmd(nntp_t *n, const char *verb, const char *arg, char *status, int status_sz);
int  nntp_code_has_block(const char *verb, int code);

/* The next line of a data block: the length (>= 0) with *line pointing at
 * it (dot-unstuffed, the CR before LF removed, NUL-terminated, valid until
 * the next read), 0 with *line NULL at the terminating ".", or an
 * NNTP_ERR. *truncated as lc_read_line. */
int  nntp_block_line(nntp_t *n, char **line, int *truncated);
/* Read the rest of a block and throw it away. 0 or an NNTP_ERR. */
int  nntp_drain(nntp_t *n);

/* Between commands, never sending a byte: 0 quiet; a status code if the
 * server spoke unprompted (RFC 3977 3.2.1: 400 before closing), with
 * status filled; NNTP_ERR_IO if it closed the connection. INN closes
 * silently at its clienttimeout (news.teletext.ca: 1800 s). */
int  nntp_idle_check(nntp_t *n, char *status, int status_sz);

/* Pipelining (RFC 3977 3.5): nntp_send sends one command without reading
 * (allowlist as nntp_cmd); nntp_status reads the next status line. Used
 * for STAT batches only, never for commands that return a block. */
int  nntp_send(nntp_t *n, const char *verb, const char *arg);
int  nntp_status(nntp_t *n, char *status, int status_sz);

/* Bytes taken off the wire for the current block (for status text). */
long long nntp_block_bytes(nntp_t *n);

const char *nntp_strerror(int err);

#endif /* NNTP_CLIENT_H */
