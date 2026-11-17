/*
 * suite_config.c - Settings > Configuration and the configuration file.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See suite_config.h for the file and its one key. The dialog follows the
 * Windows 3.1 Program Item Properties grammar: a label and its field on
 * the left, the command buttons stacked down the right (OK, Cancel,
 * Browse...), a note under the field. It is an in-memory DLGTEMPLATE like
 * the Gopher search dialog, so the Windows 3.1 hook (suite_w31.c) gives it
 * the period frame and font. Browse opens the system folder picker with
 * the hook suspended, as the Open and Save dialogs elsewhere do.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "suite_config.h"
#include "suite_shell.h"
#include "suite_w31.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define CFG_SECTION   L"Navigator"
#define CFG_KEY_DL    L"DownloadsDir"
#define CFG_KEY_SIM   L"BBSModemSimulation"   /* 1.7.2: 1 (default) or 0 */
#define CFG_FILE_NAME L"NAVIGATR.INI"
#define CFG_APP_DIR   L"Teletext Navigator"

/* ------------------------------------------------------------------ */
/* The file                                                            */
/* ------------------------------------------------------------------ */

static void wcopy(wchar_t *dst, size_t cap, const wchar_t *src)
{
    size_t n;
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    n = wcslen(src);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, src, n * sizeof(wchar_t));
    dst[n] = 0;
}

wchar_t *suite_config_file_w(wchar_t *out, size_t cap)
{
    wchar_t base[MAX_PATH];
    DWORD n;

    if (!out || cap == 0) return out;
    out[0] = 0;

    n = GetEnvironmentVariableW(L"NAV_CONFIG_INI", base, MAX_PATH);
    if (n > 0 && n < MAX_PATH) { wcopy(out, cap, base); return out; }

    n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base)))
            base[0] = 0;
    }
    if (!base[0]) return out;
    _snwprintf(out, cap - 1, L"%s\\%s\\%s", base, CFG_APP_DIR, CFG_FILE_NAME);
    out[cap - 1] = 0;
    return out;
}

/* The profile API writes a new file in the ANSI code page unless the file
 * already starts with a UTF-16 byte-order mark, in which case it reads and
 * writes UTF-16. So the file is created with the mark before the first
 * write, and a path in any script survives. */
static int ensure_file(const wchar_t *path)
{
    wchar_t dir[MAX_PATH], *slash;
    HANDLE h;

    if (!path || !path[0]) return 0;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return 1;

    wcopy(dir, MAX_PATH, path);
    slash = wcsrchr(dir, L'\\');
    if (slash) { *slash = 0; suite_config_ensure_dir_w(dir); }

    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    {
        static const unsigned char bom[2] = { 0xFF, 0xFE };
        DWORD wr = 0;
        WriteFile(h, bom, 2, &wr, NULL);
    }
    CloseHandle(h);
    return 1;
}

int suite_config_ensure_dir_w(const wchar_t *dir)
{
    int rc;
    if (!dir || !dir[0]) return 0;
    if (GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) return 1;
    rc = SHCreateDirectoryExW(NULL, dir, NULL);
    return (rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS ||
            GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES);
}

static void default_downloads_dir(wchar_t *out, size_t cap)
{
    PWSTR known = NULL;
    out[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Downloads, 0, NULL, &known)) && known) {
        _snwprintf(out, cap - 1, L"%s\\%s", known, CFG_APP_DIR);
        out[cap - 1] = 0;
    }
    if (known) CoTaskMemFree(known);
    if (!out[0]) {
        wchar_t prof[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, prof))) {
            _snwprintf(out, cap - 1, L"%s\\Downloads\\%s", prof, CFG_APP_DIR);
            out[cap - 1] = 0;
        }
    }
    if (!out[0]) {
        DWORD n = GetTempPathW((DWORD)cap, out);
        if (n == 0 || n >= cap) out[0] = 0;
    }
}

wchar_t *suite_config_downloads_dir_w(wchar_t *out, size_t cap)
{
    wchar_t file[MAX_PATH];
    wchar_t val[1024];

    if (!out || cap == 0) return out;
    out[0] = 0;
    suite_config_file_w(file, MAX_PATH);

    val[0] = 0;
    if (file[0])
        GetPrivateProfileStringW(CFG_SECTION, CFG_KEY_DL, L"", val, 1024, file);
    /* trim spaces either side; an empty value means the default */
    {
        wchar_t *s = val, *e;
        while (*s == L' ' || *s == L'\t') s++;
        e = s + wcslen(s);
        while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) *--e = 0;
        if (s != val) memmove(val, s, (wcslen(s) + 1) * sizeof(wchar_t));
    }
    if (val[0]) { wcopy(out, cap, val); return out; }

    /* First use: the default, recorded so the file says what is in force. */
    default_downloads_dir(out, cap);
    if (file[0] && out[0] && ensure_file(file))
        WritePrivateProfileStringW(CFG_SECTION, CFG_KEY_DL, out, file);
    return out;
}

