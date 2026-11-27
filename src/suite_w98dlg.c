/*
 * suite_w98dlg.c - Windows 98 style dialogs (Navigator 1.8).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See suite_w98dlg.h. Geometry measured from the Windows 98 SE references
 * in refshots\active_channel (win98_01 prompt, win98_02 confirm, win98_09
 * already exists); colours and metrics are the Windows 98 standard ones,
 * because the references are scaled captures (their face reads 201 gray).
 * Report: docs/2026-11-21_ACTIVE_CHANNEL_SUBSCRIBE_P2.md.
 */
#include "suite_w98dlg.h"
#include "suite_shell.h"
#include "suite_w31.h"   /* R2.2: the popup menu's frame */
#include <string.h>
#include <stdio.h>

#include "w98_icons_data.h"

#define W98_CLASS   "NavW98Dialog"
#define FRAME       3            /* SM_CXFIXEDFRAME */
#define CAP_H       18           /* the caption band */
#define CLIENT_Y    (FRAME + CAP_H + 1)
#define BTN_W       75
#define BTN_H       23
#define CLOSE_W     16
#define CLOSE_H     14

#define C_FACE      RGB(192, 192, 192)
#define C_LIGHT     RGB(192, 192, 192)   /* COLOR_3DLIGHT on Windows 98 */
#define C_HILITE    RGB(255, 255, 255)
#define C_SHADOW    RGB(128, 128, 128)
#define C_DKSHADOW  RGB(0, 0, 0)
#define C_CAP_L     RGB(0, 0, 128)       /* active caption, gradient left */
#define C_CAP_R     RGB(16, 132, 208)    /* ... and right */
#define C_ICAP_L    RGB(128, 128, 128)   /* inactive */
#define C_ICAP_R    RGB(192, 192, 192)
#define C_ICAP_TEXT RGB(192, 192, 192)

typedef struct {
    w98_dialog_t d;
    HWND   hwnd;
    HFONT  font, bold;
    int    w, h;                         /* the window (= our client) */
    RECT   text_rc;
    RECT   field_rc[2][2];               /* [row][0 label, 1 value] */
    RECT   btn[W98_MAX_BUTTONS];
    RECT   close_rc;
    int    icon_x, icon_y;
    int    focus;                        /* button index with the focus */
    int    press;                        /* button index held by the mouse, -1 */
    int    press_in;                     /* the pointer is still on it */
    int    space;                        /* Space held on the focused button */
    int    close_down, close_in;
    int    active;
    int    done, result;
    HWND   edit;                         /* part 8: the text box, or NULL */
    RECT   edit_rc;                      /* its sunken frame */
    RECT   list_rc;                      /* 2026-11-26: the CHECKLIST box */
} W98;

#define LIST_ROW_H  17                   /* a check box row: 13 + 4 */
#define LIST_FOCUS  (-1)                 /* s->focus: the list has it */
static void w98m_check(HDC dc, int x, int y, COLORREF c);

/* ------------------------------------------------------------------ */
/* Drawing.                                                            */
/* ------------------------------------------------------------------ */

static void hline(HDC hdc, int x0, int x1, int y, COLORREF c)
{
    RECT r; HBRUSH b;
    if (x1 < x0) return;
    r.left = x0; r.right = x1 + 1; r.top = y; r.bottom = y + 1;
    b = CreateSolidBrush(c); FillRect(hdc, &r, b); DeleteObject(b);
}

static void vline(HDC hdc, int x, int y0, int y1, COLORREF c)
{
    RECT r; HBRUSH b;
    if (y1 < y0) return;
    r.left = x; r.right = x + 1; r.top = y0; r.bottom = y1 + 1;
    b = CreateSolidBrush(c); FillRect(hdc, &r, b); DeleteObject(b);
}

/* A two-line 3D edge: outer tl/br, inner tl/br (inclusive rect). */
static void edge(HDC hdc, int x0, int y0, int x1, int y1,
                 COLORREF otl, COLORREF obr, COLORREF itl, COLORREF ibr)
{
    hline(hdc, x0, x1 - 1, y0, otl); vline(hdc, x0, y0, y1 - 1, otl);
    hline(hdc, x0, x1, y1, obr);     vline(hdc, x1, y0, y1, obr);
    hline(hdc, x0 + 1, x1 - 2, y0 + 1, itl); vline(hdc, x0 + 1, y0 + 1, y1 - 2, itl);
    hline(hdc, x0 + 1, x1 - 1, y1 - 1, ibr); vline(hdc, x1 - 1, y0 + 1, y1 - 1, ibr);
}

static void draw_icon(HDC hdc, int which, int x, int y)
{
    const char *const *map = which == W98_ICON_QUESTION ? k_w98_icon_question
                           : which == W98_ICON_DESKITEM ? k_w98_icon_deskitem
                           : which == W98_ICON_CHANNEL ? k_w98_icon_channel : NULL;
    int r, c;
    if (!map) return;
    for (r = 0; r < 32; r++)
        for (c = 0; c < 32; c++) {
            COLORREF col;
            switch (map[r][c]) {
            case 'k': col = RGB(0, 0, 0);       break;
            case 'w': col = RGB(255, 255, 255); break;
            case 'g': col = RGB(128, 128, 128); break;
            case 'l': col = RGB(192, 192, 192); break;
            case 'y': col = RGB(255, 255, 0);   break;
            case 'o': col = RGB(128, 128, 0);   break;
            case 'b': col = RGB(0, 0, 255);     break;
            case 'n': col = RGB(0, 0, 128);     break;
            case 't': col = RGB(0, 128, 128);   break;
            case 'c': col = RGB(255, 255, 192); break;
            case 'z': col = RGB(255, 255, 224); break;
            case 'r': col = RGB(255, 0, 0);     break;
            case 'e': col = RGB(0, 192, 0);     break;
            default:  continue;
            }
            SetPixelV(hdc, x + c, y + r, col);
        }
}

