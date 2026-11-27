/*
 * gopher_plus_dlg.c - The Gopher+ "Choose a View" and form dialogs.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Both dialogs come from in-memory templates, as gopher_search_dialog.c
 * builds them, so the Windows 3.1 hook (suite_w31.c) gives them the period
 * frame, buttons, owner-drawn list and scroll bars like every other dialog.
 *
 * Choose a View: the memo's "scrolling list dialog" (s2.6: "The client
 * retrieves the Attribute Information, displays the list of views to the
 * user in some kind of scrolling list dialog. User selects a line and
 * client now requests the document").
 *
 * The form: one row per +ASK line, in the server's order (memo s2.8: "The
 * client will use all lines in the order they appear"). Ask = a field with
 * its default; AskP = a field showing bullets; AskL = a field of several
 * lines; Choose = option buttons (a drop-down list past six choices);
 * Select = one check box per line (the UMN client's reading); Note = text.
 * The rows live in a child pane that scrolls when the form is taller than
 * the screen allows. The answers are checked before anything is sent:
 * Latin-1 only (P6), and no answer may be a lone "." (P3).
 * Report: docs/2026-11-22_GOPHER_PLUS_PHASE2.md.
 */

#include "gopher_plus_dlg.h"
#include "suite_shell.h"
#include "suite_w31.h"
#include "suite_infodlg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- text ---------- */

wchar_t *gplus_to_wide(const char *s)
{
    int      n;
    wchar_t *w;
    UINT     cp = CP_UTF8;
    if (!s) s = "";
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) { cp = 28591; n = MultiByteToWideChar(cp, 0, s, -1, NULL, 0); }
    if (n <= 0) n = 1;
    w = (wchar_t *)calloc((size_t)n + 1, sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, s, -1, w, n);
    return w;
}

/* The ANSI text the info box draws. */
static void to_ansi(const char *utf8, char *out, size_t n)
{
    wchar_t *w = gplus_to_wide(utf8);
    out[0] = 0;
    if (w) {
        WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)n - 1, "?", NULL);
        free(w);
    }
    out[n - 1] = 0;
}

static void message(HWND owner, const char *caption, const char *utf8_text)
{
    static char body[2048];
    w31_info_t  info;
    to_ansi(utf8_text, body, sizeof body);
    memset(&info, 0, sizeof info);
    info.caption = caption;
    info.body    = body;
    info.buttons = W31INFO_OK;
    info.width   = 420;
    w31_info_dialog(owner, &info);
}

/* ---------- template helpers (the gopher_search_dialog.c layout) ---------- */

static unsigned char *align4(unsigned char *p)
{
    ULONG_PTR a = (ULONG_PTR)p;
    return (unsigned char *)((a + 3) & ~(ULONG_PTR)3);
}
static unsigned char *put_word(unsigned char *p, WORD v) { memcpy(p, &v, 2); return p + 2; }
static unsigned char *put_wstr(unsigned char *p, const wchar_t *s)
{
    size_t n = s ? wcslen(s) + 1 : 1;
    if (s) memcpy(p, s, n * sizeof(wchar_t)); else { WORD z = 0; memcpy(p, &z, 2); }
    return p + n * sizeof(wchar_t);
}
static unsigned char *put_item(unsigned char *p, DWORD style, DWORD ex,
                               short x, short y, short cx, short cy,
                               WORD id, WORD atom, const wchar_t *text)
{
    DLGITEMTEMPLATE it;
    p = align4(p);
    it.style = style | WS_CHILD | WS_VISIBLE;
    it.dwExtendedStyle = ex;
    it.x = x; it.y = y; it.cx = cx; it.cy = cy; it.id = id;
    memcpy(p, &it, sizeof it);
    p += sizeof it;
    p = put_word(p, 0xFFFF);
    p = put_word(p, atom);
    p = put_wstr(p, text);
    return put_word(p, 0);
}
static unsigned char *put_header(unsigned char *p, const wchar_t *caption,
                                 WORD items, short cx, short cy)
{
    DLGTEMPLATE dt;
    dt.style = DS_MODALFRAME | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    dt.dwExtendedStyle = 0;
    dt.cdit = items;
    dt.x = 0; dt.y = 0; dt.cx = cx; dt.cy = cy;
    memcpy(p, &dt, sizeof dt);
    p += sizeof dt;
    p = put_wstr(p, NULL);                 /* menu  */
    p = put_wstr(p, NULL);                 /* class */
    p = put_wstr(p, caption);
    p = put_word(p, 8);
    return put_wstr(p, L"MS Sans Serif");
}

