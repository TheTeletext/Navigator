/*
 * ftp_ctrl.h - the shared FTP control-connection core (RFC 959 / RFC 765).
 *
 * One control connection: a bounded, cancellable connect; RFC 959 replies
 * including multi-line ("ddd-" ... "ddd ") ones; commands; the anonymous
 * login; passive (PASV, EPSV) and active (PORT, EPRT) data connections.
 * Extracted 2026-11-12 from ftp_fetch.c, the client Archie's Download had
 * used against real servers since 2026-07-24 (docs/2026-11-12_FTP_TAB.md).
 * Callers: the FTP tab (ftp_module.c), Archie's Download (ftp_fetch.c, now
 * a wrapper) and ARPANET FTP-Mail (arpamail_module.c).
 *
 * NEVER WRITE. Every session has a verb allowlist and ftpc_send_cmd refuses
 * any other verb before a byte is sent. STOR, STOU, APPE, DELE, MKD, RMD,
 * RNFR, RNTO and SITE are on NO list, so no caller can send them. An
 * argument holding CR or LF is refused too, so a file name cannot smuggle
 * a second command onto the wire.
 *
 * Threads: one thread drives a session (any thread: ARPANET calls from the
 * UI thread, the others from workers). ftpc_abort and ftpc_cancel_op may
 * be called from any other thread; every wait runs in 200 ms slices that
 * check them, so an abort takes effect within one slice. Lifetime: a
 * session starts with one reference; ftpc_retain gives a second holder
 * its own, and the LAST ftpc_release frees it (the telnet_proto pattern
 * of 2026-11-12), so an abort from the UI can never race a free.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#ifndef FTP_CTRL_H
#define FTP_CTRL_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

/* The one anonymous password every caller sends (operator decision
 * 2026-11-12: the value ftp_fetch.c has used since July; it names only
 * the client's public brand domain, never a person or this machine). */
#define FTPC_ANON_PASSWORD "navigator@teletext.ca"

/* Verb sets (allowlists). */
#define FTPC_VERBS_DOWNLOAD 1   /* USER PASS SYST FEAT PWD CWD CDUP TYPE PASV EPSV PORT EPRT
                                   LIST NLST SIZE MDTM RETR NOOP ABOR QUIT */
#define FTPC_VERBS_MAIL     2   /* MAIL QUIT (RFC 765 inline path) */

/* Results below zero. */
#define FTPC_ERR_IO       (-1)  /* the connection failed or closed */
#define FTPC_ERR_REFUSED  (-2)  /* not on this session's allowlist: nothing sent */
#define FTPC_ERR_STOPPED  (-3)  /* aborted or cancelled */
#define FTPC_ERR_TIMEOUT  (-4)
#define FTPC_ERR_RESOLVE  (-5)  /* host not found */
#define FTPC_ERR_CONNECT  (-6)  /* refused or unreachable */
#define FTPC_ERR_PROTO    (-7)  /* a reply that is not an FTP reply */

/* Log line kinds. */
#define FTPC_LOG_SERVER 0       /* a server line, CRLF stripped */
#define FTPC_LOG_CLIENT 1       /* a command line as sent, CRLF stripped */
#define FTPC_LOG_NOTE   2       /* a local note (connecting, data address) */
typedef void (*ftpc_log_cb)(void *user, int kind, const char *line);

typedef struct ftp_ctrl ftp_ctrl_t;

typedef struct ftpc_data {
    SOCKET s;                   /* the data connection */
    SOCKET ls;                  /* active mode: the listener until accepted */
    int    passive;
} ftpc_data_t;

ftp_ctrl_t *ftpc_new(int verbset, ftpc_log_cb cb, void *user);
void ftpc_retain(ftp_ctrl_t *c);
void ftpc_release(ftp_ctrl_t *c);       /* the last one closes and frees */