/* The close box's cross: Marlett's 'r' at 8 pt, 8 x 7 pixels. */
static void draw_cross(HDC hdc, int x, int y, COLORREF c)
{
    static const char *const k_x[7] = {
        "xx....xx", ".xx..xx.", "..xxxx..", "...xx...", "..xxxx..", ".xx..xx.", "xx....xx" };
    int r, k;
    for (r = 0; r < 7; r++)
        for (k = 0; k < 8; k++)
            if (k_x[r][k] == 'x') SetPixelV(hdc, x + k, y + r, c);
}

static void draw_button(W98 *s, HDC hdc, int i)
{
    RECT r = s->btn[i];
    int x0 = r.left, y0 = r.top, x1 = r.right - 1, y1 = r.bottom - 1;
    int focused = (i == s->focus);
    int down = (s->press == i && s->press_in) || (s->space && focused);
    int en = s->d.buttons[i].enabled;
    RECT tr;
    HBRUSH face = CreateSolidBrush(C_FACE);
    FillRect(hdc, &r, face);
    DeleteObject(face);
    if (focused) {
        /* the default frame: one black line all round, the button inside */
        hline(hdc, x0, x1, y0, C_DKSHADOW); hline(hdc, x0, x1, y1, C_DKSHADOW);
        vline(hdc, x0, y0, y1, C_DKSHADOW); vline(hdc, x1, y0, y1, C_DKSHADOW);
        x0++; y0++; x1--; y1--;
    }
    if (down) {
        /* pressed: flat, a dark gray line inside the black one */
        hline(hdc, x0, x1, y0, C_SHADOW); hline(hdc, x0, x1, y1, C_SHADOW);
        vline(hdc, x0, y0, y1, C_SHADOW); vline(hdc, x1, y0, y1, C_SHADOW);
    } else {
        edge(hdc, x0, y0, x1, y1, C_HILITE, C_DKSHADOW, C_LIGHT, C_SHADOW);
    }
    tr = r;
    if (down) OffsetRect(&tr, 1, 1);
    SelectObject(hdc, s->font);
    SetBkMode(hdc, TRANSPARENT);
    if (en) {
        SetTextColor(hdc, RGB(0, 0, 0));
        DrawTextA(hdc, s->d.buttons[i].label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        RECT t2 = tr;
        OffsetRect(&t2, 1, 1);
        SetTextColor(hdc, C_HILITE);
        DrawTextA(hdc, s->d.buttons[i].label, -1, &t2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(hdc, C_SHADOW);
        DrawTextA(hdc, s->d.buttons[i].label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (focused && en) {
        RECT f = r;
        InflateRect(&f, -4, -4);
        if (down) OffsetRect(&f, 1, 1);
        SetTextColor(hdc, RGB(0, 0, 0));
        SetBkColor(hdc, C_FACE);
        DrawFocusRect(hdc, &f);
    }
}

static void paint(W98 *s, HDC hdc)
{
    RECT all, cap;
    HBRUSH b;
    int i;
    all.left = 0; all.top = 0; all.right = s->w; all.bottom = s->h;
    b = CreateSolidBrush(C_FACE); FillRect(hdc, &all, b); DeleteObject(b);

    /* the fixed frame: light / black, then white / dark gray, then face */
    edge(hdc, 0, 0, s->w - 1, s->h - 1, C_LIGHT, C_DKSHADOW, C_HILITE, C_SHADOW);

    /* the caption: a horizontal gradient, the title in bold */
    cap.left = FRAME; cap.top = FRAME; cap.right = s->w - FRAME; cap.bottom = FRAME + CAP_H;
    {
        TRIVERTEX v[2];
        GRADIENT_RECT gr;
        COLORREF l = s->active ? C_CAP_L : C_ICAP_L, r = s->active ? C_CAP_R : C_ICAP_R;
        v[0].x = cap.left;  v[0].y = cap.top;
        v[0].Red = (COLOR16)(GetRValue(l) << 8); v[0].Green = (COLOR16)(GetGValue(l) << 8);
        v[0].Blue = (COLOR16)(GetBValue(l) << 8); v[0].Alpha = 0;
        v[1].x = cap.right; v[1].y = cap.bottom;
        v[1].Red = (COLOR16)(GetRValue(r) << 8); v[1].Green = (COLOR16)(GetGValue(r) << 8);
        v[1].Blue = (COLOR16)(GetBValue(r) << 8); v[1].Alpha = 0;
        gr.UpperLeft = 0; gr.LowerRight = 1;
        if (!GradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H)) {
            b = CreateSolidBrush(l); FillRect(hdc, &cap, b); DeleteObject(b);
        }
    }
    {
        RECT t = cap;
        t.left += 3;
        t.right = s->close_rc.left - 2;
        SelectObject(hdc, s->bold);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, s->active ? RGB(255, 255, 255) : C_ICAP_TEXT);
        DrawTextA(hdc, s->d.caption ? s->d.caption : "", -1, &t,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    /* the close box: a small push button with the cross */
    {
        RECT c = s->close_rc;
        int down = s->close_down && s->close_in;
        b = CreateSolidBrush(C_FACE); FillRect(hdc, &c, b); DeleteObject(b);
        if (down) edge(hdc, c.left, c.top, c.right - 1, c.bottom - 1, C_DKSHADOW, C_HILITE, C_SHADOW, C_LIGHT);
        else      edge(hdc, c.left, c.top, c.right - 1, c.bottom - 1, C_HILITE, C_DKSHADOW, C_LIGHT, C_SHADOW);
        draw_cross(hdc, c.left + 4 + down, c.top + 3 + down, RGB(0, 0, 0));
    }

    if (s->d.icon) draw_icon(hdc, s->d.icon, s->icon_x, s->icon_y);

    SelectObject(hdc, s->font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    if (s->d.text) {
        RECT t = s->text_rc;
        DrawTextA(hdc, s->d.text, -1, &t, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    for (i = 0; i < s->d.fields_n && i < 2; i++) {
        RECT a = s->field_rc[i][0], v = s->field_rc[i][1];
        DrawTextA(hdc, s->d.field_label[i] ? s->d.field_label[i] : "", -1, &a,
                  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
        if (s->d.edit_field == i + 1) {
            /* the Windows 98 text box: white, a 2 px sunken edge (gray and
             * white outside, black and light gray inside) */
            RECT e = s->edit_rc;
            b = CreateSolidBrush(RGB(255, 255, 255)); FillRect(hdc, &e, b); DeleteObject(b);
            edge(hdc, e.left, e.top, e.right - 1, e.bottom - 1, C_SHADOW, C_HILITE, C_DKSHADOW, C_LIGHT);
            continue;
        }
        DrawTextA(hdc, s->d.field_value[i] ? s->d.field_value[i] : "", -1, &v,
                  DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
    }
    if (s->d.layout == W98_LAYOUT_CHECKLIST) {
        /* 2026-11-26: the Windows 98 list box: white, a 2 px sunken edge;
         * each row a check box (13 x 13, its own 2 px sunken edge, white,
         * the classic check) and the label; the selected row navy with
         * white text, a dotted focus rectangle while the list has focus. */
        RECT e = s->list_rc, clip;
        int k, has_focus = (s->focus == LIST_FOCUS && GetFocus() == s->hwnd);
        b = CreateSolidBrush(RGB(255, 255, 255)); FillRect(hdc, &e, b); DeleteObject(b);
        edge(hdc, e.left, e.top, e.right - 1, e.bottom - 1, C_SHADOW, C_HILITE, C_DKSHADOW, C_LIGHT);
        clip = e; InflateRect(&clip, -2, -2);
        IntersectClipRect(hdc, clip.left, clip.top, clip.right, clip.bottom);
        for (k = 0; k < s->d.items_n; k++) {
            int y = clip.top + k * LIST_ROW_H, cx = clip.left + 2, cy = y + 2;
            RECT lab;
            SIZE tsz;
            /* the check box; a disabled one (2026-11-26) as the period draws
             * it: the face colour inside, the check in the shadow gray */
            b = CreateSolidBrush(s->d.items[k].disabled ? C_FACE : RGB(255, 255, 255));
            { RECT cb; SetRect(&cb, cx, cy, cx + 13, cy + 13); FillRect(hdc, &cb, b); }
            DeleteObject(b);
            edge(hdc, cx, cy, cx + 12, cy + 12, C_SHADOW, C_HILITE, C_DKSHADOW, C_LIGHT);
            if (s->d.items[k].checked)
                w98m_check(hdc, cx + 3, cy + 3, s->d.items[k].disabled ? C_SHADOW : RGB(0, 0, 0));
            /* the label */
            GetTextExtentPoint32A(hdc, s->d.items[k].label, (int)strlen(s->d.items[k].label), &tsz);
            SetRect(&lab, cx + 13 + 4, y + 1, cx + 13 + 4 + tsz.cx + 4, y + LIST_ROW_H - 1);
            if (k == s->d.list_sel) {
                b = CreateSolidBrush(C_CAP_L); FillRect(hdc, &lab, b); DeleteObject(b);
                SetTextColor(hdc, RGB(255, 255, 255));
            } else {
                SetTextColor(hdc, RGB(0, 0, 0));
            }
            {
                RECT tr = lab;
                tr.left += 2;
                DrawTextA(hdc, s->d.items[k].label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            if (k == s->d.list_sel && has_focus) {
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkColor(hdc, C_CAP_L);
                DrawFocusRect(hdc, &lab);
            }
        }
        SelectClipRgn(hdc, NULL);
        SetTextColor(hdc, RGB(0, 0, 0));
        /* for the gates: each button's enabled state, the selected row */
        for (k = 0; k < s->d.buttons_n; k++) {
            char key[32];
            snprintf(key, sizeof key, "NavW98.En%d", s->d.buttons[k].id);
            SetPropA(s->hwnd, key, (HANDLE)(INT_PTR)(s->d.buttons[k].enabled ? 2 : 1));
        }
        SetPropA(s->hwnd, "NavW98.Sel", (HANDLE)(INT_PTR)(s->d.list_sel + 1));
    }
    for (i = 0; i < s->d.buttons_n; i++) draw_button(s, hdc, i);
}

/* ------------------------------------------------------------------ */
/* Layout.                                                             */
/* ------------------------------------------------------------------ */

static void text_size(W98 *s, HDC hdc, const char *t, int wrap, int *w, int *h)
{
    RECT r;
    r.left = 0; r.top = 0; r.right = wrap > 0 ? wrap : 4000; r.bottom = 0;
    SelectObject(hdc, s->font);
    DrawTextA(hdc, t ? t : "", -1, &r,
              DT_CALCRECT | DT_LEFT | DT_NOPREFIX | (wrap > 0 ? DT_WORDBREAK : 0));
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

/* Everything in window coordinates (the window is all ours). */
static void layout(W98 *s)
{
    HDC hdc = GetDC(NULL);
    int cw, ch, tw, th, i, n = s->d.buttons_n;
    int cx = FRAME, cy = CLIENT_Y;      /* client origin */
    HGDIOBJ old = SelectObject(hdc, s->font);

    if (s->d.layout == W98_LAYOUT_MSGBOX) {
        /* win98_01: icon at (19,12), text at (62,11), buttons on the bottom
         * right, 6 apart, 10 from the right; client 328 wide at least */
        int wrap = s->d.text_width > 0 ? s->d.text_width : 254;
        int content_b;
        text_size(s, hdc, s->d.text, wrap, &tw, &th);
        cw = 62 + tw + 12;
        if (cw < 328) cw = 328;
        if (cw < n * BTN_W + (n - 1) * 6 + 20) cw = n * BTN_W + (n - 1) * 6 + 20;
        s->icon_x = cx + 19; s->icon_y = cy + 12;
        SetRect(&s->text_rc, cx + 62, cy + 11, cx + 62 + wrap, cy + 11 + th);
        content_b = 11 + th;
        if (s->d.icon && content_b < 12 + 32) content_b = 12 + 32;
        {
            int by = content_b + 33, bx = cw - 10 - BTN_W;
            for (i = n - 1; i >= 0; i--) {
                SetRect(&s->btn[i], cx + bx, cy + by, cx + bx + BTN_W, cy + by + BTN_H);
                bx -= BTN_W + 6;
            }
            ch = by + BTN_H + 9;
        }
    } else if (s->d.layout == W98_LAYOUT_COLUMN) {
        /* win98_02: icon at (11,11), text at (52,13) up to the button
         * column, Name/URL rows 24 apart with values at x 94, OK and Cancel
         * at the top right 29 apart, the last button at the bottom right */
        int wrap = s->d.text_width > 0 ? s->d.text_width : 280;
        int y, bx;
        text_size(s, hdc, s->d.text, wrap, &tw, &th);
        cw = 52 + wrap + 9 + BTN_W + 11;
        bx = cw - 11 - BTN_W;
        s->icon_x = cx + 11; s->icon_y = cy + 11;
        SetRect(&s->text_rc, cx + 52, cy + 13, cx + 52 + wrap, cy + 13 + th);
        y = 13 + th;
        if (y < 11 + 32) y = 11 + 32;
        y += 22;
        for (i = 0; i < s->d.fields_n && i < 2; i++) {
            SetRect(&s->field_rc[i][0], cx + 52, cy + y, cx + 94, cy + y + 13);
            SetRect(&s->field_rc[i][1], cx + 94, cy + y, cx + bx - 9, cy + y + 13);
            if (s->d.edit_field == i + 1)        /* part 8: a 21 px text box */
                SetRect(&s->edit_rc, cx + 100, cy + y - 4, cx + bx - 9, cy + y + 17);
            y += 24;
        }
        for (i = 0; i < n; i++) {
            int byy = (i == n - 1 && n >= 3) ? -1 : 11 + i * 29;
            if (byy >= 0)
                SetRect(&s->btn[i], cx + bx, cy + byy, cx + bx + BTN_W, cy + byy + BTN_H);
        }
        {
            int last_y = y + 10;                    /* below the fields */
            if (last_y < 11 + 2 * 29 + 20) last_y = 11 + 2 * 29 + 20;
            if (n >= 3)
                SetRect(&s->btn[n - 1], cx + bx, cy + last_y, cx + bx + BTN_W, cy + last_y + BTN_H);
            ch = last_y + BTN_H + 11;
            /* part 8: with no bottom button (two or fewer), no room kept
             * for one: the fields' last row or the buttons end it */
            if (n < 3) {
                ch = y + 4;
                if (ch < 11 + (n - 1) * 29 + BTN_H + 11) ch = 11 + (n - 1) * 29 + BTN_H + 11;
            }
        }
    } else if (s->d.layout == W98_LAYOUT_CHECKLIST) {
        /* 2026-11-26, Customize My Desktop: the list at (11,11) (the period
         * Web tab's item list, win98_bar_03: check box rows, buttons in a
         * column beside it 29 apart), then OK / Cancel / Apply along the
         * bottom right 6 apart, as the MSGBOX row */
        int lw = s->d.list_w > 0 ? s->d.list_w : 260;
        int rows = s->d.list_rows > 0 ? s->d.list_rows : 6;
        int lh = rows * LIST_ROW_H + 4, col = s->d.column_buttons, bx, by, k;
        SetRect(&s->list_rc, cx + 11, cy + 11, cx + 11 + lw, cy + 11 + lh);
        bx = 11 + lw + 8;
        for (k = 0; k < col && k < n; k++)
            SetRect(&s->btn[k], cx + bx, cy + 11 + k * 29, cx + bx + BTN_W, cy + 11 + k * 29 + BTN_H);
        cw = bx + BTN_W + 11;
        by = 11 + lh + 14;
        {
            int x = cw - 11 - BTN_W;
            for (k = n - 1; k >= col; k--) {
                SetRect(&s->btn[k], cx + x, cy + by, cx + x + BTN_W, cy + by + BTN_H);
                x -= BTN_W + 6;
            }
        }
        ch = by + BTN_H + 10;
    } else {
        /* win98_09: text at (11,11), one row of buttons centred */
        int total, bx, by;
        text_size(s, hdc, s->d.text, s->d.text_width, &tw, &th);
        cw = tw + 22;
        total = n * BTN_W + (n - 1) * 6;
        if (cw < total + 22) cw = total + 22;
        if (cw < 200) cw = 200;
        SetRect(&s->text_rc, cx + 11, cy + 11, cx + 11 + tw, cy + 11 + th);
        by = 11 + th + 18;
        bx = (cw - total) / 2;
        for (i = 0; i < n; i++) {
            SetRect(&s->btn[i], cx + bx, cy + by, cx + bx + BTN_W, cy + by + BTN_H);
            bx += BTN_W + 6;
        }
        ch = by + BTN_H + 10;
    }
    s->w = cw + 2 * FRAME;
    s->h = ch + CLIENT_Y + FRAME;
    SetRect(&s->close_rc, s->w - FRAME - 2 - CLOSE_W, FRAME + 2,
            s->w - FRAME - 2, FRAME + 2 + CLOSE_H);
    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
}

/* ------------------------------------------------------------------ */
/* Behaviour.                                                          */
/* ------------------------------------------------------------------ */

static void finish(W98 *s, int id)
{
    if (s->done) return;
    s->done = 1;
    s->result = id;
}

static int hit_button(W98 *s, int x, int y)
{
    int i;
    POINT p; p.x = x; p.y = y;
    for (i = 0; i < s->d.buttons_n; i++)
        if (PtInRect(&s->btn[i], p)) return i;
    return -1;
}

static void move_focus(W98 *s, int dir)
{
    int n = s->d.buttons_n, i = s->focus, k;
    if (s->d.layout == W98_LAYOUT_CHECKLIST) {
        /* the list, then each enabled button, round */
        int pos = (i == LIST_FOCUS) ? -1 : i;
        for (k = 0; k <= n; k++) {
            pos += dir;
            if (pos >= n) pos = -1;
            if (pos < -1) pos = n - 1;
            if (pos == -1) { s->focus = LIST_FOCUS; break; }
            if (s->d.buttons[pos].enabled) { s->focus = pos; break; }
        }
        InvalidateRect(s->hwnd, NULL, FALSE);
        return;
    }
    for (k = 0; k < n; k++) {
        i = (i + dir + n) % n;
        if (s->d.buttons[i].enabled) { s->focus = i; break; }
    }
    InvalidateRect(s->hwnd, NULL, FALSE);
}

static void fix_focus(W98 *s)
{
    if (s->focus >= 0 && (s->focus >= s->d.buttons_n || !s->d.buttons[s->focus].enabled))
        s->focus = s->d.layout == W98_LAYOUT_CHECKLIST ? LIST_FOCUS : 0;
    if (s->d.list_sel >= s->d.items_n) s->d.list_sel = s->d.items_n - 1;
}

/* 2026-11-26: a button or a W98_EV_* event through the caller's on_press */
static void notify(W98 *s, int id)
{
    if (!s->d.on_press) { if (id > -100) finish(s, id); return; }   /* events need a listener */
    if (s->d.on_press(s->d.ctx, id, &s->d) && id > -100) finish(s, id);
    fix_focus(s);
    InvalidateRect(s->hwnd, NULL, FALSE);
}

static void press(W98 *s, int i)
{
    if (i < 0 || i >= s->d.buttons_n || !s->d.buttons[i].enabled) return;
    if (s->d.on_press) { notify(s, s->d.buttons[i].id); return; }
    finish(s, s->d.buttons[i].id);
}

static void list_select(W98 *s, int k)
{
    if (k < 0 || k >= s->d.items_n || k == s->d.list_sel) return;
    s->d.list_sel = k;
    notify(s, W98_EV_SELECT);
}

static void list_toggle(W98 *s, int k)
{
    if (k < 0 || k >= s->d.items_n || s->d.items[k].disabled) return;   /* 2026-11-26 */
    s->d.items[k].checked = !s->d.items[k].checked;
    notify(s, W98_EV_TOGGLED);
}

static int mnemonic(W98 *s, int ch)
{
    int i;
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    for (i = 0; i < s->d.buttons_n; i++) {
        const char *a = strchr(s->d.buttons[i].label, '&');
        int c = a ? (unsigned char)a[1] : 0;
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c && c == ch) return i;
    }
    return -1;
}

/* Part 8: the text box's keys. Enter = the default button, Esc = cancel,
 * Tab = on to the buttons (the dialog window takes the keyboard). */
static WNDPROC g_edit_proc0;
static LRESULT CALLBACK W98EditProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    W98 *s = (W98 *)GetWindowLongPtrA(GetParent(h), GWLP_USERDATA);
    if (s) {
        if (m == WM_KEYDOWN && w == VK_RETURN) { finish(s, s->d.default_id); return 0; }
        if (m == WM_KEYDOWN && w == VK_ESCAPE) { finish(s, s->d.cancel_id); return 0; }
        if (m == WM_KEYDOWN && w == VK_TAB) {
            SetFocus(s->hwnd);
            InvalidateRect(s->hwnd, NULL, FALSE);
            return 0;
        }
        if (m == WM_CHAR && (w == '\r' || w == 27 || w == '\t')) return 0;
        if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS | CallWindowProcA(g_edit_proc0, h, m, w, l);
    }
    return CallWindowProcA(g_edit_proc0, h, m, w, l);
}

static LRESULT CALLBACK W98Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    W98 *s = (W98 *)GetWindowLongPtrA(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        CREATESTRUCTA *cs = (CREATESTRUCTA *)l;
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcA(h, m, w, l);
    }
    if (!s) return DefWindowProcA(h, m, w, l);
    switch (m) {
    case WM_NCHITTEST: {
        POINT p;
        p.x = (short)LOWORD(l); p.y = (short)HIWORD(l);
        ScreenToClient(h, &p);
        if (p.y >= FRAME && p.y < FRAME + CAP_H && p.x >= FRAME && p.x < s->w - FRAME
            && !PtInRect(&s->close_rc, p))
            return HTCAPTION;                     /* the caption drags the dialog */
        return HTCLIENT;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        HDC mdc = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, s->w, s->h);
        HGDIOBJ ob = SelectObject(mdc, bmp);
        paint(s, mdc);
        BitBlt(hdc, 0, 0, s->w, s->h, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, ob);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ACTIVATE:
        s->active = LOWORD(w) != WA_INACTIVE;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p; int i;
        p.x = (short)LOWORD(l); p.y = (short)HIWORD(l);
        if (s->d.layout == W98_LAYOUT_CHECKLIST && PtInRect(&s->list_rc, p)) {
            int k = (p.y - (s->list_rc.top + 2)) / LIST_ROW_H;
            s->focus = LIST_FOCUS;
            if (k >= 0 && k < s->d.items_n) {
                int cx = s->list_rc.left + 2 + 2;
                list_select(s, k);
                if (p.x >= cx && p.x < cx + 13) list_toggle(s, k);   /* the box itself */
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        } else if (PtInRect(&s->close_rc, p)) {
            s->close_down = s->close_in = 1;
            SetCapture(h);
        } else if ((i = hit_button(s, p.x, p.y)) >= 0 && s->d.buttons[i].enabled) {
            s->press = i; s->press_in = 1; s->focus = i;
            SetCapture(h);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p;
        p.x = (short)LOWORD(l); p.y = (short)HIWORD(l);
        if (s->press >= 0) {
            int in = PtInRect(&s->btn[s->press], p);
            if (in != s->press_in) { s->press_in = in; InvalidateRect(h, NULL, FALSE); }
        } else if (s->close_down) {
            int in = PtInRect(&s->close_rc, p);
            if (in != s->close_in) { s->close_in = in; InvalidateRect(h, NULL, FALSE); }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        int was = s->press, in = s->press_in, cl = s->close_down && s->close_in;
        s->press = -1; s->press_in = 0; s->close_down = s->close_in = 0;
        if (GetCapture() == h) ReleaseCapture();
        InvalidateRect(h, NULL, FALSE);
        if (was >= 0 && in) press(s, was);
        else if (cl) finish(s, s->d.cancel_id);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (s->press >= 0 || s->close_down) {
            s->press = -1; s->press_in = 0; s->close_down = s->close_in = 0;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (s->d.layout == W98_LAYOUT_CHECKLIST && s->focus == LIST_FOCUS) {
            if (w == VK_UP)   { list_select(s, s->d.list_sel - 1); return 0; }
            if (w == VK_DOWN) { list_select(s, s->d.list_sel + 1); return 0; }
            if (w == VK_SPACE) { list_toggle(s, s->d.list_sel); return 0; }
            if (w == VK_RETURN) { int k; for (k = 0; k < s->d.buttons_n; k++)
                                      if (s->d.buttons[k].id == s->d.default_id) { press(s, k); break; }
                                  return 0; }
        }
        switch (w) {
        case VK_TAB:
            /* part 8: the text box is in the Tab order, before the buttons */
            if (s->edit && (((GetKeyState(VK_SHIFT) & 0x8000) && s->focus == 0)
                            || (!(GetKeyState(VK_SHIFT) & 0x8000) && s->focus == s->d.buttons_n - 1))) {
                SetFocus(s->edit);
                SendMessageA(s->edit, EM_SETSEL, 0, (LPARAM)-1);
                return 0;
            }
            move_focus(s, (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
        case VK_LEFT: case VK_UP:
            move_focus(s, -1);
            return 0;
        case VK_RIGHT: case VK_DOWN:
            move_focus(s, 1);
            return 0;
        case VK_RETURN:
            press(s, s->focus);
            return 0;
        case VK_SPACE:
            if (!s->space) { s->space = 1; InvalidateRect(h, NULL, FALSE); }
            return 0;
        case VK_ESCAPE:
            finish(s, s->d.cancel_id);
            return 0;
        }
        return 0;
    case WM_KEYUP:
        if (s->focus == LIST_FOCUS) return 0;
        if (w == VK_SPACE && s->space) {
            s->space = 0;
            InvalidateRect(h, NULL, FALSE);
            press(s, s->focus);
        }
        return 0;
    case WM_CHAR:
    case WM_SYSCHAR: {
        int i = mnemonic(s, (int)w);
        if (i >= 0) {
            if (s->d.buttons[i].enabled) { s->focus = i; press(s, i); }
            return 0;
        }
        if (m == WM_SYSCHAR) return 0;          /* no beep for Alt+other */
        return 0;
    }
    case WM_CLOSE:
        finish(s, s->d.cancel_id);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* R2.2 (2026-11-23): the Windows 98 popup menu.                       */
/* ------------------------------------------------------------------ */

#define W98M_CLASS   "NavW98MenuOwner"
#define W98M_MAX     16
#define W98M_ITEM_H  17           /* MS Sans Serif 8 (13) + 4 */
#define W98M_SEP_H   9
#define W98M_LEFT    19           /* the check mark column */
#define W98M_RIGHT   17

typedef struct { char text[96]; char key[32]; int sep; } W98MItem;
static W98MItem g_w98m[W98M_MAX];
static int      g_w98m_n = 0;
static HFONT    g_w98m_font = NULL;

static const W98MItem *w98m_of(ULONG_PTR d)
{
    int i;
    for (i = 0; i < g_w98m_n; i++) if ((ULONG_PTR)&g_w98m[i] == d) return &g_w98m[i];
    return NULL;
}

/* The classic 7 x 7 check mark. */
static void w98m_check(HDC dc, int x, int y, COLORREF c)
{
    static const char *const g[7] = {
        "......#", ".....##", "#...###", "##.###.", "#####..", ".###...", "..#...." };
    int r, k;
    for (r = 0; r < 7; r++)
        for (k = 0; k < 7; k++)
            if (g[r][k] == '#') SetPixelV(dc, x + k, y + r, c);
}

static void w98m_draw(const DRAWITEMSTRUCT *di)
{
    const W98MItem *e = w98m_of(di->itemData);
    HDC dc = di->hDC;
    RECT r = di->rcItem, t;
    BOOL sel = (di->itemState & ODS_SELECTED) != 0;
    BOOL dis = (di->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
    HBRUSH b;
    HGDIOBJ of;
    if (!e) return;
    b = CreateSolidBrush(e->sep ? C_FACE : (sel ? RGB(0, 0, 128) : C_FACE));
    FillRect(dc, &r, b);
    DeleteObject(b);
    if (e->sep) {                                   /* etched: gray over white */
        int y = (r.top + r.bottom) / 2 - 1;
        hline(dc, r.left + 1, r.right - 1, y, C_SHADOW);
        hline(dc, r.left + 1, r.right - 1, y + 1, C_HILITE);
        w31_menu_frame_after_item(dc);
        return;
    }
    of = SelectObject(dc, g_w98m_font);
    SetBkMode(dc, TRANSPARENT);
    if (di->itemState & ODS_CHECKED)
        w98m_check(dc, r.left + 6, r.top + (r.bottom - r.top - 7) / 2,
                   sel ? RGB(255, 255, 255) : (dis ? C_SHADOW : RGB(0, 0, 0)));
    t = r;
    t.left += W98M_LEFT;
    t.right -= W98M_RIGHT;
    if (dis && !sel) {                              /* embossed: white, then gray */
        RECT s = t;
        OffsetRect(&s, 1, 1);
        SetTextColor(dc, C_HILITE);
        DrawTextA(dc, e->text, -1, &s, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (e->key[0]) DrawTextA(dc, e->key, -1, &s, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(dc, dis ? C_SHADOW : (sel ? RGB(255, 255, 255) : RGB(0, 0, 0)));
    DrawTextA(dc, e->text, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (e->key[0]) DrawTextA(dc, e->key, -1, &t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    w31_menu_frame_after_item(dc);
}

static LRESULT CALLBACK W98MenuOwnerProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)l;
        const W98MItem *e = mi ? w98m_of(mi->itemData) : NULL;
        if (mi && mi->CtlType == ODT_MENU && e) {
            if (e->sep) {
                mi->itemHeight = W98M_SEP_H;
                mi->itemWidth = 0;
            } else {
                HDC dc = GetDC(h);
                HGDIOBJ of = SelectObject(dc, g_w98m_font);
                SIZE a, k;
                RECT rr = { 0, 0, 0, 0 };
                DrawTextA(dc, e->text, -1, &rr, DT_CALCRECT | DT_SINGLELINE);
                a.cx = rr.right;
                k.cx = 0;
                if (e->key[0]) GetTextExtentPoint32A(dc, e->key, (int)strlen(e->key), &k);
                SelectObject(dc, of);
                ReleaseDC(h, dc);
                mi->itemHeight = W98M_ITEM_H;
                /* the whole width (measured live: Windows adds nothing here) */
                mi->itemWidth = W98M_LEFT + a.cx + (k.cx ? 24 + k.cx : 0) + W98M_RIGHT;
            }
            return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)l;
        if (di && di->CtlType == ODT_MENU && w98m_of(di->itemData)) { w98m_draw(di); return TRUE; }
        break;
    }
    case WM_MENUCHAR: {
        /* the underlined letter of an item chooses it */
        int ch = LOWORD(w), i;
        HMENU mm = (HMENU)l;
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        for (i = 0; i < g_w98m_n; i++) {
            const char *a = strchr(g_w98m[i].text, '&');
            int c = a ? (unsigned char)a[1] : 0;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (c && c == ch) {
                int pos, n = GetMenuItemCount(mm);
                for (pos = 0; pos < n; pos++) {
                    MENUITEMINFOA mi;
                    ZeroMemory(&mi, sizeof mi);
                    mi.cbSize = sizeof mi;
                    mi.fMask = MIIM_DATA;
                    if (GetMenuItemInfoA(mm, (UINT)pos, TRUE, &mi) && mi.dwItemData == (ULONG_PTR)&g_w98m[i])
                        return MAKELRESULT(pos, MNC_EXECUTE);
                }
            }
        }
        return MAKELRESULT(0, MNC_IGNORE);
    }
    }
    return DefWindowProcA(h, m, w, l);
}

int w98_track_popup(HMENU menu, UINT flags, int x, int y, HWND owner)
{
    static HWND mo = NULL;
    static HBRUSH face = NULL;
    int i, n, cmd;
    MENUINFO inf;
    (void)owner;
    if (!menu) return 0;
    if (!g_w98m_font)
        g_w98m_font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS,
                                  "MS Sans Serif");
    if (!face) face = CreateSolidBrush(C_FACE);
    if (!mo || !IsWindow(mo)) {
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = W98MenuOwnerProc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = W98M_CLASS;
        RegisterClassA(&wc);
        mo = CreateWindowExA(WS_EX_TOOLWINDOW, W98M_CLASS, "", WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, GetModuleHandleA(NULL), NULL);
        if (!mo) return 0;
    }
    /* the items become owner-draw for this one popup */
    g_w98m_n = 0;
    n = GetMenuItemCount(menu);
    for (i = 0; i < n && g_w98m_n < W98M_MAX; i++) {
        MENUITEMINFOA mi;
        char text[128], *tab;
        W98MItem *e = &g_w98m[g_w98m_n];
        ZeroMemory(&mi, sizeof mi);
        mi.cbSize = sizeof mi;
        mi.fMask = MIIM_FTYPE | MIIM_STRING;
        mi.dwTypeData = text;
        mi.cch = sizeof text;
        text[0] = 0;
        if (!GetMenuItemInfoA(menu, (UINT)i, TRUE, &mi)) continue;
        memset(e, 0, sizeof *e);
        e->sep = (mi.fType & MFT_SEPARATOR) != 0;
        if (!e->sep) {
            tab = strchr(text, '\t');
            if (tab) { *tab = 0; snprintf(e->key, sizeof e->key, "%s", tab + 1); }
            snprintf(e->text, sizeof e->text, "%s", text);
        }
        g_w98m_n++;
        mi.fMask = MIIM_FTYPE | MIIM_DATA;
        mi.fType |= MFT_OWNERDRAW;
        mi.dwItemData = (ULONG_PTR)e;
        SetMenuItemInfoA(menu, (UINT)i, TRUE, &mi);
    }
    ZeroMemory(&inf, sizeof inf);
    inf.cbSize = sizeof inf;
    inf.fMask = MIM_BACKGROUND;
    inf.hbrBack = face;
    SetMenuInfo(menu, &inf);
    w31_menu_set_w98(menu);
    cmd = (int)TrackPopupMenu(menu, flags | TPM_RETURNCMD, x, y, 0, mo, NULL);
    w31_menu_set_w98(NULL);
    g_w98m_n = 0;                    /* the caller destroys the menu */
    return cmd;
}

int w98_dialog(HWND owner, const w98_dialog_t *d)
{
    static int registered = 0;
    W98 s;
    HINSTANCE hi = GetModuleHandleA(NULL);
    int x = 0, y = 0, i;
    MSG msg;
    int quit = 0;
    WPARAM quit_code = 0;

    if (!d || d->buttons_n < 1) return 0;
    owner = owner ? GetAncestor(owner, GA_ROOT) : NULL;
    if (!registered) {
        WNDCLASSA wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc   = W98Proc;
        wc.hInstance     = hi;
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.lpszClassName = W98_CLASS;
        registered = RegisterClassA(&wc) != 0;
    }
    memset(&s, 0, sizeof s);
    s.d = *d;
    if (s.d.buttons_n > W98_MAX_BUTTONS) s.d.buttons_n = W98_MAX_BUTTONS;
    s.press = -1;
    s.active = 1;
    /* MS Sans Serif 8: the raster face Windows 98 dialogs used. */
    s.font = CreateFontA(-11, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "MS Sans Serif");
    s.bold = CreateFontA(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, "MS Sans Serif");
    s.focus = 0;
    for (i = 0; i < s.d.buttons_n; i++)
        if (s.d.buttons[i].id == s.d.default_id && s.d.buttons[i].enabled) s.focus = i;
    if (s.d.layout == W98_LAYOUT_CHECKLIST) s.focus = LIST_FOCUS;     /* 2026-11-26 */
    layout(&s);
    suite_center_popup(owner, s.w, s.h, &x, &y);

    s.hwnd = CreateWindowExA(0, W98_CLASS, s.d.caption ? s.d.caption : "",
                             WS_POPUP | WS_CLIPSIBLINGS, x, y, s.w, s.h, owner, NULL, hi, &s);
    if (!s.hwnd) {
        DeleteObject(s.font); DeleteObject(s.bold);
        return s.d.cancel_id;
    }
    {   /* no Windows 11 rounding, border or shadow: the frame is ours */
        typedef HRESULT (WINAPI *PFN_DWMSA)(HWND, DWORD, LPCVOID, DWORD);
        HMODULE dwm = LoadLibraryA("dwmapi.dll");
        PFN_DWMSA dsa = dwm ? (PFN_DWMSA)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        if (dsa) {
            DWORD pol = 1, corner = 1, nocol = 0xFFFFFFFE;
            dsa(s.hwnd, 2, &pol, sizeof pol);
            dsa(s.hwnd, 33, &corner, sizeof corner);
            dsa(s.hwnd, 34, &nocol, sizeof nocol);
        }
    }
    /* for the gates: the kind and each button's centre (client coords) */
    SetPropA(s.hwnd, "NavW98.Kind", (HANDLE)(INT_PTR)(s.d.kind ? s.d.kind : 1));
    for (i = 0; i < s.d.buttons_n; i++) {
        char k[32];
        snprintf(k, sizeof k, "NavW98.Btn%d", s.d.buttons[i].id);
        SetPropA(s.hwnd, k, (HANDLE)(INT_PTR)MAKELONG((s.btn[i].left + s.btn[i].right) / 2,
                                                       (s.btn[i].top + s.btn[i].bottom) / 2));
    }

    if (s.d.layout == W98_LAYOUT_CHECKLIST) {
        /* for the gates: the first row's check box centre and the row step */
        SetPropA(s.hwnd, "NavW98.List", (HANDLE)(INT_PTR)MAKELONG(s.list_rc.left + 2 + 2 + 6,
                                                                     s.list_rc.top + 2 + 2 + 6));
        SetPropA(s.hwnd, "NavW98.ListRowH", (HANDLE)(INT_PTR)LIST_ROW_H);
    }
    /* part 8: the text box, in the dialog's own face, inside its frame */
    if (s.d.edit_field >= 1 && s.d.edit_field <= s.d.fields_n && s.d.layout == W98_LAYOUT_COLUMN) {
        RECT e = s.edit_rc;
        s.edit = CreateWindowExA(0, "EDIT", s.d.edit_buf ? s.d.edit_buf : "",
                                 WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                 e.left + 3, e.top + 3, (e.right - e.left) - 5, (e.bottom - e.top) - 5,
                                 s.hwnd, (HMENU)(INT_PTR)100, hi, NULL);
        if (s.edit) {
            SendMessageA(s.edit, WM_SETFONT, (WPARAM)s.font, FALSE);
            SendMessageA(s.edit, EM_LIMITTEXT, (WPARAM)(s.d.edit_cap > 1 ? s.d.edit_cap - 1 : 1023), 0);
            g_edit_proc0 = (WNDPROC)SetWindowLongPtrA(s.edit, GWLP_WNDPROC, (LONG_PTR)W98EditProc);
        }
    }
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(s.hwnd, SW_SHOW);
    SetForegroundWindow(s.hwnd);
    SetActiveWindow(s.hwnd);
    if (s.edit) {
        SetFocus(s.edit);
        SendMessageA(s.edit, EM_SETSEL, 0, (LPARAM)-1);
    } else {
        SetFocus(s.hwnd);
    }
    while (!s.done) {
        BOOL r = GetMessageA(&msg, NULL, 0, 0);
        if (r == 0) { quit = 1; quit_code = msg.wParam; break; }
        if (r < 0) break;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (!s.done) s.result = s.d.cancel_id;
    if (s.edit && s.d.edit_buf && s.d.edit_cap > 0)
        GetWindowTextA(s.edit, s.d.edit_buf, s.d.edit_cap);
    /* the owner first, so activation goes back to it, not elsewhere */
    if (owner) EnableWindow(owner, TRUE);
    for (i = 0; i < s.d.buttons_n; i++) {
        char k[32];
        snprintf(k, sizeof k, "NavW98.Btn%d", s.d.buttons[i].id);
        RemovePropA(s.hwnd, k);
    }
    RemovePropA(s.hwnd, "NavW98.Kind");
    if (s.d.layout == W98_LAYOUT_CHECKLIST) {
        RemovePropA(s.hwnd, "NavW98.List");
        RemovePropA(s.hwnd, "NavW98.ListRowH");
        RemovePropA(s.hwnd, "NavW98.Sel");
        for (i = 0; i < s.d.buttons_n; i++) {
            char k[32];
            snprintf(k, sizeof k, "NavW98.En%d", s.d.buttons[i].id);
            RemovePropA(s.hwnd, k);
        }
    }
    DestroyWindow(s.hwnd);
    if (owner) SetActiveWindow(owner);
    DeleteObject(s.font);
    DeleteObject(s.bold);
    if (quit) PostQuitMessage((int)quit_code);
    return s.result;
}