static HWND g_owner = NULL;

static void centre(HWND h)
{
    RECT rc;
    int  x = 0, y = 0;
    GetWindowRect(h, &rc);
    suite_center_popup(g_owner, rc.right - rc.left, rc.bottom - rc.top, &x, &y);
    SetWindowPos(h, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---------- Choose a View ---------- */

#define IDC_VW_LABEL  1301
#define IDC_VW_LIST   1302
#define IDC_VW_SAVE   1303
#define IDC_VW_NOTE   1304

static const GplusAttrs *g_vw_attrs;
static const char       *g_vw_name;
static int               g_vw_sel;

static INT_PTR CALLBACK view_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        int i;
        HWND lb = GetDlgItem(h, IDC_VW_LIST);
        wchar_t *nm = gplus_to_wide(g_vw_name);
        if (nm) { SetDlgItemTextW(h, IDC_VW_LABEL, nm); free(nm); }
        for (i = 0; i < g_vw_attrs->nviews; i++) {
            char lab[300];
            wchar_t *wl;
            gplus_view_label(&g_vw_attrs->views[i], ", ", lab, sizeof lab);
            wl = gplus_to_wide(lab);
            SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)(wl ? wl : L"?"));
            free(wl);
        }
        SendMessageW(lb, LB_SETCURSEL, 0, 0);
        centre(h);
        SetFocus(lb);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_VW_LIST:
            if (HIWORD(w) != LBN_DBLCLK) return FALSE;
            /* fall through: a double-click opens */
        case IDOK:
        case IDC_VW_SAVE: {
            int s = (int)SendDlgItemMessageW(h, IDC_VW_LIST, LB_GETCURSEL, 0, 0);
            if (s < 0) return TRUE;
            g_vw_sel = s;
            EndDialog(h, LOWORD(w) == IDC_VW_SAVE ? GPDLG_SAVE : GPDLG_OPEN);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(h, GPDLG_CANCEL);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        EndDialog(h, GPDLG_CANCEL);
        return TRUE;
    }
    return FALSE;
}

int gplus_view_dialog(HWND owner, const char *item_name,
                      const GplusAttrs *a, int *out_view)
{
    static unsigned char buf[4096];
    unsigned char *p = buf;
    INT_PTR rc;
    if (!a || a->nviews <= 0 || !out_view) return GPDLG_CANCEL;
    memset(buf, 0, sizeof buf);
    /* 250 x 132 units: name, list, the note, buttons to the right as the
     * Windows 3.1 File Open box has them */
    p = put_header(p, L"Choose a View", 6, 250, 132);
    p = put_item(p, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 7, 6, 180, 9, IDC_VW_LABEL, 0x0082, L"");
    p = put_item(p, LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
                 0, 7, 18, 180, 84, IDC_VW_LIST, 0x0083, NULL);
    p = put_item(p, SS_LEFT | SS_NOPREFIX, 0, 7, 106, 236, 20, IDC_VW_NOTE, 0x0082,
                 L"The first view is the one the server prefers. "
                 L"The server may send its preferred view instead.");
    p = put_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 194, 18, 50, 14, IDOK, 0x0080, L"Open");
    p = put_item(p, BS_PUSHBUTTON | WS_TABSTOP, 0, 194, 36, 50, 14, IDC_VW_SAVE, 0x0080, L"Save As...");
    p = put_item(p, BS_PUSHBUTTON | WS_TABSTOP, 0, 194, 54, 50, 14, IDCANCEL, 0x0080, L"Cancel");
    (void)p;
    g_owner = owner; g_vw_attrs = a; g_vw_name = item_name; g_vw_sel = 0;
    rc = DialogBoxIndirectParamW((HINSTANCE)GetModuleHandleW(NULL), (DLGTEMPLATE *)buf,
                                 owner, view_proc, 0);
    g_vw_attrs = NULL;
    if (rc == GPDLG_OPEN || rc == GPDLG_SAVE) { *out_view = g_vw_sel; return (int)rc; }
    return GPDLG_CANCEL;
}