/* An optional flag the session also treats as an abort (Archie's cancel). */
void ftpc_set_cancel_flag(ftp_ctrl_t *c, volatile LONG *flag);
void ftpc_set_reply_timeout(ftp_ctrl_t *c, int ms);   /* default 15000 */
void ftpc_set_data_timeout(ftp_ctrl_t *c, int ms);    /* default 30000 idle */

int  ftpc_connect(ftp_ctrl_t *c, const char *host, int port, int timeout_ms);
void ftpc_disconnect(ftp_ctrl_t *c);    /* closes, no QUIT */
int  ftpc_connected(ftp_ctrl_t *c);
/* The address family of the control connection (AF_INET / AF_INET6). */
int  ftpc_family(ftp_ctrl_t *c);

/* One reply, multi-line aware; every line goes to the log. Returns the
 * code or an FTPC_ERR. `last` gets the final line (may be NULL). */
int  ftpc_reply(ftp_ctrl_t *c, char *last, int last_sz);
/* Replies until one that is not 1xx (each 1xx is logged on the way). */
int  ftpc_reply_final(ftp_ctrl_t *c, char *last, int last_sz);
/* "VERB" or "VERB arg" plus CRLF; refused unless on the allowlist. */
int  ftpc_send_cmd(ftp_ctrl_t *c, const char *verb, const char *arg);
int  ftpc_cmd(ftp_ctrl_t *c, const char *verb, const char *arg, char *last, int last_sz);
/* RFC 765 mail text: MAIL sessions only, and only right after a 354. */
int  ftpc_send_mail_text(ftp_ctrl_t *c, const char *data, int len);

/* USER anonymous; PASS FTPC_ANON_PASSWORD only if the server answers 331.
 * Returns the final code (2xx = logged in) or an FTPC_ERR. */
int  ftpc_login_anonymous(ftp_ctrl_t *c, char *last, int last_sz);

/* Data connections. open: passive = PASV (EPSV on IPv6 or when PASV is
 * refused) and connect; active = listen and send PORT / EPRT. Returns 0,
 * a refusing reply code (> 0), or an FTPC_ERR. start: sends LIST / NLST /
 * RETR, wants a 1xx, and in active mode accepts the server's connection.
 * Returns the 1xx code, the refusing code, or an FTPC_ERR. */
int  ftpc_data_open(ftp_ctrl_t *c, int passive, ftpc_data_t *d, char *last, int last_sz);
int  ftpc_data_start(ftp_ctrl_t *c, const char *verb, const char *arg, ftpc_data_t *d,
                     char *last, int last_sz);
int  ftpc_data_recv(ftp_ctrl_t *c, ftpc_data_t *d, char *buf, int n);  /* >0, 0 = end, FTPC_ERR */
void ftpc_data_close(ftp_ctrl_t *c, ftpc_data_t *d);

/* Cancelling one operation (a transfer) without ending the session: data
 * waits stop with FTPC_ERR_STOPPED; then ftpc_abort_transfer sends ABOR and
 * reads its replies, and clears the request. */
void ftpc_cancel_op(ftp_ctrl_t *c);
int  ftpc_op_cancelled(ftp_ctrl_t *c);
int  ftpc_abort_transfer(ftp_ctrl_t *c, char *last, int last_sz);

/* Between commands: has the server spoken or hung up? Never sends a byte
 * (no NOOP keepalive: a server's idle timeout is its own to keep). Returns
 * 0 if the control connection is quiet, the reply's code if the server
 * sent one unprompted (vsftpd: "421 Timeout." after idle_session_timeout),
 * or FTPC_ERR_IO if it closed the connection. The reply is logged. */
int  ftpc_idle_check(ftp_ctrl_t *c, char *last, int last_sz);

/* Ending the session from any thread. */
void ftpc_abort(ftp_ctrl_t *c);
int  ftpc_aborted(ftp_ctrl_t *c);

const char *ftpc_strerror(int err);

#endif /* FTP_CTRL_H */
