/*
 * news_mime.h - decoding a Netnews article for display (UTF-16 out).
 *
 * Headers (RFC 5322 / 5536): unfolding; RFC 2047 encoded words (B and Q,
 * any charset Windows can convert; bytes of adjacent words in the same
 * charset are joined before conversion, so a UTF-8 character split across
 * two words survives); raw 8-bit header bytes as UTF-8, else Windows-1252.
 *
 * Bodies (RFC 2045 / 2046): Content-Type charset honoured WITH OR WITHOUT
 * MIME-Version (news.teletext.ca sends none, 2026-11-13);
 * Content-Transfer-Encoding 7bit / 8bit / binary as is, quoted-printable,
 * base64; a multipart body shows the text/plain part (preferred in
 * multipart/alternative) and lists every other part by type and name,
 * never opening it. A charset Windows cannot convert, or none at all:
 * UTF-8 if the bytes are valid UTF-8, else Windows-1252, and the result
 * says it guessed.
 *
 * Display safety: C0 controls other than TAB (ESC above all), DEL, C1
 * controls and the bidi embedding/override/isolate characters are
 * removed from everything returned.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */
#ifndef NEWS_MIME_H
#define NEWS_MIME_H

#include <wchar.h>

/* Windows code page for an IANA charset name, 0 if unknown or not
 * installed. Case and surrounding quotes are ignored. */
int nm_charset_cp(const char *name);

/* Bytes to a malloc'd, NUL-terminated, sanitised wide string. cp 0 means
 * "unknown": UTF-8 if valid, else Windows-1252 (*guessed set to 1). */
wchar_t *nm_decode_bytes(const char *b, int len, int cp, int *guessed);

/* One header VALUE (unfolded or not) to display text: RFC 2047 words
 * decoded, raw 8-bit as above, whitespace runs from folding collapsed. */
wchar_t *nm_decode_header(const char *value);

/* A parsed article. */
typedef struct nm_article {
    wchar_t *subject, *from, *date, *newsgroups, *message_id, *organization;
    wchar_t *body;            /* the text to show, '\n' between lines   */
    char     charset[48];     /* the charset used for the body          */
    char     cte[24];         /* the transfer encoding met              */
    int      guessed;         /* the body charset was guessed           */
    int      parts;           /* parts listed but not shown (multipart) */
} nm_article_t;

/* Parse an article given as its lines (no CR/LF, dot-unstuffed): headers,
 * an empty line, the body. Fills *a (free with nm_article_free). */
void nm_parse_article(char **lines, const int *lens, int nlines, nm_article_t *a);
void nm_article_free(nm_article_t *a);

/* For tests: decode quoted-printable / base64 into a malloc'd buffer. */
char *nm_qp_decode(const char *s, int len, int *outlen, int body);
char *nm_b64_decode(const char *s, int len, int *outlen);

#endif /* NEWS_MIME_H */