/* ---------- the form ---------- */

#define IDC_FM_PANE    1400
#define IDC_FM_BASE    1500          /* controls: IDC_FM_BASE + 16 * field + k */
#define FM_PANE_CLASS  L"MGTGopherFormPaneV1"
#define FM_MAX_FIELDS  200
#define FM_MAX_CHOICE  64

typedef struct {
    HWND label, edit, combo;
    HWND radio[FM_MAX_CHOICE];
    int  nradio;
    HWND check;
    int  chosen;                     /* Choose: the option chosen     */
    int  checked;                    /* Select: the box's state       */
} FmRow;

static const GplusAskField *g_fm_f;
static int                  g_fm_n;
static FmRow               *g_fm_rows;
static char               **g_fm_answers;
static const char          *g_fm_title;
static HWND                 g_fm_pane;
static int                  g_fm_content_h, g_fm_scroll;
static HFONT                g_fm_font;

static void fm_set_scroll(void)
{
    SCROLLINFO si;
    RECT rc;
    GetClientRect(g_fm_pane, &rc);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = g_fm_content_h > 0 ? g_fm_content_h - 1 : 0;
    si.nPage  = (UINT)(rc.bottom - rc.top);
    si.nPos   = g_fm_scroll;
    SetScrollInfo(g_fm_pane, SB_VERT, &si, TRUE);
}

static void fm_scroll_to(int pos)
{
    RECT rc;
    int  page, maxp, dy;
    GetClientRect(g_fm_pane, &rc);
    page = rc.bottom - rc.top;
    maxp = g_fm_content_h - page;
    if (maxp < 0) maxp = 0;
    if (pos < 0) pos = 0;
    if (pos > maxp) pos = maxp;
    dy = g_fm_scroll - pos;
    if (!dy) return;
    g_fm_scroll = pos;
    ScrollWindowEx(g_fm_pane, 0, dy, NULL, NULL, NULL, NULL,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    fm_set_scroll();
    UpdateWindow(g_fm_pane);
}

/* Keep the focused control in view (Tab through a long form). */
static void fm_reveal(HWND c)
{
    RECT rc, pc;
    if (!c || !g_fm_pane || GetParent(c) != g_fm_pane) return;
    GetWindowRect(c, &rc);
    MapWindowPoints(NULL, g_fm_pane, (POINT *)&rc, 2);
    GetClientRect(g_fm_pane, &pc);
    if (rc.top < 4) fm_scroll_to(g_fm_scroll + rc.top - 4);
    else if (rc.bottom > pc.bottom - 4) fm_scroll_to(g_fm_scroll + rc.bottom - pc.bottom + 4);
}

static int fm_find(HWND c, int *row, int *k)
{
    int i, j;
    for (i = 0; i < g_fm_n; i++) {
        FmRow *r = &g_fm_rows[i];
        if (r->check == c) { *row = i; *k = -1; return 1; }
        for (j = 0; j < r->nradio; j++)
            if (r->radio[j] == c) { *row = i; *k = j; return 1; }
    }
    return 0;
}

static LRESULT CALLBACK fm_pane_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)l;
        int row, k;
        char lab[512];
        wchar_t wl[512];
        if (!di || !fm_find(di->hwndItem, &row, &k)) break;
        GetWindowTextW(di->hwndItem, wl, 511);
        wl[511] = 0;
        WideCharToMultiByte(CP_ACP, 0, wl, -1, lab, sizeof lab - 1, "?", NULL);
        lab[sizeof lab - 1] = 0;
        /* NULL: the bold dialog face, as w31_draw_checkbox measured it on
         * the Windows 3.1 reference (refshots/connect.png) */
        if (k < 0)
            w31_draw_checkbox(di->hDC, &di->rcItem, lab, g_fm_rows[row].checked,
                              (di->itemState & ODS_FOCUS) != 0,
                              (di->itemState & ODS_DISABLED) != 0, NULL);
        else
            w31_draw_radio(di->hDC, &di->rcItem, lab, g_fm_rows[row].chosen == k,
                           (di->itemState & ODS_FOCUS) != 0,
                           (di->itemState & ODS_DISABLED) != 0, NULL);
        return TRUE;
    }
    case WM_COMMAND: {
        int row, k;
        HWND c = (HWND)l;
        if (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == CBN_SETFOCUS || HIWORD(w) == BN_SETFOCUS)
            fm_reveal(c);
        if ((HIWORD(w) == BN_CLICKED || HIWORD(w) == BN_DOUBLECLICKED) && fm_find(c, &row, &k)) {
            FmRow *r = &g_fm_rows[row];
            int j;
            if (k < 0) r->checked = !r->checked;
            else r->chosen = k;
            InvalidateRect(c, NULL, FALSE);
            for (j = 0; j < r->nradio; j++) InvalidateRect(r->radio[j], NULL, FALSE);
            return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
        return (LRESULT)w31_ctlcolor(m, (HDC)w, (HWND)l);
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, (HBRUSH)w31_ctlcolor(WM_CTLCOLORDLG, (HDC)w, NULL));
        return 1;
    }
    case WM_VSCROLL: {
        SCROLLINFO si;
        RECT rc;
        int pos = g_fm_scroll, page;
        GetClientRect(h, &rc);
        page = rc.bottom - rc.top;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_TRACKPOS;
        GetScrollInfo(h, SB_VERT, &si);
        switch (LOWORD(w)) {
        case SB_LINEUP:   pos -= 16; break;
        case SB_LINEDOWN: pos += 16; break;
        case SB_PAGEUP:   pos -= page; break;
        case SB_PAGEDOWN: pos += page; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = g_fm_content_h; break;
        }
        fm_scroll_to(pos);
        return 0;
    }
    case WM_MOUSEWHEEL:
        fm_scroll_to(g_fm_scroll - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 48);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND fm_child(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex,
                     int x, int y, int cx, int cy, int id)
{
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                             x, y, cx, cy, g_fm_pane, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetModuleHandleW(NULL), NULL);
    if (c && g_fm_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_fm_font, FALSE);
    return c;
}

