/*
 * ftp_list.h - parse one line of an FTP LIST reply (Unix "ls -l" format).
 *
 * Handles directories, files, symlinks ("name -> target"), sizes, both
 * date forms ("Nov 05 23:39" this year, "Apr 27  2022" older), names with
 * spaces, and listings without a group column. Anything else is returned
 * as a RAW line for the caller to show as it came (2026-11-12,
 * docs/2026-11-12_FTP_TAB.md).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#ifndef FTP_LIST_H
#define FTP_LIST_H

#define FTPL_FILE 0
#define FTPL_DIR  1
#define FTPL_LINK 2
#define FTPL_RAW  3   /* unparseable: name holds the line as it came */
#define FTPL_UP   4   /* the ".." entry the caller adds */
#define FTPL_DRIVE 5  /* local pane: a drive */

typedef struct ftp_entry {
    int       kind;
    char      name[260];
    char      target[260];   /* FTPL_LINK: where it points */
    long long size;          /* -1 unknown */
    char      date[20];      /* "Nov 05 23:39" or "Apr 27 2022" */
} ftp_entry_t;

/* Returns 1 (parsed), 0 (RAW: e->name is the line) or -1 (skip: blank,
 * "total N", "." and ".."). */
int ftp_list_parse(const char *line, ftp_entry_t *e);

#endif
