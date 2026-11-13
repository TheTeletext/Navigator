/*
 * ftp_list.c - parse one line of an FTP LIST reply. See ftp_list.h.
 *
 * The anchor is the date, not a fixed column: the first token that is a
 * month name, preceded by an all-digit size and followed by a day and a
 * time or year, with a name after it. So "drwxr-xr-x 2 owner group 4096
 * Nov 05 23:39 pub" and the group-less "drwxr-xr-x 2 owner 4096 Nov 05
 * 23:39 pub" both parse, and the name keeps any spaces it has.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ftp_list.h"

#define MAXTOK 16

static int is_month(const char *s, int n)
{
    static const char *m[] = { "jan", "feb", "mar", "apr", "may", "jun",
                               "jul", "aug", "sep", "oct", "nov", "dec" };
    int i;
    if (n != 3) return 0;
    for (i = 0; i < 12; i++)
        if (tolower((unsigned char)s[0]) == m[i][0] && tolower((unsigned char)s[1]) == m[i][1] &&
            tolower((unsigned char)s[2]) == m[i][2]) return 1;
    return 0;
}

static int all_digits(const char *s, int n)
{
    int i;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static int is_time_or_year(const char *s, int n)
{
    if (n == 4 && all_digits(s, 4)) return 1;
    if ((n == 5 && isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && s[2] == ':' &&
         isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4])) ||
        (n == 4 && isdigit((unsigned char)s[0]) && s[1] == ':' &&
         isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3]))) return 1;
    return 0;
}

static void copy_raw(ftp_entry_t *e, const char *line)
{
    memset(e, 0, sizeof *e);
    e->kind = FTPL_RAW;
    e->size = -1;
    strncpy(e->name, line, sizeof e->name - 1);
}

int ftp_list_parse(const char *line, ftp_entry_t *e)
{
    const char *ts[MAXTOK];
    int         tl[MAXTOK];
    int         nt = 0, m, len;
    const char *p = line, *name;
    char        k;

    while (*p == ' ' || *p == '\t') p++;
    len = (int)strlen(p);
    while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == '\n')) len--;
    if (len == 0) return -1;
    if (len > 5 && _strnicmp(p, "total", 5) == 0 && (p[5] == ' ' || p[5] == '\t')) return -1;

    /* tokens of the first MAXTOK words (the name's own words come after) */
    {
        const char *q = p, *end = p + len;
        while (q < end && nt < MAXTOK) {
            while (q < end && (*q == ' ' || *q == '\t')) q++;
            if (q >= end) break;
            ts[nt] = q;
            while (q < end && *q != ' ' && *q != '\t') q++;
            tl[nt] = (int)(q - ts[nt]);
            nt++;
        }
    }
    {
        char buf[600];
        int n = len < (int)sizeof buf - 1 ? len : (int)sizeof buf - 1;
        memcpy(buf, p, (size_t)n); buf[n] = '\0';
        k = p[0];
        if (tl[0] < 10 || !strchr("-dlbcps", k)) { copy_raw(e, buf); return 0; }
        for (m = 3; m + 3 < nt; m++) {
            if (is_month(ts[m], tl[m]) && all_digits(ts[m - 1], tl[m - 1]) &&
                tl[m + 1] <= 2 && all_digits(ts[m + 1], tl[m + 1]) &&
                is_time_or_year(ts[m + 2], tl[m + 2]))
                break;
        }
        if (m + 3 >= nt) { copy_raw(e, buf); return 0; }

        memset(e, 0, sizeof *e);
        e->size = _strtoi64(ts[m - 1], NULL, 10);
        _snprintf(e->date, sizeof e->date, "%.3s %.*s %.*s", ts[m], tl[m + 1], ts[m + 1],
                  tl[m + 2], ts[m + 2]);
        e->date[sizeof e->date - 1] = '\0';
        name = ts[m + 3];
        n = (int)(p + len - name);
        if (n <= 0) { copy_raw(e, buf); return 0; }
        if (n > (int)sizeof e->name - 1) n = (int)sizeof e->name - 1;
        memcpy(e->name, name, (size_t)n);
        e->name[n] = '\0';
        if (k == 'd') e->kind = FTPL_DIR;
        else if (k == 'l') {
            char *arrow = strstr(e->name, " -> ");
            e->kind = FTPL_LINK;
            if (arrow) {
                strncpy(e->target, arrow + 4, sizeof e->target - 1);
                *arrow = '\0';
            }
        } else e->kind = FTPL_FILE;
        if (strcmp(e->name, ".") == 0 || strcmp(e->name, "..") == 0) return -1;
        return 1;
    }
}