/* Lines a label of width cx will take, for the layout. */
static int fm_text_h(const wchar_t *t, int cx)
{
    HDC   dc = GetDC(g_fm_pane);
    HGDIOBJ of = SelectObject(dc, g_fm_font);
    RECT  r;
    SetRect(&r, 0, 0, cx, 0);
    DrawTextW(dc, t, -1, &r, DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, of);
    ReleaseDC(g_fm_pane, dc);
    return r.bottom - r.top;
}

/* Build the rows; returns the content height in pixels. */
static int fm_build(int cx)
{
    int i, y = 6, j;
    for (i = 0; i < g_fm_n; i++) {
        const GplusAskField *f = &g_fm_f[i];
        FmRow   *r  = &g_fm_rows[i];
        wchar_t *pw = gplus_to_wide(f->prompt);
        int      id = IDC_FM_BASE + 16 * i, th;
        if (!pw) continue;
        switch (f->type) {
        case ASK_NOTE:
            th = fm_text_h(pw, cx - 12);
            r->label = fm_child(L"STATIC", pw, SS_LEFT | SS_NOPREFIX, 0, 6, y, cx - 12, th, id);
            y += th + 8;
            break;
        case ASK_ASK:
        case ASK_ASKP: {
            wchar_t *dw = gplus_to_wide(f->deflt);
            th = fm_text_h(pw, cx - 12);
            r->label = fm_child(L"STATIC", pw, SS_LEFT | SS_NOPREFIX, 0, 6, y, cx - 12, th, id);
            y += th + 3;
            r->edit = fm_child(L"EDIT", dw ? dw : L"",
                               ES_AUTOHSCROLL | WS_TABSTOP | WS_GROUP |
                               (f->type == ASK_ASKP ? ES_PASSWORD : 0),
                               WS_EX_CLIENTEDGE, 6, y, cx - 12, 22, id + 1);
            if (r->edit && f->type == ASK_ASKP)
                SendMessageW(r->edit, EM_SETPASSWORDCHAR, (WPARAM)L'*', 0);
            free(dw);
            y += 22 + 10;
            break;
        }
        case ASK_ASKL:
            th = fm_text_h(pw, cx - 12);
            r->label = fm_child(L"STATIC", pw, SS_LEFT | SS_NOPREFIX, 0, 6, y, cx - 12, th, id);
            y += th + 3;
            r->edit = fm_child(L"EDIT", L"",
                               ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL |
                               WS_TABSTOP | WS_GROUP,
                               WS_EX_CLIENTEDGE, 6, y, cx - 12, 80, id + 1);
            y += 80 + 10;
            break;
        case ASK_CHOOSE:
            th = fm_text_h(pw, cx - 12);
            r->label = fm_child(L"STATIC", pw, SS_LEFT | SS_NOPREFIX, 0, 6, y, cx - 12, th, id);
            y += th + 3;
            r->chosen = 0;
            if (f->nchoices > 6) {
                r->combo = fm_child(L"COMBOBOX", L"",
                                    CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                    0, 6, y, cx - 12, 200, id + 1);
                for (j = 0; j < f->nchoices; j++) {
                    wchar_t *cw = gplus_to_wide(f->choices[j]);
                    SendMessageW(r->combo, CB_ADDSTRING, 0, (LPARAM)(cw ? cw : L"?"));
                    free(cw);
                }
                SendMessageW(r->combo, CB_SETCURSEL, 0, 0);
                y += 24 + 10;
            } else {
                for (j = 0; j < f->nchoices && j < FM_MAX_CHOICE; j++) {
                    wchar_t *cw = gplus_to_wide(f->choices[j]);
                    r->radio[j] = fm_child(L"BUTTON", cw ? cw : L"?",
                                           BS_OWNERDRAW | WS_TABSTOP | (j == 0 ? WS_GROUP : 0),
                                           0, 18, y, cx - 30, 18, id + 1 + j);
                    free(cw);
                    r->nradio++;
                    y += 19;
                }
                y += 8;
            }
            break;
        case ASK_SELECT:
            r->checked = f->checked;
            r->check = fm_child(L"BUTTON", pw, BS_OWNERDRAW | WS_TABSTOP | WS_GROUP,
                                0, 6, y, cx - 12, 18, id + 1);
            y += 18 + 8;
            break;
        default:
            break;
        }
        free(pw);
    }
    return y;
}