char *suite_config_downloads_dir_utf8(char *out, size_t cap)
{
    wchar_t w[1024];
    if (!out || cap == 0) return out;
    out[0] = 0;
    suite_config_downloads_dir_w(w, 1024);
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL))
        out[0] = 0;
    return out;
}

int suite_config_downloads_dir_ansi(char *out, size_t cap)
{
    wchar_t w[1024];
    BOOL lossy = FALSE;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    suite_config_downloads_dir_w(w, 1024);
    if (!WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w, -1, out, (int)cap,
                             NULL, &lossy) || lossy) {
        out[0] = 0;
        return 0;
    }
    return 1;
}

int suite_config_set_downloads_dir_w(const wchar_t *dir)
{
    wchar_t file[MAX_PATH];
    if (!dir || !dir[0]) return 0;
    suite_config_file_w(file, MAX_PATH);
    if (!file[0] || !ensure_file(file)) return 0;
    return WritePrivateProfileStringW(CFG_SECTION, CFG_KEY_DL, dir, file) ? 1 : 0;
}

/* ---- the modem handshake simulation (Navigator 1.7.2) ---------------- */

int suite_config_bbs_modem_sim(void)
{
    wchar_t file[MAX_PATH], val[16];
    suite_config_file_w(file, MAX_PATH);
    if (!file[0]) return 1;
    val[0] = 0;
    GetPrivateProfileStringW(CFG_SECTION, CFG_KEY_SIM, L"", val, 16, file);
    if (val[0] == L'0') return 0;
    if (val[0] == L'1') return 1;
    /* missing or unreadable: the default, ON, recorded like the Downloads
     * directory so the file says what is in force */
    if (ensure_file(file))
        WritePrivateProfileStringW(CFG_SECTION, CFG_KEY_SIM, L"1", file);
    return 1;
}

int suite_config_set_bbs_modem_sim(int on)
{
    wchar_t file[MAX_PATH];
    suite_config_file_w(file, MAX_PATH);
    if (!file[0] || !ensure_file(file)) return 0;
    return WritePrivateProfileStringW(CFG_SECTION, CFG_KEY_SIM, on ? L"1" : L"0", file) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* The dialog                                                          */
/* ------------------------------------------------------------------ */

#define IDC_CFG_DL_LABEL  1201
#define IDC_CFG_DL_EDIT   1202
#define IDC_CFG_BROWSE    1203
#define IDC_CFG_NOTE      1204

/* Dialog units. The Program Item Properties proportions: fields to the
 * left, a 46-unit button column on the right. */
#define CFG_DLG_W  284
#define CFG_DLG_H   70

static HWND g_cfg_owner = NULL;

static void *align_dword(unsigned char *p)
{
    ULONG_PTR a = (ULONG_PTR)p;
    a = (a + 3) & ~(ULONG_PTR)3;
    return (void *)a;
}

static unsigned char *emit_word(unsigned char *p, WORD v)
{
    memcpy(p, &v, sizeof v);
    return p + sizeof(WORD);
}

static unsigned char *emit_wstr(unsigned char *p, const wchar_t *s)
{
    size_t n = (s ? wcslen(s) : 0) + 1;
    if (s) memcpy(p, s, n * sizeof(wchar_t));
    else   { WORD z = 0; memcpy(p, &z, sizeof z); }
    return p + n * sizeof(wchar_t);
}

static unsigned char *emit_item(unsigned char *p, DWORD style, DWORD ex,
                                short x, short y, short cx, short cy,
                                WORD id, WORD atom, const wchar_t *title)
{
    DLGITEMTEMPLATE it;
    p = (unsigned char *)align_dword(p);
    it.style = style | WS_CHILD | WS_VISIBLE;
    it.dwExtendedStyle = ex;
    it.x = x; it.y = y; it.cx = cx; it.cy = cy;
    it.id = id;
    memcpy(p, &it, sizeof it);
    p += sizeof it;
    p = emit_word(p, 0xFFFF);
    p = emit_word(p, atom);
    p = emit_wstr(p, title);
    p = emit_word(p, 0);
    return p;
}

static DLGTEMPLATE *build_template(void)
{
    static unsigned char buf[4096];
    unsigned char *p = buf;
    DLGTEMPLATE dt;

    memset(buf, 0, sizeof buf);
    dt.style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt.dwExtendedStyle = 0;
    dt.cdit = 6;
    dt.x = 0; dt.y = 0; dt.cx = CFG_DLG_W; dt.cy = CFG_DLG_H;
    memcpy(p, &dt, sizeof dt);
    p += sizeof dt;
    p = emit_wstr(p, NULL);                 /* menu */
    p = emit_wstr(p, NULL);                 /* class */
    p = emit_wstr(p, L"Configuration");     /* caption */
    p = emit_word(p, 8);
    p = emit_wstr(p, L"MS Sans Serif");

    /* the hook draws dialog labels in the bold face, so the label is
     * measured for bold MS Sans Serif 8 (about 112 px = 75 units) */
    p = emit_item(p, SS_LEFT, 0, 6, 9, 84, 8,
                  IDC_CFG_DL_LABEL, 0x0082, L"&Downloads Directory:");
    p = emit_item(p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 0, 92, 7, 132, 12,
                  IDC_CFG_DL_EDIT, 0x0081, L"");
    p = emit_item(p, SS_LEFT, 0, 6, 26, 218, 34, IDC_CFG_NOTE, 0x0082,
                  L"Files the program saves without asking where, Zmodem "
                  L"downloads in the BBS and RIPscrip tabs, go to this "
                  L"directory. The FTP tab starts there too.");
    p = emit_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 232, 6, 46, 14,
                  IDOK, 0x0080, L"OK");
    p = emit_item(p, BS_PUSHBUTTON | WS_TABSTOP, 0, 232, 23, 46, 14,
                  IDCANCEL, 0x0080, L"Cancel");
    p = emit_item(p, BS_PUSHBUTTON | WS_TABSTOP, 0, 232, 46, 46, 14,
                  IDC_CFG_BROWSE, 0x0080, L"&Browse...");
    (void)p;
    return (DLGTEMPLATE *)buf;
}

