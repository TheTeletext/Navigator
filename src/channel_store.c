/*
 * channel_store.c - Active Channel subscriptions, cache and scheduled
 * refresh. See channel_store.h for the layout and the rules.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Teletext Desktop (Navigator 1.8), Phase 2A step 3,
 * docs/2026-11-20_TELETEXT_DESKTOP_PHASE2A.md.
 */
#include "channel_store.h"
#include "suite_netgate.h"
#include "suite_logo.h"
#include "suite_version.h"

#include <winhttp.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CS_WIDE2_(x)  L##x
#define CS_WIDE_(x)   CS_WIDE2_(x)
#define CS_UA         CS_WIDE_(SUITE_UA_PRODUCT) L"/" CS_WIDE_(SUITE_VERSION_STR)

#define CS_MAX_CDF    (1024 * 1024)
#define CS_MAX_LOGO   (256 * 1024)
#define CS_MAX_HOPS   5

enum { ROLE_BAR = 0, ROLE_WIDE = 1, ROLE_ICON = 2, ROLE_N = 3 };
static const wchar_t *const k_role_key[ROLE_N]  = { L"LogoBar", L"LogoWide", L"LogoIcon" };
static const char    *const k_role_file[ROLE_N] = { "logo_bar", "logo_wide", "logo_icon" };
static const CdfLogoRole   k_role_cdf[ROLE_N]  = { CDF_ROLE_BAR, CDF_ROLE_WIDE, CDF_ROLE_ICON };

typedef struct {
    char        id[9];
    char        url[1024];
    char        final_url[1024];
    char        title[256];
    char        seed_title[128];
    char        href[1024];
    char        abstract_[512];
    wchar_t     etag[256];
    wchar_t     lastmod[128];
    unsigned long long hash;
    /* Navigator 2.0 Gleam (2026-11-23): the item pages' signature (their
     * ETag / Last-Modified / status, by HEAD on each refresh); 0 = none
     * taken yet */
    unsigned long long items_hash;
    /* Navigator 2.0 LASTMOD mode (2026-11-24): the CDF carries LASTMOD;
     * the baselines were seeded; the channel baseline (largest LASTMOD at
     * the last open). The per-item baselines live only in the INI
     * (Seen_<id>), read and written where they are needed. */
    int         lm_mode;
    int         lm_seeded;
    long long   seen_channel;
    int         updated;
    long long   last_check;
    long long   last_change;
    int         status;
    wchar_t     logo[ROLE_N][32];
    CdfSchedule sched;
    int         queued;
    int         fetching;
    int         deleted;
} Chan;

static CRITICAL_SECTION g_cs;
static int       g_cs_ready = 0;
static Chan     *g_ch = NULL;
static int       g_n = 0, g_cap = 0;
static wchar_t   g_ini[MAX_PATH];
/* Channel Bar build (2026-11-26, docs/2026-11-26_CHANNEL_BAR_BUILD.md): the
 * LOCKED channel, first on the bar by construction and by chstore_move: the
 * first default seed (The Teletext, CHSTORE_DEFAULT_URL; under
 * NAV_CHANNEL_SEEDS the first stand-in, as home_module.c's g_home_url). */
static char      g_locked_id[9];
static wchar_t   g_cache[MAX_PATH];
static HWND      g_notify_hwnd = NULL;
static UINT      g_notify_msg = 0;
static int       g_retry_min = 30;
static HANDLE    g_thread = NULL;
static HANDLE    g_wake = NULL;
static volatile LONG g_stop = 0;
static int       g_abandoned = 0;   /* a worker outlived chstore_shutdown */

static const struct { const char *url; const char *title; } k_seeds[CHSTORE_SEEDS_N] = {
    { CHSTORE_DEFAULT_URL,                         "The Teletext" },
    { "http://www.cedmagic.com/home/cdf/cedmagic.cdf", "CED Magic" },
    { "http://www.moonsystem.to/channel.cdf",       "The Moon Age Calendar" },
    { "http://www.camsoftcorp.com/channel.cdf",     "CamSoft" },
};

/* Navigator 2.0 (part 8): the third-party defaults of 1.8, retired. An
 * install that has one is cleared of it once (Retired_<id>); a user who
 * adds one again afterwards keeps it. 2026-11-26 (D3): Moon and CamSoft
 * are default seeds again and so are off this list (it runs after the
 * seeding and would delete them from a fresh profile); Calvin stays. */
static const char *const k_retired[] = {
    "http://homepage.mcy.info/channel/calvin.cdf",
};

/* ------------------------------------------------------------------ */
/* Small helpers.                                                     */
/* ------------------------------------------------------------------ */

static void u8_to_w(const char *s, wchar_t *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (!s) return;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap)) out[0] = 0;
    out[cap - 1] = 0;
}

static void w_to_u8(const wchar_t *s, char *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (!s) return;
    if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, out, (int)cap, NULL, NULL)) out[0] = 0;
    out[cap - 1] = 0;
}

static void copy_u8(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (!cap) return;
    if (!src) { dst[0] = 0; return; }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) n--;   /* whole characters */
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

static long long now_unix(void) { return (long long)_time64(NULL); }

static void url_id(const char *url, char out[9])
{
    unsigned h = 2166136261u;
    for (; *url; url++) { h ^= (unsigned char)*url; h *= 16777619u; }
    snprintf(out, 9, "%08x", h);
}

static unsigned long long hash64(const char *b, size_t n)
{
    unsigned long long h = 14695981039346656037ull;
    size_t i;
    for (i = 0; i < n; i++) { h ^= (unsigned char)b[i]; h *= 1099511628211ull; }
    return h;
}

static int is_http_url(const char *u)
{
    return u && (_strnicmp(u, "http://", 7) == 0 || _strnicmp(u, "https://", 8) == 0);
}

/* ------------------------------------------------------------------ */
/* INI (UTF-16, profile API).                                         */
/* ------------------------------------------------------------------ */