/* UTF-16 to Latin-1, refusing what does not fit (P6). Returns malloc'd
 * bytes, or NULL with *bad set to the first character that does not fit. */
static char *to_latin1(const wchar_t *w, wchar_t *bad)
{
    size_t n = wcslen(w), i;
    char  *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    for (i = 0; i < n; i++) {
        if (w[i] > 0xFF) { *bad = w[i]; free(s); return NULL; }
        s[i] = (char)(unsigned char)w[i];
    }
    s[n] = 0;
    return s;
}

static wchar_t *get_text(HWND c)
{
    int n = GetWindowTextLengthW(c);
    wchar_t *w = (wchar_t *)calloc((size_t)n + 2, sizeof(wchar_t));
    if (w) GetWindowTextW(c, w, n + 1);
    return w;
}

/* Collect and check the answers; on a refusal say why, focus the field and
 * return 0 (the form stays open). */
static int fm_collect(HWND dlg)
{
    int i;
    char **ans = (char **)calloc((size_t)g_fm_n + 1, sizeof(char *));
    if (!ans) return 0;
    for (i = 0; i < g_fm_n; i++) {
        const GplusAskField *f = &g_fm_f[i];
        FmRow *r = &g_fm_rows[i];
        wchar_t bad = 0;
        char    msg[600];
        switch (f->type) {
        case ASK_ASK: case ASK_ASKP: case ASK_ASKL: {
            wchar_t *w = r->edit ? get_text(r->edit) : NULL;
            char    *a;
            if (!w) break;
            if (f->type == ASK_ASKL) {         /* CRLF -> LF */
                wchar_t *s = w, *d = w;
                for (; *s; s++) if (*s != L'\r') *d++ = *s;
                *d = 0;
            }
            a = to_latin1(w, &bad);
            free(w);
            if (!a) {
                char pr[300], ch[16];
                to_ansi(f->prompt, pr, sizeof pr);
                ch[0] = 0;
                WideCharToMultiByte(CP_UTF8, 0, &bad, 1, ch, sizeof ch - 1, NULL, NULL);
                _snprintf(msg, sizeof msg,
                    "The answer to \"%s\" holds a character (%s, U+%04X) that this "
                    "form cannot send. Gopher+ forms carry ISO Latin-1 text only; "
                    "please write it another way.", pr, ch, (unsigned)bad);
                msg[sizeof msg - 1] = 0;
                message(dlg, "Gopher+ Form", msg);
                SetFocus(r->edit);
                gplus_answers_free(ans, g_fm_n);
                return 0;
            }
            {
                /* P3: a lone "." line would end the answers early */
                const char *p = a;
                int lone = 0;
                while (*p) {
                    const char *e = strchr(p, '\n');
                    size_t k = e ? (size_t)(e - p) : strlen(p);
                    if (k == 1 && p[0] == '.') lone = 1;
                    p += k; if (*p) p++;
                }
                if (lone || strcmp(a, ".") == 0) {
                    char pr[300];
                    to_ansi(f->prompt, pr, sizeof pr);
                    _snprintf(msg, sizeof msg,
                        "The answer to \"%s\" cannot be a single period (.) on a "
                        "line of its own: the server would take it as the end of "
                        "the answers.", pr);
                    msg[sizeof msg - 1] = 0;
                    message(dlg, "Gopher+ Form", msg);
                    free(a);
                    SetFocus(r->edit);
                    gplus_answers_free(ans, g_fm_n);
                    return 0;
                }
            }
            ans[i] = a;
            break;
        }
        case ASK_CHOOSE: {
            int k = r->combo ? (int)SendMessageW(r->combo, CB_GETCURSEL, 0, 0) : r->chosen;
            if (k < 0 || k >= f->nchoices) k = 0;
            /* the choice's own bytes, as the server sent them (UMN sends the
             * chosen choice's text) */
            ans[i] = _strdup(f->choices[k] ? f->choices[k] : "");
            break;
        }
        case ASK_SELECT:
            ans[i] = _strdup(r->checked ? "1" : "0");
            break;
        default:
            break;
        }
    }
    g_fm_answers = ans;
    return 1;
}

