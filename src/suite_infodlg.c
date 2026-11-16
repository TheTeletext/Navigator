/*
 * suite_infodlg.c - Windows 3.1 information dialogs (Navigator 1.7.1).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See suite_infodlg.h. The template carries only the caption and the
 * buttons; everything else is laid out in WM_INITDIALOG with the live
 * font, because whether the white box needs a scroll bar depends on how
 * many lines the text actually takes in the bold System face. The box is
 * a read-only multiline EDIT (the hook paints edits white and gives a
 * WS_VSCROLL the Windows 3.1 scroll bar), created WITH WS_VSCROLL only
 * when the text is longer than max_lines; a short message gets a box just
 * tall enough and no scroll bar at all. Every text control is marked
 * W31_PROP_KEEPFONT so the hook leaves the System face in place.
 */

#include "suite_infodlg.h"
#include "suite_shell.h"
#include "suite_w31.h"

#include <stdlib.h>
#include <string.h>

#define IDC_INFO_ICON   3101
#define IDC_INFO_TITLE  3102
#define IDC_INFO_VER    3103
#define IDC_INFO_RULE   3104
#define IDC_INFO_SUB    3105
#define IDC_INFO_BODY   3106

#define WM_APP_INFO_DESELECT  (WM_APP + 171)   /* posted to the info dialog itself */

#define MARGIN   14
#define GAP      10
#define BTN_W    88
#define BTN_H    26

static void *align4(unsigned char *p) { ULONG_PTR a = (ULONG_PTR)p; return (void *)((a + 3) & ~(ULONG_PTR)3); }
static unsigned char *w16(unsigned char *p, WORD v) { memcpy(p, &v, 2); return p + 2; }
static unsigned char *wstr(unsigned char *p, const wchar_t *s)
{
    size_t n = (s ? wcslen(s) : 0) + 1;
    if (s) memcpy(p, s, n * 2); else { WORD z = 0; memcpy(p, &z, 2); }
    return p + n * 2;
}
static unsigned char *item(unsigned char *p, DWORD style, short x, short y, short cx, short cy,
                           WORD id, const wchar_t *text)
{
    DLGITEMTEMPLATE it;
    p = (unsigned char *)align4(p);
    it.style = style | WS_CHILD | WS_VISIBLE; it.dwExtendedStyle = 0;
    it.x = x; it.y = y; it.cx = cx; it.cy = cy; it.id = id;
    memcpy(p, &it, sizeof it); p += sizeof it;
    p = w16(p, 0xFFFF); p = w16(p, 0x0080);          /* button */
    p = wstr(p, text);
    return w16(p, 0);
}

static DLGTEMPLATE *build_template(const w31_info_t *in, wchar_t *caption_w)
{
    static unsigned char buf[2048];
    unsigned char *p = buf;
    DLGTEMPLATE dt;
    wchar_t okw[64];
    memset(buf, 0, sizeof buf);
    dt.style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt.dwExtendedStyle = 0;
    dt.cdit = (WORD)(in->buttons == W31INFO_OKCANCEL ? 2 : in->buttons == W31INFO_NONE ? 0 : 1);
    dt.x = dt.y = 0; dt.cx = 200; dt.cy = 100;   /* resized in WM_INITDIALOG */
    memcpy(p, &dt, sizeof dt); p += sizeof dt;
    p = wstr(p, NULL); p = wstr(p, NULL); p = wstr(p, caption_w);
    p = w16(p, 8); p = wstr(p, L"MS Sans Serif");
    MultiByteToWideChar(CP_ACP, 0, in->ok_label ? in->ok_label : "OK", -1, okw, 64);
    if (in->buttons != W31INFO_NONE)
        p = item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 0, 40, 14, IDOK, okw);
    if (in->buttons == W31INFO_OKCANCEL)
        p = item(p, BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 40, 14, IDCANCEL, L"Cancel");
    (void)p;
    return (DLGTEMPLATE *)buf;
}

/* LF -> CRLF, as the edit control wants. */
static char *to_crlf(const char *s)
{
    size_t n = strlen(s), i, o = 0;
    char *r = (char *)malloc(n * 2 + 1);
    if (!r) return NULL;
    for (i = 0; i < n; i++) {
        if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) r[o++] = '\r';
        r[o++] = s[i];
    }
    r[o] = 0;
    return r;
}

static int text_height(HDC dc, const char *text, int width)
{
    RECT r = { 0, 0, width, 0 };
    DrawTextA(dc, text, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | DT_EDITCONTROL);
    return r.bottom;
}