/* The system folder picker, the hook suspended for it as for the Open and
 * Save dialogs. Returns 1 and fills out when a folder was chosen. */
static int browse_for_folder(HWND owner, const wchar_t *start, wchar_t *out, size_t cap)
{
    IFileOpenDialog *dlg = NULL;
    IShellItem *item = NULL, *init = NULL;
    PWSTR path = NULL;
    HRESULT hr;
    int ok = 0;
    DWORD opts = 0;

    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IFileOpenDialog, (void **)&dlg);
    if (FAILED(hr) || !dlg) return 0;

    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, L"Downloads Directory");
    if (start && start[0] && GetFileAttributesW(start) != INVALID_FILE_ATTRIBUTES &&
        SUCCEEDED(SHCreateItemFromParsingName(start, NULL, &IID_IShellItem, (void **)&init))) {
        IFileOpenDialog_SetFolder(dlg, init);
        IShellItem_Release(init);
    }

    w31_hook_suspend();
    hr = IFileOpenDialog_Show(dlg, owner);
    w31_hook_resume();

    if (SUCCEEDED(hr) && SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item)) && item) {
        if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path)) && path) {
            wcopy(out, cap, path);
            CoTaskMemFree(path);
            ok = 1;
        }
        IShellItem_Release(item);
    }
    IFileOpenDialog_Release(dlg);
    return ok;
}

/* The field's face. The hook gives dialog fields the period one, MS Sans
 * Serif 8 bold, whose bitmap cut on this system is Western (code page
 * 1252) and has no Greek or Cyrillic; a Downloads directory may well be
 * named in Greek. So the field keeps the period face while its text fits
 * code page 1252, and otherwise takes Microsoft Sans Serif (the TrueType
 * design of the same face) at the same size, drawn without antialiasing. */
static HFONT g_cfg_wide;
static int   g_cfg_is_wide;

static void cfg_pick_font(HWND dlg)
{
    HWND e = GetDlgItem(dlg, IDC_CFG_DL_EDIT);
    wchar_t t[1024];
    char a[2048];
    BOOL lossy = FALSE;
    int wide;
    if (!e) return;
    GetWindowTextW(e, t, 1024);
    WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, t, -1, a, sizeof a, NULL, &lossy);
    wide = lossy ? 1 : 0;
    if (wide == g_cfg_is_wide) return;
    g_cfg_is_wide = wide;
    if (wide) {
        if (!g_cfg_wide)
            g_cfg_wide = CreateFontW(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                     OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     NONANTIALIASED_QUALITY, VARIABLE_PITCH | FF_SWISS,
                                     L"Microsoft Sans Serif");
        SetPropA(e, W31_PROP_KEEPFONT, (HANDLE)1);
        SendMessageW(e, WM_SETFONT, (WPARAM)g_cfg_wide, TRUE);
    } else {
        RemovePropA(e, W31_PROP_KEEPFONT);
        SendMessageW(e, WM_SETFONT, (WPARAM)g_w31_font, TRUE);
    }
}