static INT_PTR CALLBACK form_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG: {
        RECT pr, dr, cr, ok;
        int  content, maxh, ph, grow, mon_h;
        wchar_t *tw = gplus_to_wide(g_fm_title);
        HMONITOR mon;
        MONITORINFO mi;
        if (tw) { SetDlgItemTextW(h, IDC_FM_PANE + 1, tw); free(tw); }
        g_fm_font = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
        g_fm_pane = GetDlgItem(h, IDC_FM_PANE);
        GetClientRect(g_fm_pane, &pr);
        content = fm_build(pr.right - pr.left - GetSystemMetrics(SM_CXVSCROLL));
        g_fm_content_h = content;
        /* the pane grows to the content, up to 3/5 of the monitor */
        mon = MonitorFromWindow(g_owner ? g_owner : h, MONITOR_DEFAULTTONEAREST);
        mi.cbSize = sizeof mi;
        GetMonitorInfoW(mon, &mi);
        mon_h = mi.rcWork.bottom - mi.rcWork.top;
        maxh = mon_h * 3 / 5;
        ph = content < maxh ? content : maxh;
        if (ph < 40) ph = 40;
        grow = ph - (pr.bottom - pr.top);
        GetWindowRect(h, &dr);
        SetWindowPos(h, NULL, 0, 0, dr.right - dr.left, dr.bottom - dr.top + grow,
                     SWP_NOMOVE | SWP_NOZORDER);
        GetWindowRect(g_fm_pane, &cr);
        MapWindowPoints(NULL, h, (POINT *)&cr, 2);
        SetWindowPos(g_fm_pane, NULL, 0, 0, cr.right - cr.left, ph, SWP_NOMOVE | SWP_NOZORDER);
        {   /* OK and Cancel follow the bottom */
            int ids[2] = { IDOK, IDCANCEL }, k;
            for (k = 0; k < 2; k++) {
                HWND b = GetDlgItem(h, ids[k]);
                GetWindowRect(b, &ok);
                MapWindowPoints(NULL, h, (POINT *)&ok, 2);
                SetWindowPos(b, NULL, ok.left, ok.top + grow, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
        }
        g_fm_scroll = 0;
        if (content > ph) fm_set_scroll();
        else ShowScrollBar(g_fm_pane, SB_VERT, FALSE);
        centre(h);
        SetFocus(GetNextDlgTabItem(h, NULL, FALSE));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            if (fm_collect(h)) EndDialog(h, 1);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(h, 0); return TRUE; }
        return FALSE;
    case WM_CLOSE:
        EndDialog(h, 0);
        return TRUE;
    }
    return FALSE;
}

