/*
 * ftp_fetch.c - one-shot FTP download (passive, anonymous, binary). See
 *               ftp_fetch.h.
 *
 * Since 2026-11-12 a thin wrapper over the shared core src/ftp_ctrl.c,
 * which was extracted from this file's own July 2026 client (so there is
 * one FTP implementation, not two; docs/2026-11-12_FTP_TAB.md). Wire flow
 * unchanged: connect; 220; USER anonymous (PASS navigator@teletext.ca on
 * 331); TYPE I; PASV, data to the control peer's own address; RETR; stream
 * to the local file; 226; QUIT. New: ftp_fetch_file_ex takes a cancel flag.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>

#include "ftp_fetch.h"
#include "ftp_ctrl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FTP_CONNECT_MS   12000
#define FTP_RECV_CHUNK   32768

static void ftp_err(char *buf, int sz, const char *msg)
{
    if (buf && sz > 0) { strncpy(buf, msg, (size_t)sz - 1); buf[sz - 1] = '\0'; }
}

int ftp_fetch_file_ex(const char *host, int port,
                      const char *remote_path, const char *local_path,
                      ftp_progress_cb progress, void *user,
                      volatile LONG *cancel,
                      long *out_total, char *errbuf, int errbuf_sz)
{
    ftp_ctrl_t  *c;
    ftpc_data_t  d;
    char         line[1024];
    int          code, ok = 0;
    FILE        *fp = NULL;
    long         total = 0;
    char        *buf = NULL;

    d.s = d.ls = INVALID_SOCKET;
    if (out_total) *out_total = 0;
    if (!host || !*host || !remote_path || !*remote_path || !local_path || !*local_path) {
        ftp_err(errbuf, errbuf_sz, "Bad download parameters.");
        return 0;
    }
    if (port <= 0 || port > 65535) port = FTP_FETCH_DEFAULT_PORT;

    c = ftpc_new(FTPC_VERBS_DOWNLOAD, NULL, NULL);
    if (!c) { ftp_err(errbuf, errbuf_sz, "Out of memory."); return 0; }
    if (cancel) ftpc_set_cancel_flag(c, cancel);

    if (ftpc_connect(c, host, port, FTP_CONNECT_MS) != 0) {
        if (ftpc_aborted(c)) ftp_err(errbuf, errbuf_sz, "Download cancelled.");
        else _snprintf(errbuf, errbuf_sz, "Cannot connect to %s.", host);
        goto done;
    }
    if (ftpc_reply(c, line, sizeof line) / 100 != 2) {   /* 220 greeting */
        ftp_err(errbuf, errbuf_sz, "No FTP greeting from server.");
        goto done;
    }
    code = ftpc_cmd(c, "USER", "anonymous", line, sizeof line);
    if (code != 331 && code / 100 != 2) {
        _snprintf(errbuf, errbuf_sz, "USER rejected: %.180s", line);
        goto done;
    }
    if (code == 331) {
        code = ftpc_cmd(c, "PASS", FTPC_ANON_PASSWORD, line, sizeof line);
        if (code / 100 != 2) {
            _snprintf(errbuf, errbuf_sz, "Anonymous login failed: %.180s", line);
            goto done;
        }
    }
    if (ftpc_cmd(c, "TYPE", "I", line, sizeof line) / 100 != 2) {
        ftp_err(errbuf, errbuf_sz, "Server refused binary mode.");
        goto done;
    }
    code = ftpc_data_open(c, 1, &d, line, sizeof line);
    if (code != 0) {
        ftp_err(errbuf, errbuf_sz, code > 0 ? "Server refused passive mode." : "Cannot open data connection.");
        goto done;
    }
    /* Open the local file only once the transfer is about to start. */
    fp = fopen(local_path, "wb");
    if (!fp) {
        _snprintf(errbuf, errbuf_sz, "Cannot create %s.", local_path);
        goto done;
    }
    code = ftpc_data_start(c, "RETR", remote_path, &d, line, sizeof line);
    if (code < 0 || code / 100 != 1) {
        _snprintf(errbuf, errbuf_sz, "RETR failed: %.180s", line);
        goto done;
    }
    buf = (char *)malloc(FTP_RECV_CHUNK);
    if (!buf) { ftp_err(errbuf, errbuf_sz, "Out of memory."); goto done; }
    for (;;) {
        int n = ftpc_data_recv(c, &d, buf, FTP_RECV_CHUNK);
        if (n == 0) break;
        if (n < 0) {
            ftp_err(errbuf, errbuf_sz, n == FTPC_ERR_STOPPED ? "Download cancelled." : "Data connection dropped.");
            goto done;
        }
        if (fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
            ftp_err(errbuf, errbuf_sz, "Cannot write to local file.");
            goto done;
        }
        total += n;
        if (progress) progress(user, total);
    }
    ftpc_data_close(c, &d);
    if (fclose(fp) != 0) { fp = NULL; ftp_err(errbuf, errbuf_sz, "Cannot flush local file."); goto done; }
    fp = NULL;
    code = ftpc_reply(c, line, sizeof line);
    if (code / 100 != 2) {
        _snprintf(errbuf, errbuf_sz, "Transfer incomplete: %.180s", line);
        goto done;
    }
    ok = 1;
    if (out_total) *out_total = total;

done:
    /* a cancel can land in any step: whatever that step said, say this */
    if (!ok && ftpc_aborted(c)) ftp_err(errbuf, errbuf_sz, "Download cancelled.");
    if (buf) free(buf);
    if (fp)  fclose(fp);
    ftpc_data_close(c, &d);
    if (ftpc_connected(c) && !ftpc_aborted(c)) ftpc_send_cmd(c, "QUIT", NULL);
    ftpc_release(c);
    if (!ok) remove(local_path);   /* never leave a partial file behind */
    return ok;
}

int ftp_fetch_file(const char *host, int port,
                   const char *remote_path, const char *local_path,
                   ftp_progress_cb progress, void *user,
                   long *out_total, char *errbuf, int errbuf_sz)
{
    return ftp_fetch_file_ex(host, port, remote_path, local_path, progress, user,
                             NULL, out_total, errbuf, errbuf_sz);
}