static void ensure_ini(void)
{
    HANDLE f;
    wchar_t dir[MAX_PATH], *slash;
    if (GetFileAttributesW(g_ini) != INVALID_FILE_ATTRIBUTES) return;
    wcsncpy(dir, g_ini, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    slash = wcsrchr(dir, L'\\');
    if (slash) { *slash = 0; SHCreateDirectoryExW(NULL, dir, NULL); }
    /* A UTF-16 byte-order mark first, as suite_config.c writes it, so the
     * profile API keeps non-ANSI titles intact. */
    f = CreateFileW(g_ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD w;
        static const BYTE bom[2] = { 0xFF, 0xFE };
        WriteFile(f, bom, 2, &w, NULL);
        CloseHandle(f);
    }
}

static void sect_name(const char *id, wchar_t *out, size_t cap)
{
    _snwprintf(out, cap, L"Channel %S", id);
    out[cap - 1] = 0;
}

static void ini_get(const wchar_t *sect, const wchar_t *key, wchar_t *out, DWORD cap)
{
    GetPrivateProfileStringW(sect, key, L"", out, cap, g_ini);
}

static void ini_set(const wchar_t *sect, const wchar_t *key, const wchar_t *val)
{
    WritePrivateProfileStringW(sect, key, val, g_ini);
}

/* String values are written inside one pair of double quotes, because
 * GetPrivateProfileString strips one surrounding pair when it reads: an
 * ETag is itself quoted ("6b00d6d8-54e") and must come back exactly, or
 * If-None-Match never matches. */
static void ini_set_q(const wchar_t *sect, const wchar_t *key, const wchar_t *val)
{
    wchar_t w[1100];
    _snwprintf(w, 1100, L"\"%s\"", val ? val : L"");
    w[1099] = 0;
    ini_set(sect, key, w);
}

static void ini_set_u8(const wchar_t *sect, const wchar_t *key, const char *val)
{
    wchar_t w[1024];
    u8_to_w(val, w, 1024);
    ini_set_q(sect, key, w);
}

static void ini_set_ll(const wchar_t *sect, const wchar_t *key, long long v)
{
    wchar_t w[32];
    _snwprintf(w, 32, L"%lld", v);
    w[31] = 0;
    ini_set(sect, key, w);
}

static long long ini_get_ll(const wchar_t *sect, const wchar_t *key)
{
    wchar_t w[32];
    ini_get(sect, key, w, 32);
    return _wtoi64(w);
}

/* Caller holds g_cs. */
static void save_order(void)
{
    wchar_t buf[8192];
    size_t len = 0;
    int i;
    buf[0] = 0;
    for (i = 0; i < g_n; i++) {
        if (g_ch[i].deleted) continue;
        if (len + 10 >= sizeof buf / sizeof buf[0]) break;
        len += (size_t)_snwprintf(buf + len, 10, L"%s%S", len ? L"," : L"", g_ch[i].id);
    }
    buf[len] = 0;
    ini_set(L"Channels", L"Order", buf);
}

/* Caller holds g_cs. */
static void save_chan(const Chan *c)
{
    wchar_t s[64], w[64];
    sect_name(c->id, s, 64);
    ini_set_u8(s, L"URL", c->url);
    ini_set_u8(s, L"FinalURL", c->final_url);
    ini_set_u8(s, L"Href", c->href);
    ini_set_u8(s, L"Title", c->title);
    ini_set_u8(s, L"SeedTitle", c->seed_title);
    ini_set_u8(s, L"Abstract", c->abstract_);
    ini_set_ll(s, L"LastCheck", c->last_check);
    ini_set_ll(s, L"LastChange", c->last_change);
    ini_set_ll(s, L"Status", c->status);
    ini_set(s, L"Updated", c->updated ? L"1" : L"0");
    ini_set_q(s, L"ETag", c->etag);
    ini_set_q(s, L"LastModified", c->lastmod);
    _snwprintf(w, 64, L"%016llx", c->hash);
    w[63] = 0;
    ini_set(s, L"Hash", w);
    _snwprintf(w, 64, L"%016llx", c->items_hash);
    w[63] = 0;
    ini_set(s, L"ItemsHash", w);
    ini_set(s, L"LmSeeded", c->lm_seeded ? L"1" : L"0");
    ini_set_ll(s, L"SeenChannel", c->seen_channel);
    ini_set_q(s, k_role_key[ROLE_BAR], c->logo[ROLE_BAR]);
    ini_set_q(s, k_role_key[ROLE_WIDE], c->logo[ROLE_WIDE]);
    ini_set_q(s, k_role_key[ROLE_ICON], c->logo[ROLE_ICON]);
}

/* ------------------------------------------------------------------ */
/* Cache files.                                                       */
/* ------------------------------------------------------------------ */

static void chan_dir(const char *id, wchar_t *out, size_t cap)
{
    _snwprintf(out, cap, L"%s\\%S", g_cache, id);
    out[cap - 1] = 0;
}

static void chan_file(const char *id, const wchar_t *name, wchar_t *out, size_t cap)
{
    _snwprintf(out, cap, L"%s\\%S\\%s", g_cache, id, name);
    out[cap - 1] = 0;
}

static int write_file_atomic(const wchar_t *path, const void *data, size_t n)
{
    wchar_t tmp[MAX_PATH + 8];
    HANDLE f;
    DWORD w = 0;
    int ok;
    _snwprintf(tmp, MAX_PATH + 8, L"%s.tmp", path);
    tmp[MAX_PATH + 7] = 0;
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(f, data, (DWORD)n, &w, NULL) && w == (DWORD)n;
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

static char *read_file(const wchar_t *path, size_t *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    char *b;
    DWORD r = 0;
    *len = 0;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart > CS_MAX_CDF) { CloseHandle(f); return NULL; }
    b = (char *)malloc((size_t)sz.QuadPart + 1);
    if (b && (!ReadFile(f, b, (DWORD)sz.QuadPart, &r, NULL) || r != (DWORD)sz.QuadPart)) { free(b); b = NULL; }
    CloseHandle(f);
    if (b) { b[r] = 0; *len = r; }
    return b;
}

static void remove_dir_files(const wchar_t *dir)
{
    wchar_t pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snwprintf(pat, MAX_PATH, L"%s\\*", dir);
    pat[MAX_PATH - 1] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            _snwprintf(p, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            p[MAX_PATH - 1] = 0;
            DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static const char *image_ext(const unsigned char *b, size_t n)
{
    if (n >= 4 && memcmp(b, "GIF8", 4) == 0) return "gif";
    if (n >= 4 && b[0] == 0 && b[1] == 0 && (b[2] == 1 || b[2] == 2) && b[3] == 0) return "ico";
    if (n >= 8 && memcmp(b, "\x89PNG", 4) == 0) return "png";
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return "jpg";
    if (n >= 2 && b[0] == 'B' && b[1] == 'M') return "bmp";
    return "bin";
}

/* ------------------------------------------------------------------ */
/* HTTP GET over WinHTTP (http and https, manual redirects).          */
/* ------------------------------------------------------------------ */

typedef struct {
    int     status;             /* HTTP status, or CHSTORE_E_* */
    char   *body;
    size_t  len;
    wchar_t ctype[128];
    wchar_t etag[256];
    wchar_t lastmod[128];
    char    final_url[1024];
} Resp;

static void resp_free(Resp *r) { free(r->body); r->body = NULL; r->len = 0; }

static void query_hdr(HINTERNET req, DWORD what, wchar_t *out, DWORD cap_chars)
{
    DWORD sz = cap_chars * sizeof(wchar_t);
    out[0] = 0;
    if (!WinHttpQueryHeaders(req, what, WINHTTP_HEADER_NAME_BY_INDEX, out, &sz, WINHTTP_NO_HEADER_INDEX))
        out[0] = 0;
    out[cap_chars - 1] = 0;
}

/* GET, or HEAD for the Gleam's item check (no body is read: a HEAD answer
 * has none). */
static void http_req(const wchar_t *verb, const char *url, const wchar_t *etag,
                     const wchar_t *lastmod, size_t max, Resp *r);
static void http_get(const char *url, const wchar_t *etag, const wchar_t *lastmod,
                     size_t max, Resp *r)
{
    http_req(L"GET", url, etag, lastmod, max, r);
}

static void http_req(const wchar_t *verb, const char *url, const wchar_t *etag,
                     const wchar_t *lastmod, size_t max, Resp *r)
{
    char cur[1024];
    int hop;
    memset(r, 0, sizeof *r);
    copy_u8(cur, sizeof cur, url);
    for (hop = 0; hop <= CS_MAX_HOPS; hop++) {
        wchar_t wurl[1024], host[256], path[2048], loc[1024], hdr[512];
        URL_COMPONENTS uc;
        HINTERNET ses = NULL, con = NULL, req = NULL;
        DWORD status = 0, sz, flags;
        int secure;

        if (InterlockedCompareExchange(&g_stop, 0, 0)) { r->status = CHSTORE_E_CANCEL; return; }
        if (!is_http_url(cur)) { r->status = CHSTORE_E_URL; return; }
        copy_u8(r->final_url, sizeof r->final_url, cur);
        u8_to_w(cur, wurl, 1024);
        memset(&uc, 0, sizeof uc);
        uc.dwStructSize = sizeof uc;
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
        {
            wchar_t extra[1024];
            uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
            if (!WinHttpCrackUrl(wurl, 0, 0, &uc)) { r->status = CHSTORE_E_URL; return; }
            if (!path[0]) wcscpy(path, L"/");
            if (extra[0] && wcslen(path) + wcslen(extra) < 2048) wcscat(path, extra);
        }
        secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);

        ses = WinHttpOpen(CS_UA, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) { r->status = CHSTORE_E_NET; return; }
        WinHttpSetTimeouts(ses, 10000, 10000, 10000, 30000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
        if (con) req = WinHttpOpenRequest(con, verb, path, NULL, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (!req) {
            if (con) WinHttpCloseHandle(con);
            WinHttpCloseHandle(ses);
            r->status = CHSTORE_E_NET;
            return;
        }
        flags = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &flags, sizeof flags);
        if (etag && etag[0]) {
            _snwprintf(hdr, 512, L"If-None-Match: %s", etag); hdr[511] = 0;
            WinHttpAddRequestHeaders(req, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        }
        if (lastmod && lastmod[0]) {
            _snwprintf(hdr, 512, L"If-Modified-Since: %s", lastmod); hdr[511] = 0;
            WinHttpAddRequestHeaders(req, hdr, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
        }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(req, NULL)) {
            WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
            r->status = CHSTORE_E_NET;
            return;
        }
        sz = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        r->status = (int)status;
        query_hdr(req, WINHTTP_QUERY_CONTENT_TYPE, r->ctype, 128);
        query_hdr(req, WINHTTP_QUERY_ETAG, r->etag, 256);
        query_hdr(req, WINHTTP_QUERY_LAST_MODIFIED, r->lastmod, 128);

        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            char loc8[1024], *next;
            query_hdr(req, WINHTTP_QUERY_LOCATION, loc, 1024);
            WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
            w_to_u8(loc, loc8, sizeof loc8);
            next = loc8[0] ? cdf_url_resolve(cur, loc8) : NULL;
            if (!next) return;              /* a redirect with nowhere to go: its status stands */
            copy_u8(cur, sizeof cur, next);
            free(next);
            etag = NULL; lastmod = NULL;    /* validators belong to the first URL */
            if (hop == CS_MAX_HOPS) { r->status = CHSTORE_E_REDIRECT; return; }
            continue;
        }
        if (status == 200 && wcscmp(verb, L"HEAD") != 0) {
            size_t cap = 16384;
            r->body = (char *)malloc(cap + 1);
            for (;;) {
                DWORD avail = 0, got = 0;
                if (!r->body) { r->status = CHSTORE_E_NET; break; }
                if (InterlockedCompareExchange(&g_stop, 0, 0)) { r->status = CHSTORE_E_CANCEL; break; }
                if (!WinHttpQueryDataAvailable(req, &avail)) { r->status = CHSTORE_E_NET; break; }
                if (avail == 0) break;
                if (r->len + avail > max) { r->status = CHSTORE_E_TOOBIG; break; }
                if (r->len + avail > cap) {
                    char *nb;
                    while (r->len + avail > cap) cap *= 2;
                    nb = (char *)realloc(r->body, cap + 1);
                    if (!nb) { r->status = CHSTORE_E_NET; break; }
                    r->body = nb;
                }
                if (!WinHttpReadData(req, r->body + r->len, avail, &got)) { r->status = CHSTORE_E_NET; break; }
                r->len += got;
            }
            if (r->status != 200) resp_free(r);
            else r->body[r->len] = 0;
        }
        WinHttpCloseHandle(req); WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Schedule.                                                          */
/* ------------------------------------------------------------------ */

int chstore_schedule_due(const CdfSchedule *s, long long last, long long now)
{
    return chstore_schedule_due_floor(s, last, now, 15);
}

/* The CDF's INTERVALTIME is the channel's cadence; Navigator's own default
 * (once a day) when it gives none; never under the floor. Navigator 2.0
 * (2026-11-24): the floor is a parameter so a LASTMOD channel (The
 * Teletext: INTERVALTIME MIN=1) is polled every minute, the brief's "never
 * faster than once a minute"; the UI tick itself is one minute. */
int chstore_schedule_due_floor(const CdfSchedule *s, long long last, long long now,
                               int floor_minutes)
{
    long long interval = 1440;
    struct tm tmv;
    __time64_t t;
    int mod, ymd;
    if (floor_minutes < 1) floor_minutes = 1;
    if (s && s->interval.present && s->interval.minutes > 0) interval = s->interval.minutes;
    if (interval < floor_minutes) interval = floor_minutes;

    if (s && s->tz_present) {
        t = (__time64_t)(now + (long long)s->tz_minutes * 60);
        if (_gmtime64_s(&tmv, &t) != 0) return 0;
    } else {
        t = (__time64_t)now;
        if (_localtime64_s(&tmv, &t) != 0) return 0;
    }
    ymd = (tmv.tm_year + 1900) * 10000 + (tmv.tm_mon + 1) * 100 + tmv.tm_mday;
    if (s && s->start_ymd && ymd < s->start_ymd) return 0;
    if (s && s->stop_ymd && ymd > s->stop_ymd) return 0;

    if (last <= 0) return 1;
    if (now - last < interval * 60) return 0;
    if (now - last >= 2 * interval * 60) return 1;     /* catch-up */

    if (s && (s->earliest.present || s->latest.present)) {
        int e = s->earliest.present ? s->earliest.minutes : 0;
        int l = s->latest.present ? s->latest.minutes : (int)interval;
        mod = tmv.tm_hour * 60 + tmv.tm_min;
        if (interval < 1440) mod = (int)(mod % interval);
        if (e <= l) return mod >= e && mod <= l;
        return mod >= e || mod <= l;                    /* window wraps */
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* One update (worker thread).                                        */
/* ------------------------------------------------------------------ */

#define LM_MAX_ITEMS 64

typedef struct {
    int         status;
    int         changed;        /* new CDF bytes */
    int         have_model;
    char        final_url[1024];
    char        title[256];
    char        href[1024];
    char        abstract_[512];
    wchar_t     etag[256];
    wchar_t     lastmod[128];
    unsigned long long hash;
    CdfSchedule sched;
    int         logos_done;
    wchar_t     logo[ROLE_N][32];
    int         items_checked;  /* the Gleam's item check ran */
    unsigned long long items_hash;
    /* LASTMOD mode (Navigator 2.0, 2026-11-24): the model's largest LASTMOD
     * and each item's (key = FNV-1a of its resolved HREF), for seeding */
    int         lm_mode;
    long long   lm_max;
    int         lm_n;
    struct { char key[9]; long long lm; } lm_items[LM_MAX_ITEMS];
    int         used_fallback;  /* the default channel came from www */
} Result;

/* ---- the Gleam's item check (Navigator 2.0, 2026-11-23) -------------
 * A channel's CDF can stay byte for byte the same while its pages change
 * (The Teletext's does: five fixed ITEMs whose pages are rewritten through
 * the day). The period clients looked at the pages too (the period
 * Windows 98 shell's channel subscription crawled them).
 * So each refresh also sends a HEAD to every
 * ITEM page (http/https, at most GLEAM_MAX_ITEMS, document order) and
 * hashes what the server says about it: the URL, the status, the ETag and
 * the Last-Modified. A page that gives neither validator counts only by
 * its URL and status (its changes cannot be seen this way). */
#define GLEAM_MAX_ITEMS 16
typedef struct { const char *href[GLEAM_MAX_ITEMS]; int n; } ItemList;

static int collect_item(const CdfNode *n, int depth, void *ctx)
{
    ItemList *l = (ItemList *)ctx;
    (void)depth;
    if (n->kind == CDF_NODE_ITEM && n->href && is_http_url(n->href) && l->n < GLEAM_MAX_ITEMS)
        l->href[l->n++] = n->href;
    return 0;                       /* 0 = walk on */
}

static int check_items(const CdfModel *m, unsigned long long *out)
{
    ItemList l;
    size_t cap = 4096, len = 0;
    char *sig;
    int i;
    memset(&l, 0, sizeof l);
    cdf_model_walk(m, collect_item, &l);
    if (l.n == 0) return 0;
    sig = (char *)malloc(cap);
    if (!sig) return 0;
    for (i = 0; i < l.n; i++) {
        Resp r;
        char line[1600], et[256], lm[128];
        int k;
        if (InterlockedCompareExchange(&g_stop, 0, 0)) { free(sig); return 0; }
        http_req(L"HEAD", l.href[i], NULL, NULL, 0, &r);
        resp_free(&r);
        if (r.status == CHSTORE_E_NET || r.status == CHSTORE_E_CANCEL) {
            free(sig);              /* a page out of reach says nothing: no verdict */
            return 0;
        }
        w_to_u8(r.etag, et, sizeof et);
        w_to_u8(r.lastmod, lm, sizeof lm);
        k = _snprintf(line, sizeof line, "%s\n%d\n%s\n%s\n", l.href[i], r.status, et, lm);
        if (k < 0) k = (int)sizeof line - 1;
        if (len + (size_t)k + 1 > cap) {
            char *nb;
            while (len + (size_t)k + 1 > cap) cap *= 2;
            nb = (char *)realloc(sig, cap);
            if (!nb) { free(sig); return 0; }
            sig = nb;
        }
        memcpy(sig + len, line, (size_t)k);
        len += (size_t)k;
    }
    *out = hash64(sig, len);
    free(sig);
    return 1;
}

typedef struct { const char *href; const char *href_fetch; CdfLogoStyle style; } LogoRef;

/* GET each distinct channel logo, measure it, keep the best per role. */
static void fetch_logos(const char *id, const CdfNode *root, Result *res)
{
    struct { char *bytes; size_t len; int w, h; CdfLogoStyle style; } got[CDF_MAX_LOGOS];
    int ng = 0, i, role;
    memset(got, 0, sizeof got);
    for (i = 0; i < root->logo_n && ng < CDF_MAX_LOGOS; i++) {
        const CdfLogo *lg = &root->logos[i];
        Resp r;
        int j, dup = 0, attempt;
        for (j = 0; j < i; j++)
            if (root->logos[j].href && lg->href && strcmp(root->logos[j].href, lg->href) == 0
                && root->logos[j].style == lg->style) dup = 1;
        if (dup || !lg->href) continue;
        for (attempt = 0; attempt < 2; attempt++) {
            const char *u = attempt == 0 ? lg->href : lg->href_fetch;
            SuiteImage *img;
            if (!u) break;
            http_get(u, NULL, NULL, CS_MAX_LOGO, &r);
            if (r.status == CHSTORE_E_CANCEL) { resp_free(&r); goto done; }
            if (r.status != 200 || !r.body) { resp_free(&r); continue; }
            img = suite_image_from_bytes(r.body, r.len);
            if (!img) { resp_free(&r); continue; }
            suite_image_size(img, &got[ng].w, &got[ng].h);
            suite_image_free(img);
            got[ng].bytes = r.body;
            got[ng].len = r.len;
            got[ng].style = lg->style;
            ng++;
            break;
        }
    }
    for (role = 0; role < ROLE_N; role++) {
        int best = -1, best_score = 0;
        for (i = 0; i < ng; i++) {
            int sc = cdf_logo_score(got[i].style, got[i].w, got[i].h, k_role_cdf[role]);
            if (sc > best_score) { best_score = sc; best = i; }
        }
        res->logo[role][0] = 0;
        if (best >= 0) {
            wchar_t name[32], path[MAX_PATH];
            _snwprintf(name, 32, L"%S.%S", k_role_file[role],
                       image_ext((const unsigned char *)got[best].bytes, got[best].len));
            name[31] = 0;
            chan_file(id, name, path, MAX_PATH);
            if (write_file_atomic(path, got[best].bytes, got[best].len))
                wcscpy(res->logo[role], name);
        }
    }
    res->logos_done = 1;
done:
    for (i = 0; i < ng; i++) free(got[i].bytes);
}

/* ---- entry icons (Channels pane fidelity, 2026-11-25,
 * docs/2026-11-25_CHANNELS_PANE_IE4_FIDELITY.md) -------------------------
 * An ITEM or sub-CHANNEL may carry its own LOGO STYLE="ICON" (Microsoft LOGO
 * reference, aa768150: parents CHANNEL, ITEM, SOFTPKG; the ICON "will
 * represent the ITEM in the Channels Explorer Bar"). The pane shows it, so
 * each DISTINCT entry icon that is not one of the channel's own ICON files
 * (those are logo_icon.*) is cached as item_<FNV-1a of its href>.<ext>; a
 * href that could not be had is remembered as item_<key>.none (an empty
 * file), so a broken icon is not fetched again on every refresh. They are
 * fetched with the channel logos (a changed CDF, a missing logo file) and
 * when one of them has neither file yet. */
#define ITEM_ICON_MAX 32
typedef struct { const char *href[ITEM_ICON_MAX]; const char *href_fetch[ITEM_ICON_MAX]; int n;
                 const CdfNode *root; } IconList;

static int root_has_icon_href(const CdfNode *root, const char *href)
{
    int i;
    for (i = 0; i < root->logo_n; i++)
        if (root->logos[i].style == CDF_LOGO_ICON && root->logos[i].href
            && strcmp(root->logos[i].href, href) == 0) return 1;
    return 0;
}

static int collect_icon(const CdfNode *n, int depth, void *ctx)
{
    IconList *l = (IconList *)ctx;
    int i, j;
    if (depth == 0) return 0;
    for (i = 0; i < n->logo_n && l->n < ITEM_ICON_MAX; i++) {
        const CdfLogo *lg = &n->logos[i];
        int dup = 0;
        if (lg->style != CDF_LOGO_ICON || !lg->href || !is_http_url(lg->href)) continue;
        if (root_has_icon_href(l->root, lg->href)) continue;
        for (j = 0; j < l->n; j++) if (strcmp(l->href[j], lg->href) == 0) dup = 1;
        if (dup) continue;
        l->href[l->n] = lg->href;
        l->href_fetch[l->n] = lg->href_fetch;
        l->n++;
    }
    return 0;
}

/* The cached file for an entry icon href: 1 and its path if it exists (an
 * image or the .none marker; *is_none says which). */
static int item_icon_file(const char *id, const char *href, wchar_t *out, size_t cap, int *is_none)
{
    char key[9];
    wchar_t pat[MAX_PATH], name[64];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    url_id(href, key);
    _snwprintf(name, 64, L"item_%S.*", key);
    name[63] = 0;
    chan_file(id, name, pat, MAX_PATH);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    chan_file(id, fd.cFileName, out, cap);
    if (is_none) {
        size_t k = wcslen(fd.cFileName);
        *is_none = k >= 5 && _wcsicmp(fd.cFileName + k - 5, L".none") == 0;
    }
    return 1;
}

static int item_icons_missing(const char *id, const CdfModel *m)
{
    IconList l;
    int i;
    wchar_t p[MAX_PATH];
    memset(&l, 0, sizeof l);
    l.root = m->root;
    cdf_model_walk(m, collect_icon, &l);
    for (i = 0; i < l.n; i++)
        if (!item_icon_file(id, l.href[i], p, MAX_PATH, NULL)) return 1;
    return 0;
}

static void fetch_item_icons(const char *id, const CdfModel *m)
{
    IconList l;
    int i;
    wchar_t dir[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    /* stale entry icons go first (an icon the channel dropped or changed) */
    chan_dir(id, dir, MAX_PATH);
    chan_file(id, L"item_*", pat, MAX_PATH);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[MAX_PATH];
            chan_file(id, fd.cFileName, p, MAX_PATH);
            DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    memset(&l, 0, sizeof l);
    l.root = m->root;
    cdf_model_walk(m, collect_icon, &l);
    for (i = 0; i < l.n; i++) {
        char key[9];
        wchar_t name[64], path[MAX_PATH];
        int attempt, ok = 0;
        url_id(l.href[i], key);
        for (attempt = 0; attempt < 2 && !ok; attempt++) {
            const char *u = attempt == 0 ? l.href[i] : l.href_fetch[i];
            Resp r;
            SuiteImage *img;
            if (!u) break;
            http_get(u, NULL, NULL, CS_MAX_LOGO, &r);
            if (r.status == CHSTORE_E_CANCEL) { resp_free(&r); return; }
            if (r.status == 200 && r.body && (img = suite_image_from_bytes(r.body, r.len)) != NULL) {
                suite_image_free(img);
                _snwprintf(name, 64, L"item_%S.%S", key, image_ext((const unsigned char *)r.body, r.len));
                name[63] = 0;
                chan_file(id, name, path, MAX_PATH);
                ok = write_file_atomic(path, r.body, r.len);
            }
            resp_free(&r);
        }
        if (!ok) {
            _snwprintf(name, 64, L"item_%S.none", key);
            name[63] = 0;
            chan_file(id, name, path, MAX_PATH);
            write_file_atomic(path, "", 0);
        }
    }
    (void)dir;
}

static int logo_files_missing(const char *id, wchar_t logos[ROLE_N][32])
{
    int role;
    for (role = 0; role < ROLE_N; role++) {
        wchar_t p[MAX_PATH];
        if (!logos[role][0]) continue;
        chan_file(id, logos[role], p, MAX_PATH);
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) return 1;
    }
    return 0;
}

/* LASTMOD mode: every node with a LASTMOD and an HREF, keyed by the FNV-1a
 * of the resolved HREF (the same key chstore_item_is_new computes). The
 * CHANNEL root is left out: it has the channel baseline (SeenChannel). */
static void item_key(const char *href, char out[9]) { url_id(href ? href : "", out); }

static int collect_lm(const CdfNode *n, int depth, void *ctx)
{
    Result *res = (Result *)ctx;
    long long k = cdf_lastmod_key(n);
    if (depth == 0 || !k || !n->href || res->lm_n >= LM_MAX_ITEMS) return 0;
    item_key(n->href, res->lm_items[res->lm_n].key);
    res->lm_items[res->lm_n].lm = k;
    res->lm_n++;
    return 0;
}

static void update_one(const Chan *snap, Result *res)
{
    Resp r;
    wchar_t dir[MAX_PATH], cdfp[MAX_PATH];
    CdfModel *m = NULL;
    int need_logos = 0;
    wchar_t old_logos[ROLE_N][32];

    memset(res, 0, sizeof *res);
    memcpy(old_logos, snap->logo, sizeof old_logos);
    chan_dir(snap->id, dir, MAX_PATH);
    chan_file(snap->id, L"channel.cdf", cdfp, MAX_PATH);
    {
        /* Send validators only while the cached copy is really there. */
        int cached = GetFileAttributesW(cdfp) != INVALID_FILE_ATTRIBUTES;
        http_get(snap->url, cached ? snap->etag : NULL, cached ? snap->lastmod : NULL, CS_MAX_CDF, &r);
        /* Navigator 2.0 (2026-11-24): The Teletext's channel host out of
         * reach (or not serving the file): the same CDF from www, its
         * back-compat home. Unconditional (the validators belong to the
         * channel host), and not kept (see the worker). */
        if (_stricmp(snap->url, CHSTORE_DEFAULT_URL) == 0
            && (r.status == CHSTORE_E_NET || r.status == 404 || r.status >= 500)) {
            resp_free(&r);
            http_get(CHSTORE_FALLBACK_URL, NULL, NULL, CS_MAX_CDF, &r);
            res->used_fallback = 1;
        }
    }
    res->status = r.status;
    copy_u8(res->final_url, sizeof res->final_url, r.final_url[0] ? r.final_url : snap->url);

    if (r.status == 304) {
        size_t len;
        char *b = read_file(cdfp, &len);
        wcscpy(res->etag, snap->etag);
        wcscpy(res->lastmod, snap->lastmod);
        res->hash = snap->hash;
        copy_u8(res->final_url, sizeof res->final_url, snap->final_url[0] ? snap->final_url : snap->url);
        if (b) {
            m = cdf_model_parse(b, len, res->final_url);
            free(b);
        }
        need_logos = m && (logo_files_missing(snap->id, old_logos)
                           || (!old_logos[0][0] && !old_logos[1][0] && !old_logos[2][0] && m->root->logo_n > 0));
    } else if (r.status == 200) {
        m = cdf_model_parse(r.body, r.len, res->final_url);
        if (!m) {
            res->status = CHSTORE_E_NOTCDF;
        } else {
            res->hash = hash64(r.body, r.len);
            wcscpy(res->etag, r.etag);
            wcscpy(res->lastmod, r.lastmod);
            if (res->hash != snap->hash || GetFileAttributesW(cdfp) == INVALID_FILE_ATTRIBUTES) {
                SHCreateDirectoryExW(NULL, dir, NULL);
                if (write_file_atomic(cdfp, r.body, r.len)) {
                    res->changed = (res->hash != snap->hash);
                    need_logos = 1;
                } else {
                    res->status = CHSTORE_E_NET;
                }
            } else {
                need_logos = logo_files_missing(snap->id, old_logos);
            }
        }
    }
    resp_free(&r);

    if (m) {
        res->have_model = 1;
        copy_u8(res->title, sizeof res->title, m->root->title);
        copy_u8(res->href, sizeof res->href, m->root->href);
        copy_u8(res->abstract_, sizeof res->abstract_, m->root->abstract);
        res->sched = m->root->schedule;
        if (need_logos && res->status != CHSTORE_E_NET) {
            int role;
            /* Stale role files go first, so a logo the channel dropped
             * does not linger. */
            for (role = 0; role < ROLE_N; role++) {
                if (old_logos[role][0]) {
                    wchar_t p[MAX_PATH];
                    chan_file(snap->id, old_logos[role], p, MAX_PATH);
                    DeleteFileW(p);
                }
            }
            fetch_logos(snap->id, m->root, res);
        }
        /* entry icons (2026-11-25): with the logos, or when one is missing */
        if (res->status != CHSTORE_E_NET && (need_logos || item_icons_missing(snap->id, m)))
            fetch_item_icons(snap->id, m);
        /* LASTMOD mode (Navigator 2.0, 2026-11-24): the CDF says itself
         * what changed; no HEAD requests. Otherwise the 2.0 Gleam: the item
         * pages, on every refresh that reached the CDF. */
        if (cdf_model_has_lastmod(m)) {
            res->lm_mode = 1;
            res->lm_max = cdf_model_max_lastmod(m);
            cdf_model_walk(m, collect_lm, res);
        } else if (res->status == 200 || res->status == 304) {
            res->items_checked = check_items(m, &res->items_hash);
        }
        cdf_model_free(m);
    }
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    while (!InterlockedCompareExchange(&g_stop, 0, 0)) {
        int i, pick = -1;
        Chan snap;
        Result res;
        EnterCriticalSection(&g_cs);
        for (i = 0; i < g_n; i++)
            if (g_ch[i].queued && !g_ch[i].deleted) { pick = i; break; }
        if (pick >= 0) {
            g_ch[pick].queued = 0;
            g_ch[pick].fetching = 1;
            snap = g_ch[pick];
        }
        LeaveCriticalSection(&g_cs);
        if (pick < 0) {
            WaitForSingleObject(g_wake, 1000);
            continue;
        }

        if (!netgate_acquire(&g_stop, INFINITE)) {
            int k;
            EnterCriticalSection(&g_cs);
            for (k = 0; k < g_n; k++)        /* 2026-11-26: by id (chstore_move) */
                if (g_ch[k].fetching && strcmp(g_ch[k].id, snap.id) == 0) { g_ch[k].fetching = 0; break; }
            LeaveCriticalSection(&g_cs);
            break;
        }
        update_one(&snap, &res);
        netgate_release();

        EnterCriticalSection(&g_cs);
        {
            Chan *c;
            /* 2026-11-26: chstore_move may have reordered the array while
             * the fetch ran unlocked: find the channel again by its id (the
             * one marked fetching). */
            {
                int k;
                for (k = 0; k < g_n; k++)
                    if (g_ch[k].fetching && strcmp(g_ch[k].id, snap.id) == 0) { pick = k; break; }
            }
            c = &g_ch[pick];
            c->fetching = 0;
            if (c->deleted) {
                wchar_t dir[MAX_PATH];
                chan_dir(c->id, dir, MAX_PATH);
                remove_dir_files(dir);
            } else if (res.status != CHSTORE_E_CANCEL) {
                c->last_check = now_unix();
                c->status = res.status;
                if (res.status == 200 || res.status == 304) {
                    copy_u8(c->final_url, sizeof c->final_url, res.final_url);
                    if (res.have_model) {
                        copy_u8(c->title, sizeof c->title, res.title);
                        copy_u8(c->href, sizeof c->href, res.href);
                        copy_u8(c->abstract_, sizeof c->abstract_, res.abstract_);
                        c->sched = res.sched;
                    }
                    wcscpy(c->etag, res.etag);
                    wcscpy(c->lastmod, res.lastmod);
                    if (res.used_fallback) {
                        /* www's validators would be sent to the channel
                         * host next time: keep none */
                        c->etag[0] = 0;
                        c->lastmod[0] = 0;
                    }
                    if (res.have_model) c->lm_mode = res.lm_mode;
                    if (res.lm_mode) {
                        /* LASTMOD mode (Navigator 2.0, 2026-11-24). */
                        wchar_t s[64];
                        sect_name(c->id, s, 64);
                        if (!c->lm_seeded) {
                            /* first refresh in this mode: SEED, no gleam */
                            int k;
                            for (k = 0; k < res.lm_n; k++) {
                                wchar_t key[32];
                                _snwprintf(key, 32, L"Seen_%S", res.lm_items[k].key);
                                key[31] = 0;
                                ini_set_ll(s, key, res.lm_items[k].lm);
                            }
                            c->seen_channel = res.lm_max;
                            c->lm_seeded = 1;
                            c->updated = 0;
                        } else {
                            int was = c->updated;
                            c->updated = res.lm_max > c->seen_channel;
                            if (c->updated && !was) c->last_change = c->last_check;
                        }
                    } else {
                    if (res.changed) {
                        if (c->hash != 0) c->updated = 1;   /* never on the first fetch */
                        c->last_change = c->last_check;
                    }
                    /* Navigator 2.0 Gleam: a page of the channel changed
                     * since the last refresh (never on the first check) */
                    if (res.items_checked) {
                        if (c->items_hash != 0 && res.items_hash != c->items_hash) {
                            c->updated = 1;
                            c->last_change = c->last_check;
                        }
                        c->items_hash = res.items_hash;
                    }
                    }   /* not LASTMOD mode */
                    c->hash = res.hash;
                    if (res.logos_done) memcpy(c->logo, res.logo, sizeof c->logo);
                }
                save_chan(c);
            }
        }
        LeaveCriticalSection(&g_cs);
        if (g_notify_hwnd && g_notify_msg)
            PostMessageW(g_notify_hwnd, g_notify_msg, (WPARAM)(res.changed ? 1 : 0), 0);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API.                                                        */
/* ------------------------------------------------------------------ */

/* Caller holds g_cs. Visible index -> array index. */
static int real_index(int index)
{
    int i, k = 0;
    for (i = 0; i < g_n; i++) {
        if (g_ch[i].deleted) continue;
        if (k == index) return i;
        k++;
    }
    return -1;
}

/* Caller holds g_cs. Array index of a URL or id, or -1. */
static int find_real(const char *url_or_id)
{
    int i;
    char id[9];
    if (!url_or_id) return -1;
    if (is_http_url(url_or_id)) url_id(url_or_id, id);
    else copy_u8(id, sizeof id, url_or_id);
    for (i = 0; i < g_n; i++)
        if (!g_ch[i].deleted && strcmp(g_ch[i].id, id) == 0) return i;
    return -1;
}

/* Caller holds g_cs. */
static Chan *append_chan(void)
{
    if (g_n == g_cap) {
        int nc = g_cap ? g_cap * 2 : 8;
        Chan *nch = (Chan *)realloc(g_ch, sizeof(Chan) * (size_t)nc);
        if (!nch) return NULL;
        g_ch = nch;
        g_cap = nc;
    }
    memset(&g_ch[g_n], 0, sizeof(Chan));
    return &g_ch[g_n++];
}

static void load_one(const char *id)
{
    wchar_t s[64], w[1024];
    Chan *c;
    sect_name(id, s, 64);
    ini_get(s, L"URL", w, 1024);
    if (!w[0]) return;
    c = append_chan();
    if (!c) return;
    copy_u8(c->id, sizeof c->id, id);
    w_to_u8(w, c->url, sizeof c->url);
    ini_get(s, L"FinalURL", w, 1024); w_to_u8(w, c->final_url, sizeof c->final_url);
    ini_get(s, L"Title", w, 1024);    w_to_u8(w, c->title, sizeof c->title);
    ini_get(s, L"Href", w, 1024);     w_to_u8(w, c->href, sizeof c->href);
    ini_get(s, L"SeedTitle", w, 1024); w_to_u8(w, c->seed_title, sizeof c->seed_title);
    ini_get(s, L"Abstract", w, 1024); w_to_u8(w, c->abstract_, sizeof c->abstract_);
    c->last_check = ini_get_ll(s, L"LastCheck");
    c->last_change = ini_get_ll(s, L"LastChange");
    c->status = (int)ini_get_ll(s, L"Status");
    ini_get(s, L"Updated", w, 8); c->updated = (w[0] == L'1');
    ini_get(s, L"ETag", c->etag, 256);
    ini_get(s, L"LastModified", c->lastmod, 128);
    ini_get(s, L"Hash", w, 32); c->hash = _wcstoui64(w, NULL, 16);
    ini_get(s, L"ItemsHash", w, 32); c->items_hash = _wcstoui64(w, NULL, 16);
    c->lm_seeded = (int)ini_get_ll(s, L"LmSeeded") != 0;
    c->seen_channel = ini_get_ll(s, L"SeenChannel");
    ini_get(s, k_role_key[ROLE_BAR], c->logo[ROLE_BAR], 32);
    ini_get(s, k_role_key[ROLE_WIDE], c->logo[ROLE_WIDE], 32);
    ini_get(s, k_role_key[ROLE_ICON], c->logo[ROLE_ICON], 32);
    /* The schedule comes from the cached CDF. */
    {
        wchar_t p[MAX_PATH];
        size_t len;
        char *b;
        chan_file(c->id, L"channel.cdf", p, MAX_PATH);
        b = read_file(p, &len);
        if (b) {
            CdfModel *m = cdf_model_parse(b, len, c->final_url[0] ? c->final_url : c->url);
            if (m) {
                c->sched = m->root->schedule;
                c->lm_mode = cdf_model_has_lastmod(m);
                cdf_model_free(m);
            }
            free(b);
        }
    }
}

/* Navigator 2.0 (2026-11-24): The Teletext's channel moved from
 * www.teletext.ca to channel.teletext.ca. An install subscribed to the www
 * copy is moved ONCE ([Channels] MigratedChannelHost=1) to the new URL: the
 * whole INI section is copied under the new id (gleam state, baselines,
 * logos), the cache folder renamed, the Channel Bar order kept, the
 * validators dropped (they belong to the old host) and the desktop items
 * that point at the old id ([Desktop Item *] Channel=) re-pointed. The
 * once-ever flags DefaultSubscribed / DefaultDeleted are not keyed by id,
 * so they carry over. Caller holds g_cs. */
int chstore_delete(const char *url_or_id);
static void migrate_default_host(void)
{
    char oid[9], nid[9];
    int oi, ni;
    if (GetPrivateProfileIntW(L"Channels", L"MigratedChannelHost", 0, g_ini)) return;
    url_id(CHSTORE_FALLBACK_URL, oid);
    url_id(CHSTORE_DEFAULT_URL, nid);
    oi = find_real(oid);
    ni = find_real(nid);
    if (oi >= 0 && ni >= 0) {
        chstore_delete(oid);                /* both: the new one is The Teletext */
    } else if (oi >= 0) {
        Chan *c = &g_ch[oi];
        wchar_t os[64], ns[64], od[MAX_PATH], nd[MAX_PATH];
        wchar_t *sec = (wchar_t *)malloc(32768 * sizeof(wchar_t));
        sect_name(oid, os, 64);
        sect_name(nid, ns, 64);
        if (sec) {
            DWORD n = GetPrivateProfileSectionW(os, sec, 32768, g_ini);
            if (n > 0 && n < 32766) WritePrivateProfileSectionW(ns, sec, g_ini);
            free(sec);
        }
        WritePrivateProfileStringW(os, NULL, NULL, g_ini);
        chan_dir(oid, od, MAX_PATH);
        chan_dir(nid, nd, MAX_PATH);
        if (GetFileAttributesW(od) != INVALID_FILE_ATTRIBUTES) MoveFileW(od, nd);
        copy_u8(c->id, sizeof c->id, nid);
        copy_u8(c->url, sizeof c->url, CHSTORE_DEFAULT_URL);
        c->final_url[0] = 0;
        c->etag[0] = 0;
        c->lastmod[0] = 0;
        save_chan(c);
        save_order();
        {   /* the desktop items' Channel= (desktop_items.c's layout) */
            wchar_t order[256], *tok, *ctx = NULL, want[16];
            _snwprintf(want, 16, L"%S", oid);
            want[15] = 0;
            GetPrivateProfileStringW(L"Desktop", L"Order", L"", order, 256, g_ini);
            for (tok = wcstok_s(order, L",", &ctx); tok; tok = wcstok_s(NULL, L",", &ctx)) {
                wchar_t s[64], ch[64], v[32];
                _snwprintf(s, 64, L"Desktop Item %s", tok);
                s[63] = 0;
                GetPrivateProfileStringW(s, L"Channel", L"", ch, 64, g_ini);
                if (_wcsicmp(ch, want) == 0) {
                    _snwprintf(v, 32, L"\"%S\"", nid);
                    v[31] = 0;
                    WritePrivateProfileStringW(s, L"Channel", v, g_ini);
                }
            }
        }
    }
    ini_set(L"Channels", L"MigratedChannelHost", L"1");
}

/* Seeding flags: The Teletext keeps its 1.8 keys DefaultSubscribed /
 * DefaultDeleted (Phase 2A); every other default has Seeded_<id> /
 * Deleted_<id>. Caller holds g_cs. */
static void seed_keys(const char *id, wchar_t *seeded, wchar_t *deleted, size_t cap)
{
    char def[9];
    url_id(CHSTORE_DEFAULT_URL, def);
    if (strcmp(id, def) == 0) {
        _snwprintf(seeded, cap, L"DefaultSubscribed");
        _snwprintf(deleted, cap, L"DefaultDeleted");
    } else {
        _snwprintf(seeded, cap, L"Seeded_%S", id);
        _snwprintf(deleted, cap, L"Deleted_%S", id);
    }
    seeded[cap - 1] = deleted[cap - 1] = 0;
}

static void seed_one(const char *url)
{
    char id[9];
    wchar_t ks[32], kd[32];
    int i;
    if (!is_http_url(url) || strlen(url) >= 1024) return;
    url_id(url, id);
    seed_keys(id, ks, kd, 32);
    if (GetPrivateProfileIntW(L"Channels", ks, 0, g_ini)
        || GetPrivateProfileIntW(L"Channels", kd, 0, g_ini)) return;
    if (find_real(id) < 0) {
        Chan *c = append_chan();
        if (c) {
            copy_u8(c->id, sizeof c->id, id);
            copy_u8(c->url, sizeof c->url, url);
            for (i = 0; i < CHSTORE_SEEDS_N; i++)
                if (strcmp(k_seeds[i].url, url) == 0)
                    copy_u8(c->seed_title, sizeof c->seed_title, k_seeds[i].title);
            save_chan(c);
        }
    }
    ini_set(L"Channels", ks, L"1");
    save_order();
}

int chstore_init(const ChStoreInit *in)
{
    wchar_t order[8192], *tok, *ctx = NULL;
    if (!in || !in->ini_path || !in->ini_path[0]) return 0;
    if (g_thread) return 1;
    if (g_abandoned) return 0;      /* a stuck worker still owns the state */
    if (!g_cs_ready) { InitializeCriticalSection(&g_cs); g_cs_ready = 1; }

    wcsncpy(g_ini, in->ini_path, MAX_PATH - 1);
    g_ini[MAX_PATH - 1] = 0;
    if (in->cache_dir && in->cache_dir[0]) {
        wcsncpy(g_cache, in->cache_dir, MAX_PATH - 1);
    } else {
        wchar_t *slash;
        wcsncpy(g_cache, g_ini, MAX_PATH - 1);
        g_cache[MAX_PATH - 1] = 0;
        slash = wcsrchr(g_cache, L'\\');
        if (slash) slash[1] = 0; else g_cache[0] = 0;
        wcsncat(g_cache, L"Channels", MAX_PATH - 1 - wcslen(g_cache));
    }
    g_cache[MAX_PATH - 1] = 0;
    g_notify_hwnd = in->notify_hwnd;
    g_notify_msg = in->notify_msg;
    g_retry_min = in->retry_minutes > 0 ? in->retry_minutes : 30;
    ensure_ini();

    EnterCriticalSection(&g_cs);
    g_n = 0;
    GetPrivateProfileStringW(L"Channels", L"Order", L"", order, 8192, g_ini);
    for (tok = wcstok_s(order, L",", &ctx); tok; tok = wcstok_s(NULL, L",", &ctx)) {
        char id[16];
        w_to_u8(tok, id, sizeof id);
        if (strlen(id) == 8) load_one(id);
    }
    migrate_default_host();
    /* The defaults, each once ever: never again after the user deletes it. */
    {
        char list[4096];
        char *tok8, *ctx8 = NULL;
        int k = 0;
        list[0] = 0;
        if (in->seed_override) {
            copy_u8(list, sizeof list, in->seed_override);
        } else {
            for (k = 0; k < CHSTORE_SEEDS_N; k++) {
                if (k) strncat(list, ";", sizeof list - strlen(list) - 1);
                strncat(list, k_seeds[k].url, sizeof list - strlen(list) - 1);
            }
        }
        {   /* 2026-11-26: the locked channel is the first seed */
            char first[1024];
            size_t fl = strcspn(list, ";");
            if (fl >= sizeof first) fl = sizeof first - 1;
            memcpy(first, list, fl);
            first[fl] = 0;
            g_locked_id[0] = 0;
            if (first[0]) url_id(first, g_locked_id);
        }
        for (tok8 = strtok_s(list, ";", &ctx8); tok8; tok8 = strtok_s(NULL, ";", &ctx8))
            seed_one(tok8);
    }
    /* Navigator 2.0 (part 8): the retired defaults go, once. (g_cs is a
     * critical section: chstore_delete re-enters it on this thread.) */
    {
        size_t r;
        for (r = 0; r < sizeof k_retired / sizeof k_retired[0]; r++) {
            char id[9];
            wchar_t key[32];
            url_id(k_retired[r], id);
            _snwprintf(key, 32, L"Retired_%S", id);
            key[31] = 0;
            if (GetPrivateProfileIntW(L"Channels", key, 0, g_ini)) continue;
            if (find_real(id) >= 0) chstore_delete(id);
            ini_set(L"Channels", key, L"1");
        }
    }
    LeaveCriticalSection(&g_cs);

    InterlockedExchange(&g_stop, 0);
    g_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    return g_thread != NULL;
}

void chstore_shutdown(void)
{
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    SetEvent(g_wake);
    if (WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0) {
        /* Stuck in a slow server (WinHTTP's own timeouts run to 30 s).
         * Never hold up Navigator's close, and never TerminateThread a
         * thread that may own the heap or g_cs: leave it to process exit,
         * and leave its state alone. */
        g_abandoned = 1;
        CloseHandle(g_thread);
        g_thread = NULL;
        return;
    }
    CloseHandle(g_thread);
    CloseHandle(g_wake);
    g_thread = NULL;
    g_wake = NULL;
    EnterCriticalSection(&g_cs);
    free(g_ch);
    g_ch = NULL;
    g_n = g_cap = 0;
    LeaveCriticalSection(&g_cs);
}

int chstore_count(void)
{
    int i, k = 0;
    if (!g_cs_ready) return 0;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_n; i++) if (!g_ch[i].deleted) k++;
    LeaveCriticalSection(&g_cs);
    return k;
}

int chstore_get(int index, ChStoreChannel *out)
{
    int i, role;
    if (!out || !g_cs_ready) return 0;
    memset(out, 0, sizeof *out);
    EnterCriticalSection(&g_cs);
    i = real_index(index);
    if (i >= 0) {
        const Chan *c = &g_ch[i];
        wchar_t *dst[ROLE_N];
        copy_u8(out->id, sizeof out->id, c->id);
        copy_u8(out->url, sizeof out->url, c->url);
        copy_u8(out->final_url, sizeof out->final_url, c->final_url);
        copy_u8(out->title, sizeof out->title, c->title);
        copy_u8(out->href, sizeof out->href, c->href);
        if (c->title[0]) copy_u8(out->label, sizeof out->label, c->title);
        else if (c->seed_title[0]) copy_u8(out->label, sizeof out->label, c->seed_title);
        else {
            /* The URL's host. */
            const char *h = strstr(c->url, "://");
            size_t n;
            h = h ? h + 3 : c->url;
            n = strcspn(h, "/:?#");
            if (n >= sizeof out->label) n = sizeof out->label - 1;
            memcpy(out->label, h, n);
            out->label[n] = 0;
        }
        copy_u8(out->abstract_, sizeof out->abstract_, c->abstract_);
        out->updated = c->updated;
        out->last_check = c->last_check;
        out->last_change = c->last_change;
        out->status = c->status;
        out->busy = c->queued || c->fetching;
        chan_file(c->id, L"channel.cdf", out->cdf_path, MAX_PATH);
        if (GetFileAttributesW(out->cdf_path) == INVALID_FILE_ATTRIBUTES) out->cdf_path[0] = 0;
        dst[ROLE_BAR] = out->logo_bar; dst[ROLE_WIDE] = out->logo_wide; dst[ROLE_ICON] = out->logo_icon;
        for (role = 0; role < ROLE_N; role++)
            if (c->logo[role][0]) chan_file(c->id, c->logo[role], dst[role], MAX_PATH);
    }
    LeaveCriticalSection(&g_cs);
    return i >= 0;
}

int chstore_find(const char *url_or_id)
{
    int i, k = -1;
    if (!g_cs_ready) return -1;
    EnterCriticalSection(&g_cs);
    i = find_real(url_or_id);
    if (i >= 0) {
        int j;
        k = 0;
        for (j = 0; j < i; j++) if (!g_ch[j].deleted) k++;
    }
    LeaveCriticalSection(&g_cs);
    return k;
}

/* A URL as a key for "is this channel already subscribed?": the scheme and
 * host in lower case, the default port (80 for http, 443 for https)
 * dropped, the fragment dropped, an empty path made "/". The path and query
 * keep their case (servers may treat them as case-sensitive). Returns 0
 * for anything that is not http or https. Active Channel subscribe P2,
 * docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md. */
static int url_norm(const char *u, char *out, size_t cap)
{
    const char *p, *host, *host_end, *port = NULL, *rest;
    size_t n = 0, k;
    int https;
    if (!is_http_url(u) || cap < 16) return 0;
    https = (_strnicmp(u, "https://", 8) == 0);
    p = u + (https ? 8 : 7);
    host = p;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    host_end = p;
    rest = p;
    {   /* user info is never part of a channel key; skip "user@" */
        const char *at;
        for (at = host; at < host_end; at++) if (*at == '@') host = at + 1;
    }
    for (k = 0; host + k < host_end; k++) if (host[k] == ':') { port = host + k; break; }
    n = (size_t)snprintf(out, cap, "%s", https ? "https://" : "http://");
    for (p = host; p < host_end && n + 1 < cap; p++) {
        if (port && p == port) {
            const char *d = port + 1;
            int dflt = d == host_end
                || (https && host_end - d == 3 && strncmp(d, "443", 3) == 0)
                || (!https && host_end - d == 2 && strncmp(d, "80", 2) == 0);
            if (dflt) break;
        }
        out[n++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    }
    if (*rest != '/' && n + 1 < cap) out[n++] = '/';
    for (p = rest; *p && *p != '#' && n + 1 < cap; p++) out[n++] = *p;
    out[n] = 0;
    return 1;
}

int chstore_find_url(const char *url)
{
    char want[1100], have[1100];
    int i, k = 0, found = -1;
    char fb[1100];
    if (!g_cs_ready || !url_norm(url, want, sizeof want)) return -1;
    /* Navigator 2.0 (2026-11-24): the www copy of The Teletext's CDF is the
     * same channel as the channel-host one (alias). */
    if (url_norm(CHSTORE_FALLBACK_URL, fb, sizeof fb) && strcmp(fb, want) == 0)
        url_norm(CHSTORE_DEFAULT_URL, want, sizeof want);
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_n; i++) {
        if (g_ch[i].deleted) continue;
        if ((url_norm(g_ch[i].url, have, sizeof have) && strcmp(have, want) == 0)
            || (g_ch[i].final_url[0] && url_norm(g_ch[i].final_url, have, sizeof have)
                && strcmp(have, want) == 0)) {
            found = k;
            break;
        }
        k++;
    }
    LeaveCriticalSection(&g_cs);
    return found;
}

int chstore_subscribe(const char *url)
{
    int i, idx;
    char id[9];
    if (!g_cs_ready || !is_http_url(url) || strlen(url) >= 1024) return -1;
    url_id(url, id);
    EnterCriticalSection(&g_cs);
    i = find_real(id);
    if (i < 0) {
        Chan *c = append_chan();
        if (c) {
            copy_u8(c->id, sizeof c->id, id);
            copy_u8(c->url, sizeof c->url, url);
            c->queued = 1;
            save_chan(c);
            save_order();
        }
    }
    LeaveCriticalSection(&g_cs);
    if (g_wake) SetEvent(g_wake);
    idx = chstore_find(id);
    return idx;
}

int chstore_delete(const char *url_or_id)
{
    int i;
    if (!g_cs_ready) return 0;
    EnterCriticalSection(&g_cs);
    i = find_real(url_or_id);
    if (i >= 0) {
        Chan *c = &g_ch[i];
        wchar_t s[64];
        wchar_t ks[32], kd[32];
        c->deleted = 1;
        c->queued = 0;
        sect_name(c->id, s, 64);
        WritePrivateProfileStringW(s, NULL, NULL, g_ini);
        /* Deleted stays deleted: a default is never seeded again. (Written
         * for every channel; it matters only for defaults.) */
        seed_keys(c->id, ks, kd, 32);
        ini_set(L"Channels", kd, L"1");
        save_order();
        if (!c->fetching) {                 /* else the worker clears it when done */
            wchar_t dir[MAX_PATH];
            chan_dir(c->id, dir, MAX_PATH);
            remove_dir_files(dir);
        }
    }
    LeaveCriticalSection(&g_cs);
    return i >= 0;
}

int chstore_update_now(const char *url_or_id)
{
    int i, n = 0;
    if (!g_cs_ready) return 0;
    EnterCriticalSection(&g_cs);
    if (!url_or_id) {
        for (i = 0; i < g_n; i++)
            if (!g_ch[i].deleted && !g_ch[i].fetching) { g_ch[i].queued = 1; n++; }
    } else {
        i = find_real(url_or_id);
        if (i >= 0 && !g_ch[i].fetching) { g_ch[i].queued = 1; n = 1; }
    }
    LeaveCriticalSection(&g_cs);
    if (n && g_wake) SetEvent(g_wake);
    return n;
}

int chstore_tick(void)
{
    int i, n = 0;
    long long now = now_unix();
    if (!g_cs_ready) return 0;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_n; i++) {
        Chan *c = &g_ch[i];
        int due;
        if (c->deleted || c->queued || c->fetching) continue;
        if (c->last_check > 0 && c->status != 200 && c->status != 304)
            due = (now - c->last_check) >= (long long)g_retry_min * 60;     /* retry a failure */
        else
            due = chstore_schedule_due_floor(c->sched.present ? &c->sched : NULL, c->last_check, now,
                                             c->lm_mode ? 1 : 15);
        if (due) { c->queued = 1; n++; }
    }
    LeaveCriticalSection(&g_cs);
    if (n && g_wake) SetEvent(g_wake);
    return n;
}

int chstore_mark_seen(const char *url_or_id)
{
    int i;
    CdfModel *m;
    if (!g_cs_ready) return 0;
    /* LASTMOD mode: the cached CDF tells what "now" is (read outside the
     * lock: chstore_load_model takes it). */
    m = chstore_load_model(url_or_id);
    EnterCriticalSection(&g_cs);
    i = find_real(url_or_id);
    if (i >= 0) {
        wchar_t s[64];
        sect_name(g_ch[i].id, s, 64);
        if (g_ch[i].updated) {
            g_ch[i].updated = 0;
            ini_set(s, L"Updated", L"0");
        }
        if (m && g_ch[i].lm_mode && g_ch[i].lm_seeded) {
            long long mx = cdf_model_max_lastmod(m);
            int k;
            if (mx > g_ch[i].seen_channel) {
                g_ch[i].seen_channel = mx;
                ini_set_ll(s, L"SeenChannel", mx);
            }
            /* the page the channel opens on is read too */
            for (k = 0; m->root->href && k < m->root->kid_n; k++) {
                const CdfNode *n = m->root->kids[k];
                long long lm = cdf_lastmod_key(n);
                if (n->kind == CDF_NODE_ITEM && lm && n->href && strcmp(n->href, m->root->href) == 0) {
                    char key8[9];
                    wchar_t key[32];
                    item_key(n->href, key8);
                    _snwprintf(key, 32, L"Seen_%S", key8);
                    key[31] = 0;
                    if (ini_get_ll(s, key) < lm) ini_set_ll(s, key, lm);
                }
            }
        }
    }
    LeaveCriticalSection(&g_cs);
    if (m) cdf_model_free(m);
    return i >= 0;
}

int chstore_item_is_new(const char *url_or_id, const CdfNode *item)
{
    int i, is_new = 0;
    long long lm = cdf_lastmod_key(item);
    if (!g_cs_ready || !item || !item->href || !lm) return 0;
    EnterCriticalSection(&g_cs);
    i = find_real(url_or_id);
    if (i >= 0 && g_ch[i].lm_mode && g_ch[i].lm_seeded) {
        wchar_t s[64], key[32];
        char key8[9];
        sect_name(g_ch[i].id, s, 64);
        item_key(item->href, key8);
        _snwprintf(key, 32, L"Seen_%S", key8);
        key[31] = 0;
        /* no baseline = an item that appeared after seeding: new */
        is_new = lm > ini_get_ll(s, key);
    }
    LeaveCriticalSection(&g_cs);
    return is_new;
}

void chstore_mark_item_seen(const char *url_or_id, const CdfNode *item)
{
    int i;
    long long lm = cdf_lastmod_key(item);
    if (!g_cs_ready || !item || !item->href || !lm) return;
    EnterCriticalSection(&g_cs);
    i = find_real(url_or_id);
    if (i >= 0 && g_ch[i].lm_mode && g_ch[i].lm_seeded) {
        wchar_t s[64], key[32];
        char key8[9];
        sect_name(g_ch[i].id, s, 64);
        item_key(item->href, key8);
        _snwprintf(key, 32, L"Seen_%S", key8);
        key[31] = 0;
        if (ini_get_ll(s, key) < lm) ini_set_ll(s, key, lm);
    }
    LeaveCriticalSection(&g_cs);
}

CdfModel *chstore_load_model(const char *url_or_id)
{
    int i;
    char id[9], base[1024];
    CdfModel *m = NULL;
    if (!g_cs_ready) return NULL;
    EnterCriticalSection(&g_cs);
    i = find_real(url_or_id);
    if (i >= 0) {
        copy_u8(id, sizeof id, g_ch[i].id);
        copy_u8(base, sizeof base, g_ch[i].final_url[0] ? g_ch[i].final_url : g_ch[i].url);
    }
    LeaveCriticalSection(&g_cs);
    if (i >= 0) {
        wchar_t p[MAX_PATH];
        size_t len;
        char *b;
        chan_file(id, L"channel.cdf", p, MAX_PATH);
        b = read_file(p, &len);
        if (b) { m = cdf_model_parse(b, len, base); free(b); }
    }
    return m;
}

int chstore_item_icon_path(const char *url_or_id, const char *icon_href, wchar_t *out, size_t cap)
{
    int i, is_none = 0;
    char id[9];
    if (!g_cs_ready || !icon_href || !out || cap == 0) return 0;
    out[0] = 0;
    EnterCriticalSection(&g_cs);
    i = find_real(url_or_id);
    if (i >= 0) copy_u8(id, sizeof id, g_ch[i].id);
    LeaveCriticalSection(&g_cs);
    if (i < 0 || !item_icon_file(id, icon_href, out, cap, &is_none) || is_none) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

/* Channel Bar build (2026-11-26): reorder by drag and drop. new_index is a
 * position among the channels as chstore_get counts them. The locked
 * channel never moves, and nothing may take its place at 0 while it is
 * there: a move to 0 lands at 1. 1 if the order changed. */
int chstore_move(const char *url_or_id, int new_index)
{
    int i, vis_n = 0, k, from_vis = -1, locked_at0;
    Chan tmp;
    if (!g_cs_ready || !url_or_id) return 0;
    EnterCriticalSection(&g_cs);
    i = find_real(url_or_id);
    if (i < 0 || (g_locked_id[0] && strcmp(g_ch[i].id, g_locked_id) == 0)) {
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    for (k = 0; k < g_n; k++) {
        if (g_ch[k].deleted) continue;
        if (k == i) from_vis = vis_n;
        vis_n++;
    }
    {
        int r0 = real_index(0);
        locked_at0 = r0 >= 0 && g_locked_id[0] && strcmp(g_ch[r0].id, g_locked_id) == 0;
    }
    if (new_index < (locked_at0 ? 1 : 0)) new_index = locked_at0 ? 1 : 0;
    if (new_index > vis_n - 1) new_index = vis_n - 1;
    if (new_index == from_vis) { LeaveCriticalSection(&g_cs); return 0; }
    /* take it out, then put it before the channel now at new_index (or at
     * the end), deleted entries left where they are */
    tmp = g_ch[i];
    memmove(&g_ch[i], &g_ch[i + 1], (size_t)(g_n - i - 1) * sizeof g_ch[0]);
    g_n--;
    {
        int at = g_n, seen = 0;
        for (k = 0; k < g_n; k++) {
            if (g_ch[k].deleted) continue;
            if (seen == new_index) { at = k; break; }
            seen++;
        }
        memmove(&g_ch[at + 1], &g_ch[at], (size_t)(g_n - at) * sizeof g_ch[0]);
        g_ch[at] = tmp;
        g_n++;
    }
    save_order();
    LeaveCriticalSection(&g_cs);
    return 1;
}

int chstore_is_locked(const char *url_or_id)
{
    char id[9];
    if (!url_or_id || !g_locked_id[0]) return 0;
    if (is_http_url(url_or_id)) url_id(url_or_id, id);
    else copy_u8(id, sizeof id, url_or_id);
    return strcmp(id, g_locked_id) == 0;
}

int chstore_wait_idle(DWORD max_ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        int i, busy = 0;
        if (!g_cs_ready) return 1;
        EnterCriticalSection(&g_cs);
        for (i = 0; i < g_n; i++) if (g_ch[i].queued || g_ch[i].fetching) busy = 1;
        LeaveCriticalSection(&g_cs);
        if (!busy) return 1;
        if (GetTickCount() - start >= max_ms) return 0;
        Sleep(50);
    }
}