void gplus_answers_free(char **answers, int nf)
{
    int i;
    if (!answers) return;
    for (i = 0; i < nf; i++) free(answers[i]);
    free(answers);
}

int gplus_form_dialog(HWND owner, const char *title,
                      const GplusAskField *f, int nf, char ***out_answers)
{
    static unsigned char buf[4096];
    static BOOL          reg = FALSE;
    unsigned char *p = buf;
    INT_PTR rc;
    if (!f || nf <= 0 || !out_answers || nf > FM_MAX_FIELDS) return 0;
    *out_answers = NULL;
    if (!reg) {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc   = fm_pane_proc;
        wc.hInstance     = (HINSTANCE)GetModuleHandleW(NULL);
        wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
        wc.lpszClassName = FM_PANE_CLASS;
        RegisterClassW(&wc);
        reg = TRUE;
    }
    g_fm_rows = (FmRow *)calloc((size_t)nf, sizeof(FmRow));
    if (!g_fm_rows) return 0;
    memset(buf, 0, sizeof buf);
    /* 270 units wide; the pane starts 60 units tall and is grown in
     * WM_INITDIALOG to fit the rows. The title line names the item. */
    p = put_header(p, L"Gopher+ Form", 4, 270, 104);
    p = put_item(p, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 7, 6, 256, 9,
                 IDC_FM_PANE + 1, 0x0082, L"");
    /* the pane: our own class, by name (no atom) */
    {
        DLGITEMTEMPLATE it;
        p = align4(p);
        it.style = WS_CHILD | WS_VISIBLE | WS_VSCROLL;
        it.dwExtendedStyle = WS_EX_CONTROLPARENT | WS_EX_CLIENTEDGE;
        it.x = 7; it.y = 18; it.cx = 256; it.cy = 60; it.id = IDC_FM_PANE;
        memcpy(p, &it, sizeof it);
        p += sizeof it;
        p = put_wstr(p, FM_PANE_CLASS);
        p = put_wstr(p, NULL);
        p = put_word(p, 0);
    }
    p = put_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, 0, 157, 84, 50, 14, IDOK, 0x0080, L"OK");
    p = put_item(p, BS_PUSHBUTTON | WS_TABSTOP, 0, 213, 84, 50, 14, IDCANCEL, 0x0080, L"Cancel");
    (void)p;
    g_owner = owner; g_fm_f = f; g_fm_n = nf; g_fm_title = title;
    g_fm_answers = NULL; g_fm_scroll = 0; g_fm_content_h = 0;
    rc = DialogBoxIndirectParamW((HINSTANCE)GetModuleHandleW(NULL), (DLGTEMPLATE *)buf,
                                 owner, form_proc, 0);
    free(g_fm_rows);
    g_fm_rows = NULL; g_fm_pane = NULL; g_fm_f = NULL;
    if (rc == 1 && g_fm_answers) {
        *out_answers = g_fm_answers;
        g_fm_answers = NULL;
        return 1;
    }
    gplus_answers_free(g_fm_answers, nf);
    g_fm_answers = NULL;
    return 0;
}