static HWND make_child(HWND dlg, const char *cls, const char *text, DWORD style, DWORD ex,
                       int x, int y, int w, int h, int id, HFONT f)
{
    HWND c = CreateWindowExA(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                             dlg, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    if (c && f) {
        SetPropA(c, W31_PROP_KEEPFONT, (HANDLE)1);
        SendMessageA(c, WM_SETFONT, (WPARAM)f, FALSE);
    }
    return c;
}

static void layout(HWND dlg, const w31_info_t *in)
{
    HFONT f = w31_system_font();
    HDC dc = GetDC(dlg);
    HGDIOBJ of = SelectObject(dc, f);
    TEXTMETRICA tm;
    int W = in->width > 0 ? in->width : 440;
    int y = MARGIN, lineH, maxLines = in->max_lines > 0 ? in->max_lines : 12;
    RECT wr;
    int bx = MARGIN, bw = W - 2 * MARGIN;

    GetTextMetricsA(dc, &tm);
    lineH = tm.tmHeight;

    /* the heading: icon, name, version, a rule, centred lines; bold on gray */
    if (in->title) {
        HICON ic = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
        HWND h = make_child(dlg, "STATIC", "", SS_ICON, 0, MARGIN, y, 32, 32, IDC_INFO_ICON, NULL);
        if (h && ic) SendMessageA(h, STM_SETICON, (WPARAM)ic, 0);
        make_child(dlg, "STATIC", in->title, SS_LEFT | SS_NOPREFIX, 0,
                   MARGIN + 48, y + (32 - lineH) / 2, W / 2, lineH, IDC_INFO_TITLE, f);
        if (in->version)
            make_child(dlg, "STATIC", in->version, SS_RIGHT | SS_NOPREFIX, 0,
                       W / 2, y + (32 - lineH) / 2, W / 2 - MARGIN, lineH, IDC_INFO_VER, f);
        y += 32 + GAP;
        make_child(dlg, "STATIC", "", SS_ETCHEDHORZ, 0, MARGIN, y, W - 2 * MARGIN, 2, IDC_INFO_RULE, NULL);
        y += 2 + GAP;
    }
    if (in->subtitle) {
        int h = text_height(dc, in->subtitle, W - 2 * MARGIN);
        make_child(dlg, "STATIC", in->subtitle, SS_CENTER | SS_NOPREFIX, 0,
                   MARGIN, y, W - 2 * MARGIN, h, IDC_INFO_SUB, f);
        y += h + GAP;
    }

    /* the white box: tall enough for the text, up to maxLines; past that
     * it scrolls, the scroll bar inside the box */
    if (in->body) {
        char *crlf = to_crlf(in->body);
        const char *text = crlf ? crlf : in->body;
        int pass, scroll = 0, ch = 0;
        HWND e = NULL;
        /* The EDIT itself is asked how many lines the text wraps to and
         * how far apart its lines are: its client area is narrower than
         * its window (the client edge and the hook's sunken frame), and
         * its line pitch is its own. Born without a scroll bar; born again
         * WITH one only if the text is longer than max_lines. */
        for (pass = 0; pass < 2; pass++) {
            RECT cr, wrc, fr;
            int extra, count, pitch = lineH;
            e = make_child(dlg, "EDIT", "", ES_MULTILINE | ES_READONLY | (scroll ? WS_VSCROLL : 0),
                           WS_EX_CLIENTEDGE, bx, y, bw, (maxLines + 2) * lineH + 40, IDC_INFO_BODY, f);
            if (!e) break;
            /* the hook's sunken frame is a non-client change: apply it now,
             * so that the client area measured below is the final one */
            SetWindowPos(e, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                                              SWP_NOACTIVATE | SWP_FRAMECHANGED);
            SendMessageA(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(5, 5));
            SetWindowTextA(e, text);
            GetClientRect(e, &cr);
            GetWindowRect(e, &wrc);
            extra = (wrc.bottom - wrc.top) - cr.bottom;
            fr = cr; fr.left += 5; fr.right -= 5; fr.top += 3; fr.bottom -= 1;
            SendMessageA(e, EM_SETRECT, 0, (LPARAM)&fr);
            count = (int)SendMessageA(e, EM_GETLINECOUNT, 0, 0);
            if (count > 1) {
                LRESULT p0 = SendMessageA(e, EM_POSFROMCHAR, 0, 0);
                LRESULT p1 = SendMessageA(e, EM_POSFROMCHAR,
                                          (WPARAM)SendMessageA(e, EM_LINEINDEX, 1, 0), 0);
                int d = (short)HIWORD(p1) - (short)HIWORD(p0);
                if (d > 0 && d < 64) pitch = d;
            }
            if (count < 1) count = 1;
            if (count > maxLines && !scroll) {
                DestroyWindow(e);
                e = NULL;
                scroll = 1;
                continue;
            }
            if (count > maxLines) count = maxLines;
            /* the first line sits a pixel below the formatting rectangle's
             * top, and the edit draws only lines that fit it whole: 3 px
             * above, 1 below, 2 to spare (the formatting rectangle ends 1 px
             * short of the client bottom; the 1.7.1 dialog gate checks it) */
            ch = count * pitch + 6 + extra;
            SetWindowPos(e, NULL, bx, y, bw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
            GetClientRect(e, &fr);
            if (fr.bottom < count * pitch + 6) {         /* the frame grew after all */
                ch += count * pitch + 6 - fr.bottom;
                SetWindowPos(e, NULL, bx, y, bw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
                GetClientRect(e, &fr);
            }
            fr.left += 5; fr.right -= 5; fr.top += 3; fr.bottom -= 1;
            SendMessageA(e, EM_SETRECT, 0, (LPARAM)&fr);
            break;
        }
        free(crlf);
        y += ch + GAP + 2;
    }

    /* the buttons, centred along the bottom */
    if (GetDlgItem(dlg, IDOK)) {
        HWND ok = GetDlgItem(dlg, IDOK), ca = GetDlgItem(dlg, IDCANCEL);
        int n = ca ? 2 : 1, total = n * BTN_W + (n - 1) * 16, x = (W - total) / 2;
        MoveWindow(ok, x, y, BTN_W, BTN_H, FALSE);
        if (ca) MoveWindow(ca, x + BTN_W + 16, y, BTN_W, BTN_H, FALSE);
        y += BTN_H + MARGIN;
    } else {
        y += MARGIN - GAP - 2;
    }
    SelectObject(dc, of);
    ReleaseDC(dlg, dc);

    /* the window around that client area, centred on its owner (or on the
     * monitor under the pointer when there is none, the update helper) */
    wr.left = 0; wr.top = 0; wr.right = W; wr.bottom = y;
    AdjustWindowRectEx(&wr, (DWORD)GetWindowLongA(dlg, GWL_STYLE), FALSE, (DWORD)GetWindowLongA(dlg, GWL_EXSTYLE));
    {
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top, x = 0, yy = 0;
        HWND owner = GetWindow(dlg, GW_OWNER);
        if (in->has_anchor || !owner) {
            POINT pt; MONITORINFO mi;
            if (in->has_anchor) pt = in->anchor; else GetCursorPos(&pt);
            mi.cbSize = sizeof mi;
            GetMonitorInfoA(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
            x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
            yy = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
        } else {
            suite_center_popup(owner, ww, wh, &x, &yy);
        }
        SetWindowPos(dlg, NULL, x, yy, ww, wh, SWP_NOZORDER);
    }
}

static INT_PTR CALLBACK info_proc(HWND dlg, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_INITDIALOG:
        layout(dlg, (const w31_info_t *)l);
        if (GetDlgItem(dlg, IDOK)) { SetFocus(GetDlgItem(dlg, IDOK)); return FALSE; }
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) { EndDialog(dlg, LOWORD(w)); return TRUE; }
        /* The box is read-only text, not a field: when it takes the focus
         * (the "please wait" box has no other control, so the dialog
         * manager gives it the focus, and an edit given the focus that way
         * selects all its text) nothing is selected, and in the box with
         * no buttons no caret blinks either. */
        if (LOWORD(w) == IDC_INFO_BODY && HIWORD(w) == EN_SETFOCUS) {
            /* posted: the dialog manager selects everything AFTER this
             * notification (DLGC_HASSETSEL), so deselect once it has */
            PostMessageA(dlg, WM_APP_INFO_DESELECT, 0, l);
            return TRUE;
        }
        break;
    case WM_APP_INFO_DESELECT: {
        HWND e = (HWND)l;
        SendMessageA(e, EM_SETSEL, 0, 0);
        if (!GetDlgItem(dlg, IDOK)) HideCaret(e);
        return TRUE;
    }
    case WM_CLOSE:
        if (GetDlgItem(dlg, IDOK)) EndDialog(dlg, IDCANCEL);
        return TRUE;              /* the "please wait" box cannot be closed */
    }
    return FALSE;
}

int w31_info_dialog(HWND owner, const w31_info_t *info)
{
    wchar_t cap[256];
    INT_PTR r;
    if (!info) return IDCANCEL;
    MultiByteToWideChar(CP_ACP, 0, info->caption ? info->caption : "", -1, cap, 256);
    r = DialogBoxIndirectParamA(GetModuleHandleA(NULL), build_template(info, cap), owner,
                                info_proc, (LPARAM)info);
    return r == IDOK ? IDOK : IDCANCEL;
}

HWND w31_info_modeless(const w31_info_t *info)
{
    wchar_t cap[256];
    HWND h;
    if (!info) return NULL;
    MultiByteToWideChar(CP_ACP, 0, info->caption ? info->caption : "", -1, cap, 256);
    h = CreateDialogIndirectParamA(GetModuleHandleA(NULL), build_template(info, cap), NULL,
                                   info_proc, (LPARAM)info);
    if (h) { ShowWindow(h, SW_SHOWNORMAL); UpdateWindow(h); }
    return h;
}