static INT_PTR CALLBACK cfg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        wchar_t dir[1024];
        RECT rc;
        int x = 0, y = 0;
        GetWindowRect(h, &rc);
        suite_center_popup(g_cfg_owner, rc.right - rc.left, rc.bottom - rc.top, &x, &y);
        SetWindowPos(h, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        suite_config_downloads_dir_w(dir, 1024);
        SendDlgItemMessageW(h, IDC_CFG_DL_EDIT, EM_LIMITTEXT, 1000, 0);
        g_cfg_is_wide = 0;
        SetDlgItemTextW(h, IDC_CFG_DL_EDIT, dir);
        cfg_pick_font(h);
        SetFocus(GetDlgItem(h, IDC_CFG_DL_EDIT));
        SendDlgItemMessageW(h, IDC_CFG_DL_EDIT, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_CFG_DL_EDIT:
            if (HIWORD(w) == EN_CHANGE) cfg_pick_font(h);
            return FALSE;
        case IDC_CFG_BROWSE: {
            wchar_t cur[1024], pick[1024];
            GetDlgItemTextW(h, IDC_CFG_DL_EDIT, cur, 1024);
            if (browse_for_folder(h, cur, pick, 1024)) {
                SetDlgItemTextW(h, IDC_CFG_DL_EDIT, pick);
                SetFocus(GetDlgItem(h, IDC_CFG_DL_EDIT));
            }
            return TRUE;
        }
        case IDOK: {
            wchar_t typed[1024], full[1024], *s, *e;
            DWORD n;
            GetDlgItemTextW(h, IDC_CFG_DL_EDIT, typed, 1024);
            s = typed;
            while (*s == L' ' || *s == L'\t') s++;
            e = s + wcslen(s);
            while (e > s && (e[-1] == L' ' || e[-1] == L'\t')) *--e = 0;
            /* Windows 3.1 messages state the fault plainly. */
            if (!s[0]) {
                MessageBoxW(h, L"Type the name of a directory.", L"Configuration",
                            MB_OK | MB_ICONEXCLAMATION);
                SetFocus(GetDlgItem(h, IDC_CFG_DL_EDIT));
                return TRUE;
            }
            n = GetFullPathNameW(s, 1024, full, NULL);
            if (n == 0 || n >= 1024 || !suite_config_ensure_dir_w(full) ||
                !(GetFileAttributesW(full) & FILE_ATTRIBUTE_DIRECTORY)) {
                MessageBoxW(h, L"Cannot create or use this directory. "
                               L"Check the path and try again.",
                            L"Configuration", MB_OK | MB_ICONEXCLAMATION);
                SetFocus(GetDlgItem(h, IDC_CFG_DL_EDIT));
                SendDlgItemMessageW(h, IDC_CFG_DL_EDIT, EM_SETSEL, 0, -1);
                return TRUE;
            }
            if (!suite_config_set_downloads_dir_w(full)) {
                MessageBoxW(h, L"Cannot write the configuration file.",
                            L"Configuration", MB_OK | MB_ICONEXCLAMATION);
                return TRUE;
            }
            EndDialog(h, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, 0);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

void suite_config_dialog(HWND owner)
{
    INT_PTR rc;
    g_cfg_owner = owner;
    rc = DialogBoxIndirectParamW(GetModuleHandleW(NULL), build_template(),
                                 owner, cfg_proc, 0);
    g_cfg_owner = NULL;
    if (g_cfg_wide) { DeleteObject(g_cfg_wide); g_cfg_wide = NULL; }
    if (rc == 1) {
        wchar_t dir[1024];
        char line[1100];
        char a[1024];
        BOOL lossy = FALSE;
        suite_config_downloads_dir_w(dir, 1024);
        /* The status line is ANSI; a path it cannot spell is still saved. */
        if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, dir, -1, a, sizeof a, NULL, &lossy) && !lossy)
            _snprintf(line, sizeof line - 1, "Downloads directory: %s", a);
        else
            _snprintf(line, sizeof line - 1, "Downloads directory saved.");
        line[sizeof line - 1] = 0;
        suite_set_status(line);
    }
}

/* ------------------------------------------------------------------ */
/* Settings > BBS... (Navigator 1.7.2)                                 */
/* ------------------------------------------------------------------ */

/* One check box, "Modem handshake simulation", in the same Windows 3.1
 * grammar as Configuration: the setting and a note on the left, OK and
 * Cancel stacked on the right. The check box is drawn here the Windows
 * 3.1 way (w31_draw_checkbox: white box, black frame, an X, bold label),
 * because the system's own check box is the Windows 11 one. */

#define IDC_BBS_SIM   1211
#define IDC_BBS_NOTE  1212

#define BBS_DLG_W  236
#define BBS_DLG_H   62

static int g_bbs_sim_state;

static DLGTEMPLATE *build_bbs_template(void)
{
    static unsigned char buf[2048];
    unsigned char *p = buf;
    DLGTEMPLATE dt;

    memset(buf, 0, sizeof buf);
    dt.style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt.dwExtendedStyle = 0;
    dt.cdit = 4;
    dt.x = 0; dt.y = 0; dt.cx = BBS_DLG_W; dt.cy = BBS_DLG_H;
    memcpy(p, &dt, sizeof dt);
    p += sizeof dt;
    p = emit_wstr(p, NULL);                 /* menu */
    p = emit_wstr(p, NULL);                 /* class */
    p = emit_wstr(p, L"BBS");               /* caption: the menu item, no ellipsis */
    p = emit_word(p, 8);
    p = emit_wstr(p, L"MS Sans Serif");

    /* the bold label measures about 170 px = 114 units; the box 13 + 6 */
    p = emit_item(p, BS_OWNERDRAW | WS_TABSTOP | WS_GROUP, 0, 6, 7, 170, 11,
                  IDC_BBS_SIM, 0x0080, L"&Modem handshake simulation");
    p = emit_item(p, SS_LEFT, 0, 6, 24, 170, 34, IDC_BBS_NOTE, 0x0082,
                  L"When checked, Connect first shows the call being dialed "
                  L"and plays the modem's handshake, then it connects.");
    p = emit_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 184, 6, 46, 14,
                  IDOK, 0x0080, L"OK");
    p = emit_item(p, BS_PUSHBUTTON | WS_TABSTOP, 0, 184, 23, 46, 14,
                  IDCANCEL, 0x0080, L"Cancel");
    (void)p;
    return (DLGTEMPLATE *)buf;
}

