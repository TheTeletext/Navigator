/*
 * suite_clipboard.c - CF_TEXT clipboard helpers. See suite_clipboard.h.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * OpenClipboard can fail with ERROR_ACCESS_DENIED when another
 * process is holding it open (browsers, password managers); we retry
 * once after an 80ms back-off, which is enough for the rare collision.
 */

#include "suite_clipboard.h"
#include "suite_w31.h"   /* w31_track_popup: the Windows 3.1 popup */

#include <stdlib.h>
#include <string.h>

static int open_clipboard_with_retry(HWND owner)
{
    if (OpenClipboard(owner)) return 1;
    Sleep(80);
    return OpenClipboard(owner) ? 1 : 0;
}

int suite_clipboard_put_text(HWND owner, const char *text)
{
    size_t  len;
    HGLOBAL hMem;
    void   *p;

    if (!text) return 0;
    len = strlen(text);

    if (!open_clipboard_with_retry(owner)) return 0;
    if (!EmptyClipboard()) { CloseClipboard(); return 0; }

    hMem = GlobalAlloc(GMEM_MOVEABLE, len + 1);
    if (!hMem) { CloseClipboard(); return 0; }
    p = GlobalLock(hMem);
    if (!p) { GlobalFree(hMem); CloseClipboard(); return 0; }
    memcpy(p, text, len + 1);
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_TEXT, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return 0;
    }
    /* Clipboard owns hMem now; do not free. */
    CloseClipboard();
    return 1;
}

char *suite_clipboard_get_text(HWND owner)
{
    HANDLE  hMem;
    LPCSTR  src;
    size_t  len;
    char   *dup;

    if (!open_clipboard_with_retry(owner)) return NULL;

    hMem = GetClipboardData(CF_TEXT);
    if (!hMem) { CloseClipboard(); return NULL; }

    src = (LPCSTR)GlobalLock(hMem);
    if (!src) { CloseClipboard(); return NULL; }

    len = strlen(src);
    dup = (char *)malloc(len + 1);
    if (dup) {
        memcpy(dup, src, len);
        dup[len] = '\0';
    }

    GlobalUnlock(hMem);
    CloseClipboard();
    return dup;
}

int suite_clipboard_has_text(void)
{
    return IsClipboardFormatAvailable(CF_TEXT) ? 1 : 0;
}

/* 2026-11-13 (docs/2026-11-13_NETNEWS_TAB.md): CF_UNICODETEXT for the
 * Newsgroups article pane, whose text is UTF-16. Windows synthesises
 * CF_TEXT from it for ANSI readers, so one format is put. The CF_TEXT
 * functions above are unchanged and every other tab keeps them. */
int suite_clipboard_put_wtext(HWND owner, const wchar_t *text)
{
    size_t  len;
    HGLOBAL hMem;
    void   *p;

    if (!text) return 0;
    len = wcslen(text);

    if (!open_clipboard_with_retry(owner)) return 0;
    if (!EmptyClipboard()) { CloseClipboard(); return 0; }

    hMem = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(wchar_t));
    if (!hMem) { CloseClipboard(); return 0; }
    p = GlobalLock(hMem);
    if (!p) { GlobalFree(hMem); CloseClipboard(); return 0; }
    memcpy(p, text, (len + 1) * sizeof(wchar_t));
    GlobalUnlock(hMem);

    if (!SetClipboardData(CF_UNICODETEXT, hMem)) {
        GlobalFree(hMem);
        CloseClipboard();
        return 0;
    }
    CloseClipboard();
    return 1;
}

/* The Copy / Select All context menu; see the header. This is the Web
 * tab's menu as it was (web_module.c, WM_RBUTTONUP), moved here when the
 * Newsgroups article pane became its second user (2026-11-13). */
int suite_clipboard_context_menu(HWND owner, int x, int y,
                                 int can_copy, int can_select_all)
{
    HMENU menu = CreatePopupMenu();
    UINT  cmd;
    if (!menu) return 0;
    AppendMenuA(menu, MF_STRING | (can_copy ? 0u : (UINT)MF_GRAYED),
                SUITE_CTX_COPY, "Copy\tCtrl+C");
    AppendMenuA(menu, MF_STRING | (can_select_all ? 0u : (UINT)MF_GRAYED),
                SUITE_CTX_SELECT_ALL, "Select All\tCtrl+A");
    /* the Windows 3.1 popup since 2026-11-13 (w31_track_popup, suite_w31.c) */
    cmd = (UINT)w31_track_popup(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
                                x, y, owner);
    DestroyMenu(menu);
    return (int)cmd;
}