static INT_PTR CALLBACK bbs_cfg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG: {
        RECT rc;
        int x = 0, y = 0;
        GetWindowRect(h, &rc);
        suite_center_popup(g_cfg_owner, rc.right - rc.left, rc.bottom - rc.top, &x, &y);
        SetWindowPos(h, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        g_bbs_sim_state = suite_config_bbs_modem_sim();
        SetFocus(GetDlgItem(h, IDC_BBS_SIM));
        return FALSE;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)l;
        char label[128];
        if (!di || di->CtlID != IDC_BBS_SIM) return FALSE;
        GetWindowTextA(di->hwndItem, label, sizeof label);
        w31_draw_checkbox(di->hDC, &di->rcItem, label, g_bbs_sim_state ? TRUE : FALSE,
                          (di->itemState & ODS_FOCUS) != 0,
                          (di->itemState & ODS_DISABLED) != 0, NULL);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_BBS_SIM:
            if (HIWORD(w) == BN_CLICKED || HIWORD(w) == BN_DOUBLECLICKED) {
                g_bbs_sim_state = !g_bbs_sim_state;
                InvalidateRect(GetDlgItem(h, IDC_BBS_SIM), NULL, FALSE);
            }
            return TRUE;
        case IDOK:
            if (!suite_config_set_bbs_modem_sim(g_bbs_sim_state)) {
                MessageBoxW(h, L"Cannot write the configuration file.",
                            L"BBS", MB_OK | MB_ICONEXCLAMATION);
                return TRUE;
            }
            EndDialog(h, 1);
            return TRUE;
        case IDCANCEL:
            EndDialog(h, 0);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

void suite_config_bbs_dialog(HWND owner)
{
    INT_PTR rc;
    g_cfg_owner = owner;
    rc = DialogBoxIndirectParamW(GetModuleHandleW(NULL), build_bbs_template(),
                                 owner, bbs_cfg_proc, 0);
    g_cfg_owner = NULL;
    if (rc == 1)
        suite_set_status(suite_config_bbs_modem_sim()
                         ? "BBS: modem handshake simulation on."
                         : "BBS: modem handshake simulation off.");
}
