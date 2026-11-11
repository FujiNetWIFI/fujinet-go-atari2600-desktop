/*
 * The debugger's Video tab (Win32).
 *
 * Everything that decides what to show lives in the core (a2600vidtab.h): this file is
 * controls and a GDI picture. The picture is the controller's cached 0xFFRRGGBB buffer drawn
 * 1:1 with StretchDIBits (BI_RGB ignores the alpha byte), double buffered; a mouse move
 * redraws only the marks. Only the section showing is filled in.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dbg_video.h"

#include <commctrl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* <dlgs.h> (through <windows.h>) defines grp1 as a dialog control id; the TIA has a register of that name */
#undef grp1

#include "a2600vidtab.h"

#define PAGE_CLASS L"FujiNetGoA2600VideoPage"
#define PIC_CLASS L"FujiNetGoA2600VideoPic"
#define COL_CLASS L"FujiNetGoA2600VideoColours"

#define ACCENT RGB(0xFF, 0xA6, 0x45)      /* A2600SESSION_ACCENT_RGB */
#define MAX_OVERLAYS 16

enum {
    IDV_VIEW = 3000, IDV_SOURCE, IDV_LABEL_SOURCE, IDV_ZOOM, IDV_BREAK, IDV_BREAK_REG, IDV_STATUS, IDV_NOTE, IDV_SEC, IDV_PIXEL, IDV_UNPIN,
    IDV_REGS, IDV_LINES, IDV_WRITES, IDV_BREAK_WRITE, IDV_BEAM, IDV_TIMING, IDV_PIC, IDV_COLS, IDV_LABEL_VIEW, IDV_LABEL_ZOOM,
    IDV_OV0 = 3200
};

#define MAX_ZOOMS 16

typedef struct {
    unsigned char *hot;
    int nhot;
    uint64_t sig;
    int have_sig;
} listfill;

typedef struct {
    HWND page, view, zoom, brk, status, note, pic, sec;
    HWND ov[MAX_OVERLAYS];
    unsigned ov_bit[MAX_OVERLAYS];
    int nov;
    HWND label_view, label_zoom, label_source, source, brk_reg;
    HWND pixel, unpin, regs, lines, writes, brk_write, cols, beam, timing;
    int reg_selected;              /* the swatch Break on register write acts on, -1 none */
    HFONT ui, mono;
    int dpi;
    a2600vid_tab *tab;
    int updating;
    int scroll_x, scroll_y;
    int tracking;
    int pal_hover, col_tracking;
    int zoom_val[MAX_ZOOMS], nzoom;
    listfill rf, lf, wf;
    BITMAPINFO bmi;
} vidwin;

#define S(v, n) MulDiv((n), (v)->dpi, 96)

/* ---- text ------------------------------------------------------------------------ */

/* UTF-8 to UTF-16; '\n' becomes CR LF (multi-line controls want it) */
static wchar_t *wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0), i;
    wchar_t *w = malloc(((size_t)n + 1) * sizeof *w), *o, *d;
    if (!w) return NULL;
    o = malloc(((size_t)n * 2 + 2) * sizeof *w);
    if (!o) {
        free(w);
        return NULL;
    }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
    d = o;
    for (i = 0; w[i]; i++) {
        if (w[i] == L'\n') *d++ = L'\r';
        *d++ = w[i];
    }
    *d = 0;
    free(w);
    return o;
}

/* a control's text, set only when it changed (no flicker on the live refresh) */
static void set_text(HWND h, const char *utf8)
{
    wchar_t *w = wide(utf8), cur[2048];
    if (!w) return;
    GetWindowTextW(h, cur, 2048);
    if (wcscmp(cur, w) != 0) SetWindowTextW(h, w);
    free(w);
}

static uint64_t fnv(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

/* ---- the picture --------------------------------------------------------------------- */

static void pic_update_scroll(vidwin *v)
{
    RECT rc;
    SCROLLINFO si;
    int pw = 0, ph = 0;
    GetClientRect(v->pic, &rc);
    a2600vid_tab_picture(v->tab, &pw, &ph);
    memset(&si, 0, sizeof si);
    si.cbSize = sizeof si;
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = pw > 0 ? pw - 1 : 0;
    si.nPage = (UINT)rc.right;
    if (v->scroll_x > pw - rc.right) v->scroll_x = pw > rc.right ? pw - rc.right : 0;
    si.nPos = v->scroll_x;
    SetScrollInfo(v->pic, SB_HORZ, &si, TRUE);
    si.nMax = ph > 0 ? ph - 1 : 0;
    si.nPage = (UINT)rc.bottom;
    if (v->scroll_y > ph - rc.bottom) v->scroll_y = ph > rc.bottom ? ph - rc.bottom : 0;
    si.nPos = v->scroll_y;
    SetScrollInfo(v->pic, SB_VERT, &si, TRUE);
}

static void pic_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    RECT rc;
    HBITMAP bmp, old;
    int pw = 0, ph = 0, marks, hover[4] = { 0 }, pin[4] = { 0 };
    const uint32_t *px;
    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 24));
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, bg);
    px = a2600vid_tab_picture(v->tab, &pw, &ph);
    if (!px) {
        const char *why = a2600vid_tab_why(v->tab);
        wchar_t *w = wide(why && *why ? why : "Nothing to show");
        RECT tr = rc;
        InflateRect(&tr, -S(v, 10), -S(v, 10));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(140, 140, 148));
        SelectObject(mem, v->ui);
        if (w) DrawTextW(mem, w, -1, &tr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        free(w);
    } else {
        HPEN pen_h, pen_p, pen_k, oldpen;
        HGDIOBJ oldbr;
        v->bmi.bmiHeader.biWidth = pw;      /* the buffer is pw words per row, whatever the view */
        v->bmi.bmiHeader.biHeight = -ph;    /* top-down */
        SetStretchBltMode(mem, COLORONCOLOR);
        StretchDIBits(mem, -v->scroll_x, -v->scroll_y, pw, ph, 0, 0, pw, ph, px, &v->bmi, DIB_RGB_COLORS, SRCCOPY);
        marks = a2600vid_tab_marks(v->tab, hover, pin);
        oldbr = SelectObject(mem, GetStockObject(NULL_BRUSH));
        pen_h = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
        pen_p = CreatePen(PS_SOLID, 2, RGB(255, 217, 26));
        pen_k = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        if (marks & 1) {            /* hover: a white box just outside the pixel, over a dark one */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, hover[0] - v->scroll_x - 2, hover[1] - v->scroll_y - 2, hover[0] - v->scroll_x + hover[2] + 2,
                      hover[1] - v->scroll_y + hover[3] + 2);
            SelectObject(mem, pen_h);
            Rectangle(mem, hover[0] - v->scroll_x - 1, hover[1] - v->scroll_y - 1, hover[0] - v->scroll_x + hover[2] + 1,
                      hover[1] - v->scroll_y + hover[3] + 1);
            SelectObject(mem, oldpen);
        }
        if (marks & 2) {            /* pin: a thick yellow box */
            oldpen = SelectObject(mem, pen_k);
            Rectangle(mem, pin[0] - v->scroll_x - 3, pin[1] - v->scroll_y - 3, pin[0] - v->scroll_x + pin[2] + 3,
                      pin[1] - v->scroll_y + pin[3] + 3);
            SelectObject(mem, pen_p);
            Rectangle(mem, pin[0] - v->scroll_x - 1, pin[1] - v->scroll_y - 1, pin[0] - v->scroll_x + pin[2] + 1,
                      pin[1] - v->scroll_y + pin[3] + 1);
            SelectObject(mem, oldpen);
        }
        SelectObject(mem, oldbr);
        DeleteObject(pen_h);
        DeleteObject(pen_p);
        DeleteObject(pen_k);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    DeleteObject(bg);
    EndPaint(h, &ps);
}

static void update_pixel(vidwin *v)
{
    const a2600vid_pixel *p = a2600vid_tab_pixel(v->tab);
    set_text(v->pixel, p && p->text[0] ? p->text
                                       : "Move over the picture to inspect a pixel.\n"
                                         "Click to pin it; click again to unpin.\n"
                                         "Double-click to break on writes to it.");
    if (v->cols) InvalidateRect(v->cols, NULL, FALSE);
}

static void set_note(vidwin *v, const char *text)
{
    set_text(v->note, text);
}

/* Break on writes to the inspected pixel's registers, and say what was done. */
static void break_on_pixel(vidwin *v)
{
    char msg[160];
    int id = a2600vid_tab_break_on_pixel(v->tab);
    if (id >= 0) snprintf(msg, sizeof msg, "Break on writes to the pixel's colour, graphics and position registers (see Breakpoints)");
    else snprintf(msg, sizeof msg, "Nothing to break on: pin or hover a pixel inside the rows the TIA draws");
    set_note(v, msg);
}

static LRESULT CALLBACK pic_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        pic_paint(v, h);
        return 0;
    case WM_SIZE:
        pic_update_scroll(v);
        return 0;
    case WM_HSCROLL:
    case WM_VSCROLL: {
        SCROLLINFO si;
        int bar = msg == WM_HSCROLL ? SB_HORZ : SB_VERT, pos;
        memset(&si, 0, sizeof si);
        si.cbSize = sizeof si;
        si.fMask = SIF_ALL;
        GetScrollInfo(h, bar, &si);
        pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= 16; break;
        case SB_LINERIGHT: pos += 16; break;
        case SB_PAGELEFT: pos -= (int)si.nPage; break;
        case SB_PAGERIGHT: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: break;
        }
        if (pos < 0) pos = 0;
        if (bar == SB_HORZ) v->scroll_x = pos; else v->scroll_y = pos;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        v->scroll_y -= GET_WHEEL_DELTA_WPARAM(wp) / 3;
        if (v->scroll_y < 0) v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        int x = (short)LOWORD(lp) + v->scroll_x, y = (short)HIWORD(lp) + v->scroll_y;
        if (!v->tracking) {
            TRACKMOUSEEVENT t;
            memset(&t, 0, sizeof t);
            t.cbSize = sizeof t;
            t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
            TrackMouseEvent(&t);
            v->tracking = 1;
        }
        if (a2600vid_tab_hover(v->tab, x, y)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v->tracking = 0;
        if (a2600vid_tab_hover(v->tab, -1, -1)) {
            InvalidateRect(h, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(h);
        a2600vid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        a2600vid_tab_pin(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    case WM_LBUTTONDBLCLK:
        a2600vid_tab_hover(v->tab, (short)LOWORD(lp) + v->scroll_x, (short)HIWORD(lp) + v->scroll_y);
        break_on_pixel(v);
        InvalidateRect(h, NULL, FALSE);
        update_pixel(v);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- the Colours panel: the seven objects' colour registers, a description and Break on register write ---- */

static int col_row_h(vidwin *v) { return S(v, 26); }

/* "P0 (COLUP0 $06): $1E  #RRGGBB  N pixels" */
static void swatch_describe(const a2600vid_swatch *swatch, char *out, size_t cap)
{
    snprintf(out, cap, "%s (%s $%02X): $%02X  #%06X  %ld pixels%s", swatch->name, swatch->reg_name, swatch->address,
             swatch->value, swatch->rgb & 0xFFFFFF, swatch->uses, swatch->inspected ? "  - drew the inspected pixel" : "");
}

static void col_paint(vidwin *v, HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    HBITMAP bmp, old;
    RECT rc;
    const a2600vid_swatch *swatches;
    int count = a2600vid_tab_swatches(v->tab, &swatches);
    int index, row_h = col_row_h(v);
    HFONT oldf;
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right > 0 ? rc.right : 1, rc.bottom > 0 ? rc.bottom : 1);
    old = SelectObject(mem, bmp);
    FillRect(mem, &rc, GetSysColorBrush(COLOR_BTNFACE));
    SetBkMode(mem, TRANSPARENT);
    oldf = SelectObject(mem, v->mono);
    SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
    for (index = 0; index < count; index++) {
        const a2600vid_swatch *swatch = &swatches[index];
        RECT cell_rect, text_rect;
        char text[300];
        wchar_t *wide_text;
        HBRUSH fill = CreateSolidBrush(RGB((swatch->rgb >> 16) & 255, (swatch->rgb >> 8) & 255, swatch->rgb & 255));
        cell_rect.left = 3;
        cell_rect.top = index * row_h + 3;
        cell_rect.right = cell_rect.left + row_h - 6;
        cell_rect.bottom = cell_rect.top + row_h - 6;
        FillRect(mem, &cell_rect, fill);
        DeleteObject(fill);
        if (swatch->inspected) {
            HBRUSH accent_brush = CreateSolidBrush(ACCENT);
            RECT outline = cell_rect;
            InflateRect(&outline, 2, 2);
            FrameRect(mem, &outline, accent_brush);
            DeleteObject(accent_brush);
        }
        if (index == v->reg_selected || index == v->pal_hover) {
            HBRUSH edge = CreateSolidBrush(index == v->reg_selected ? ACCENT : RGB(0, 0, 0));
            RECT outline = cell_rect;
            InflateRect(&outline, 3, 3);
            FrameRect(mem, &outline, edge);
            DeleteObject(edge);
        }
        swatch_describe(swatch, text, sizeof text);
        text_rect.left = cell_rect.right + S(v, 8);
        text_rect.top = index * row_h;
        text_rect.right = rc.right;
        text_rect.bottom = index * row_h + row_h;
        wide_text = wide(text);
        if (wide_text) DrawTextW(mem, wide_text, -1, &text_rect, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        free(wide_text);
    }
    {
        RECT text_rect = { 0, count * row_h + S(v, 6), rc.right, rc.bottom };
        wchar_t *wide_text = wide("Hover or click an object's colour register; Break on register write then traps writes to it.");
        if (wide_text) DrawTextW(mem, wide_text, -1, &text_rect, DT_WORDBREAK | DT_NOPREFIX);
        free(wide_text);
    }
    SelectObject(mem, oldf);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(h, &ps);
}

/* the swatch row under a point of the panel, or -1 */
static int col_hit(vidwin *v, int py)
{
    const a2600vid_swatch *swatches;
    int count = a2600vid_tab_swatches(v->tab, &swatches), row = py < 0 ? -1 : py / col_row_h(v);
    return row >= 0 && row < count ? row : -1;
}

static LRESULT CALLBACK col_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        col_paint(v, h);
        return 0;
    case WM_LBUTTONDOWN: {
        int index = col_hit(v, (short)HIWORD(lp));
        v->reg_selected = index == v->reg_selected ? -1 : index;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int index = col_hit(v, (short)HIWORD(lp));
        if (!v->col_tracking) {
            TRACKMOUSEEVENT track;
            memset(&track, 0, sizeof track);
            track.cbSize = sizeof track;
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = h;
            TrackMouseEvent(&track);
            v->col_tracking = 1;
        }
        if (index != v->pal_hover) {
            v->pal_hover = index;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        v->col_tracking = 0;
        if (v->pal_hover != -1) {
            v->pal_hover = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* Break on a write to the selected object's colour register (or the inspected pixel's own), and say what was done. */
static void break_on_cram(vidwin *v)
{
    char msg[160];
    int index = v->reg_selected >= 0 ? v->reg_selected : a2600vid_tab_pixel_object(v->tab);
    int id = index >= 0 ? a2600vid_tab_break_on_swatch(v->tab, index) : -1;
    if (id >= 0) snprintf(msg, sizeof msg, "Write trap on the colour register of object %d added (see Breakpoints)", index);
    else snprintf(msg, sizeof msg, "Select an object (or pin a pixel) first");
    set_note(v, msg);
}

/* ---- sections ----------------------------------------------------------------------- */

static void lv_cell(HWND lv, int row, int col, const char *text)
{
    wchar_t *w = wide(text);
    LVITEMW it;
    memset(&it, 0, sizeof it);
    it.iSubItem = col;
    it.pszText = w ? w : L"";
    if (col == 0) {
        it.mask = LVIF_TEXT;
        it.iItem = row;
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    } else {
        SendMessageW(lv, LVM_SETITEMTEXTW, (WPARAM)row, (LPARAM)&it);
    }
    free(w);
}

static void mark_hot(listfill *lf, int n, int i, int hot)
{
    if (i == 0) {
        free(lf->hot);
        lf->hot = calloc((size_t)(n > 0 ? n : 1), 1);
        lf->nhot = lf->hot ? n : 0;
    }
    if (lf->hot && i < lf->nhot) lf->hot[i] = (unsigned char)hot;
}

static void fill_regs(vidwin *v)
{
    const a2600vid_reg *r;
    int n = a2600vid_tab_regs(v->tab, &r), i;
    uint64_t sig = 1469598103934665603ull;
    for (i = 0; i < n; i++) {
        int hot = r[i].age < A2600VID_REG_FADE;
        sig = fnv(sig, r[i].name, strlen(r[i].name));
        sig = fnv(sig, r[i].value, strlen(r[i].value));
        sig = fnv(sig, r[i].meaning, strlen(r[i].meaning));
        sig = fnv(sig, &hot, sizeof hot);
    }
    if (v->rf.have_sig && v->rf.sig == sig) return;
    v->rf.sig = sig;
    v->rf.have_sig = 1;
    SendMessageW(v->regs, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->regs, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < n; i++) {
        lv_cell(v->regs, i, 0, r[i].name);
        lv_cell(v->regs, i, 1, r[i].value);
        lv_cell(v->regs, i, 2, r[i].meaning);
        mark_hot(&v->rf, n, i, r[i].age < A2600VID_REG_FADE);
    }
    if (n == 0) mark_hot(&v->rf, 0, 0, 0);
    SendMessageW(v->regs, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->regs, NULL, TRUE);
}

static void fill_lines(vidwin *v)
{
    const a2600vid_line_row *r;
    int n = a2600vid_tab_lines(v->tab, &r), i;
    uint64_t sig = 1469598103934665603ull;
    for (i = 0; i < n; i++) {
        sig = fnv(sig, &r[i], sizeof r[i]);
    }
    sig = fnv(sig, &n, sizeof n);
    if (v->lf.have_sig && v->lf.sig == sig) return;
    v->lf.sig = sig;
    v->lf.have_sig = 1;
    SendMessageW(v->lines, WM_SETREDRAW, FALSE, 0);
    SendMessageW(v->lines, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < n; i++) {
        char text[64];
        int col = 0;
        snprintf(text, sizeof text, "%d", r[i].row);
        lv_cell(v->lines, i, col++, text);
        snprintf(text, sizeof text, "%d", r[i].scanline);
        lv_cell(v->lines, i, col++, text);
        snprintf(text, sizeof text, "%02X %02X %02X %02X", r[i].colubk, r[i].colupf, r[i].colup0, r[i].colup1);
        lv_cell(v->lines, i, col++, text);
        snprintf(text, sizeof text, "%02X %02X %02X %02X", r[i].pf0, r[i].pf1, r[i].pf2, r[i].ctrlpf);
        lv_cell(v->lines, i, col++, text);
        snprintf(text, sizeof text, "%02X %02X", r[i].grp0, r[i].grp1);
        lv_cell(v->lines, i, col++, text);
        snprintf(text, sizeof text, "%02X %02X", r[i].nusiz0, r[i].nusiz1);
        lv_cell(v->lines, i, col++, text);
        snprintf(text, sizeof text, "%d%d%d", r[i].enables & 1, (r[i].enables >> 1) & 1, (r[i].enables >> 2) & 1);
        lv_cell(v->lines, i, col++, text);
        snprintf(text, sizeof text, "%3d %3d %3d %3d %3d", r[i].pos[A2600VID_P0], r[i].pos[A2600VID_P1], r[i].pos[A2600VID_M0],
                 r[i].pos[A2600VID_M1], r[i].pos[A2600VID_BL]);
        lv_cell(v->lines, i, col++, text);
        snprintf(text, sizeof text, "%d", r[i].writes);
        lv_cell(v->lines, i, col++, text);
        mark_hot(&v->lf, n, i, r[i].changed_mask != 0);
    }
    if (n == 0) mark_hot(&v->lf, 0, 0, 0);
    SendMessageW(v->lines, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(v->lines, NULL, TRUE);
}

/* The Writes list is virtual (a frame holds thousands of writes): it only learns how many rows there are, and
 * LVN_GETDISPINFO asks the controller for each cell as it is drawn. */
static void fill_writes(vidwin *v)
{
    const a2600vid_write *r;
    int n = a2600vid_tab_writes(v->tab, &r), i;
    uint64_t sig = 1469598103934665603ull;
    for (i = 0; i < n; i++) sig = fnv(sig, &r[i], sizeof r[i]);
    sig = fnv(sig, &n, sizeof n);
    if (v->wf.have_sig && v->wf.sig == sig) return;
    v->wf.sig = sig;
    v->wf.have_sig = 1;
    SendMessageW(v->writes, LVM_SETITEMCOUNT, (WPARAM)n, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    InvalidateRect(v->writes, NULL, TRUE);
}

static void writes_dispinfo(vidwin *v, NMLVDISPINFOW *di)
{
    const a2600vid_write *r;
    const int n = a2600vid_tab_writes(v->tab, &r);
    char text[64];
    wchar_t *w;
    static wchar_t cell[64];
    if (!(di->item.mask & LVIF_TEXT) || di->item.iItem < 0 || di->item.iItem >= n) return;
    r += di->item.iItem;
    text[0] = 0;
    switch (di->item.iSubItem) {
    case 0: snprintf(text, sizeof text, "%s", r->name); break;
    case 1: snprintf(text, sizeof text, "$%02X", r->value); break;
    case 2: snprintf(text, sizeof text, "$%02X", r->old); break;
    case 3: snprintf(text, sizeof text, "%d", r->clock); break;
    case 4: if (r->row >= 0) snprintf(text, sizeof text, "%d", r->row); else snprintf(text, sizeof text, "-"); break;
    case 5: if (r->scanline >= 0) snprintf(text, sizeof text, "%d", r->scanline); else snprintf(text, sizeof text, "-"); break;
    default: break;
    }
    w = wide(text);
    if (w) {
        wcsncpy(cell, w, 63);
        cell[63] = 0;
        free(w);
    }
    di->item.pszText = cell;
}

/* Break on writes to the selected row's TIA register */
static void break_on_write_row(vidwin *v)
{
    const a2600vid_write *r;
    int n = a2600vid_tab_writes(v->tab, &r), sel = (int)SendMessageW(v->writes, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    char msg[160];
    if (sel >= 0 && sel < n && a2600vid_tab_break_on_register(v->tab, r[sel].reg) == 0)
        snprintf(msg, sizeof msg, "Write trap on %s added (see Breakpoints)", r[sel].name);
    else snprintf(msg, sizeof msg, "Select a write in the list first");
    set_note(v, msg);
}

static void fill_timing(vidwin *v)
{
    const char *const *lines;
    int n = a2600vid_tab_timing(v->tab, &lines), i;
    char buf[2048];
    size_t len = 0;
    buf[0] = 0;
    for (i = 0; i < n && len < sizeof buf - 160; i++) {
        const char *tab = strchr(lines[i], '\t');
        if (tab) len += (size_t)snprintf(buf + len, sizeof buf - len, "%-14.*s %s\n", (int)(tab - lines[i]), lines[i], tab + 1);
        else len += (size_t)snprintf(buf + len, sizeof buf - len, "%s\n", lines[i]);
    }
    if (!n) snprintf(buf, sizeof buf, "No machine");
    set_text(v->timing, buf);
    SendMessageW(v->beam, PBM_SETPOS, (WPARAM)a2600vid_tab_beam_permille(v->tab), 0);
}

static int cur_section(vidwin *v)
{
    int s = (int)SendMessageW(v->sec, TCM_GETCURSEL, 0, 0);
    return s < 0 ? 0 : s;
}

static void show_section(vidwin *v)
{
    int s = cur_section(v);
    ShowWindow(v->pixel, s == A2600VID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->unpin, s == A2600VID_S_PIXEL ? SW_SHOW : SW_HIDE);
    ShowWindow(v->regs, s == A2600VID_S_REGISTERS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->cols, s == A2600VID_S_COLOURS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->brk_reg, s == A2600VID_S_COLOURS ? SW_SHOW : SW_HIDE);
    ShowWindow(v->lines, s == A2600VID_S_LINES ? SW_SHOW : SW_HIDE);
    ShowWindow(v->writes, s == A2600VID_S_WRITES ? SW_SHOW : SW_HIDE);
    ShowWindow(v->brk_write, s == A2600VID_S_WRITES ? SW_SHOW : SW_HIDE);
    ShowWindow(v->beam, s == A2600VID_S_TIMING ? SW_SHOW : SW_HIDE);
    ShowWindow(v->timing, s == A2600VID_S_TIMING ? SW_SHOW : SW_HIDE);
}

/* ---- refresh -------------------------------------------------------------------------- */

#define R_RESET 0x8000u         /* the view or zoom changed: scroll back to the corner */

static void refresh_view(vidwin *v, unsigned what)
{
    if (what & (A2600VID_TAB_PICTURE | A2600VID_TAB_OFF)) {
        RECT rc;
        GetClientRect(v->pic, &rc);
        if (rc.right > 4 && rc.bottom > 4) a2600vid_tab_set_viewport(v->tab, rc.right - 2, rc.bottom - 2);
        if (what & R_RESET) v->scroll_x = v->scroll_y = 0;
        pic_update_scroll(v);
        InvalidateRect(v->pic, NULL, FALSE);
    }
    if (what & A2600VID_TAB_TABLES) {
        set_text(v->status, a2600vid_tab_status(v->tab));
        switch (cur_section(v)) {
        case A2600VID_S_REGISTERS: fill_regs(v); break;
        case A2600VID_S_COLOURS: InvalidateRect(v->cols, NULL, FALSE); break;
        case A2600VID_S_LINES: fill_lines(v); break;
        case A2600VID_S_WRITES: fill_writes(v); break;
        case A2600VID_S_TIMING: fill_timing(v); break;
        default: break;
        }
        update_pixel(v);
    }
}

void dbg_video_refresh(HWND page, int force)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    unsigned what;
    if (!v) return;
    what = a2600vid_tab_refresh(v->tab, force);
    if (force) {
        what |= A2600VID_TAB_PICTURE | A2600VID_TAB_TABLES;
        v->rf.have_sig = v->lf.have_sig = v->wf.have_sig = 0;
    }
    refresh_view(v, what);
}

/* ---- layout --------------------------------------------------------------------------- */

static int text_w(vidwin *v, HFONT f, const wchar_t *s)
{
    HDC dc = GetDC(v->page);
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz;
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, old);
    ReleaseDC(v->page, dc);
    return sz.cx;
}

static void layout(vidwin *v)
{
    RECT rc, tr;
    int W, H, rw, lw, x, y, i, row_h = S(v, 24), lh = S(v, 18), gap = S(v, 6), mono_h;
    int right_x, tab_h, top;
    TEXTMETRICW tm;
    HDC dc;
    GetClientRect(v->page, &rc);
    W = rc.right;
    H = rc.bottom;
    if (W < 100 || H < 100) return;
    dc = GetDC(v->page);
    SelectObject(dc, v->mono);
    mono_h = GetTextMetricsW(dc, &tm) ? tm.tmHeight : S(v, 15);
    ReleaseDC(v->page, dc);

    /* toolbar: View, Zoom, Break on writes; then the overlay checkboxes, flowing */
    x = 0;
    y = 0;
    MoveWindow(v->label_view, x, y + (row_h - lh) / 2, S(v, 36), lh, TRUE);
    x += S(v, 36);
    MoveWindow(v->view, x, y, S(v, 130), S(v, 300), TRUE);
    x += S(v, 130) + gap * 2;
    MoveWindow(v->label_source, x, y + (row_h - lh) / 2, S(v, 50), lh, TRUE);
    x += S(v, 50);
    MoveWindow(v->source, x, y, S(v, 130), S(v, 300), TRUE);
    x += S(v, 130) + gap * 2;
    MoveWindow(v->label_zoom, x, y + (row_h - lh) / 2, S(v, 40), lh, TRUE);
    x += S(v, 40);
    MoveWindow(v->zoom, x, y, S(v, 80), S(v, 300), TRUE);
    x += S(v, 80) + gap * 3;
    {
        int bw = text_w(v, v->ui, L"Break on writes") + S(v, 28);
        MoveWindow(v->brk, x, y - S(v, 1), bw, row_h, TRUE);
    }
    y += row_h + gap;
    x = 0;
    for (i = 0; i < v->nov; i++) {
        char dummy[1];
        wchar_t lbl[64];
        int bw;
        (void)dummy;
        GetWindowTextW(v->ov[i], lbl, 64);
        bw = text_w(v, v->ui, lbl) + S(v, 26);
        if (x + bw > W && x > 0) {
            x = 0;
            y += row_h - S(v, 2);
        }
        MoveWindow(v->ov[i], x, y, bw, row_h - S(v, 4), TRUE);
        x += bw + gap;
    }
    y += row_h;
    MoveWindow(v->status, 0, y, W, mono_h * 2 + S(v, 4), TRUE);
    y += mono_h * 2 + S(v, 8);
    top = y;

    /* the split: picture left, sections right */
    rw = S(v, 480);
    if (rw > W * 50 / 100) rw = W * 48 / 100;
    lw = W - rw - gap;
    right_x = lw + gap;
    MoveWindow(v->pic, 0, top, lw, H - top - lh - gap > 20 ? H - top - lh - gap : 20, TRUE);
    MoveWindow(v->note, 0, H - lh, W, lh, TRUE);

    MoveWindow(v->sec, right_x, top, rw, H - top - lh - gap, TRUE);
    SetRect(&tr, 0, 0, rw, H - top - lh - gap);
    SendMessageW(v->sec, TCM_ADJUSTRECT, FALSE, (LPARAM)&tr);
    tab_h = tr.top;
    {
        int cx = right_x + S(v, 6), cy = top + tab_h + S(v, 6), cw = rw - S(v, 12);
        int ch = (H - lh - gap) - cy - S(v, 6);
        MoveWindow(v->pixel, cx, cy, cw, ch - S(v, 34) > 20 ? ch - S(v, 34) : 20, TRUE);
        MoveWindow(v->unpin, cx, cy + (ch > S(v, 60) ? ch - S(v, 28) : S(v, 32)), S(v, 80), S(v, 26), TRUE);
        MoveWindow(v->regs, cx, cy, cw, ch, TRUE);
        MoveWindow(v->cols, cx, cy, cw, ch - S(v, 34) > 20 ? ch - S(v, 34) : 20, TRUE);
        MoveWindow(v->brk_reg, cx, cy + (ch > S(v, 60) ? ch - S(v, 28) : S(v, 32)), S(v, 150), S(v, 26), TRUE);
        MoveWindow(v->lines, cx, cy, cw, ch, TRUE);
        MoveWindow(v->writes, cx, cy, cw, ch - S(v, 34) > 20 ? ch - S(v, 34) : 20, TRUE);
        MoveWindow(v->brk_write, cx, cy + (ch > S(v, 60) ? ch - S(v, 28) : S(v, 32)), S(v, 150), S(v, 26), TRUE);
        MoveWindow(v->beam, cx, cy, cw, S(v, 20), TRUE);
        MoveWindow(v->timing, cx, cy + S(v, 30), cw, ch - S(v, 30), TRUE);
        SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 0, S(v, 80));
        SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 1, S(v, 60));
        SendMessageW(v->regs, LVM_SETCOLUMNWIDTH, 2, S(v, 520));
        {
            static const int lw_[9] = { 36, 40, 112, 130, 90, 74, 66, 160, 44 };
            static const int ww_[6] = { 90, 56, 56, 56, 48, 52 };
            for (i = 0; i < 9; i++) SendMessageW(v->lines, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(v, lw_[i]));
            for (i = 0; i < 6; i++) SendMessageW(v->writes, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(v, ww_[i]));
        }
    }
    pic_update_scroll(v);
}

/* ---- the page -------------------------------------------------------------------------- */

static HWND mk_in(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int id, HFONT font)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id,
                             (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    if (font) SendMessageW(h, WM_SETFONT, (WPARAM)font, TRUE);
    return h;
}

#define mk(v, cls, text, style, id, font) mk_in((v)->page, cls, text, style, id, font)

static void on_overlay(vidwin *v)
{
    unsigned m = 0;
    int i;
    for (i = 0; i < v->nov; i++)
        if (SendMessageW(v->ov[i], BM_GETCHECK, 0, 0) == BST_CHECKED) m |= v->ov_bit[i];
    a2600vid_tab_set_overlays(v->tab, m);
    refresh_view(v, A2600VID_TAB_PICTURE | A2600VID_TAB_TABLES);
}

static LRESULT CALLBACK page_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!v) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        layout(v);
        refresh_view(v, A2600VID_TAB_PICTURE);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (v->updating) return 0;
        if (id == IDV_SOURCE && code == CBN_SELCHANGE) {
            a2600vid_tab_set_source(v->tab, (int)SendMessageW(v->source, CB_GETCURSEL, 0, 0));
            dbg_video_refresh(h, 1);
            v->scroll_x = v->scroll_y = 0;
            pic_update_scroll(v);
        } else if (id == IDV_BREAK_WRITE && code == BN_CLICKED) {
            break_on_write_row(v);
        } else if (id == IDV_BREAK_REG && code == BN_CLICKED) {
            break_on_cram(v);
        } else if (id == IDV_VIEW && code == CBN_SELCHANGE) {
            a2600vid_tab_set_view(v->tab, (int)SendMessageW(v->view, CB_GETCURSEL, 0, 0));
            EnableWindow(v->source, a2600vid_tab_view(v->tab) == A2600VID_V_DECODED);
            dbg_video_refresh(h, 1);
            v->scroll_x = v->scroll_y = 0;
            pic_update_scroll(v);
        } else if (id == IDV_ZOOM && code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(v->zoom, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < v->nzoom) a2600vid_tab_set_zoom(v->tab, v->zoom_val[i]);
            refresh_view(v, A2600VID_TAB_PICTURE | R_RESET);
        } else if (id >= IDV_OV0 && id < IDV_OV0 + v->nov && code == BN_CLICKED) {
            on_overlay(v);
        } else if (id == IDV_BREAK && code == BN_CLICKED) {
            break_on_pixel(v);
        } else if (id == IDV_UNPIN && code == BN_CLICKED) {
            a2600vid_tab_unpin(v->tab);
            InvalidateRect(v->pic, NULL, FALSE);
            update_pixel(v);
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == IDV_WRITES && nh->code == LVN_GETDISPINFOW) {
            writes_dispinfo(v, (NMLVDISPINFOW *)lp);
            return 0;
        }
        if (nh->idFrom == IDV_SEC && nh->code == TCN_SELCHANGE) {
            show_section(v);
            refresh_view(v, A2600VID_TAB_TABLES);
        } else if ((nh->idFrom == IDV_REGS || nh->idFrom == IDV_LINES) && nh->code == NM_CUSTOMDRAW) {
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            listfill *lf = nh->idFrom == IDV_REGS ? &v->rf : &v->lf;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                int i = (int)cd->nmcd.dwItemSpec;
                if (i >= 0 && i < lf->nhot && lf->hot[i]) {
                    if (nh->idFrom == IDV_REGS) {       /* a register that just changed: the accent */
                        cd->clrTextBk = ACCENT;
                        cd->clrText = RGB(0x99, 0x63, 0x28);   /* A2600SESSION_ACCENT_DARK_RGB */
                    } else {                            /* a line where something changed */
                        cd->clrTextBk = RGB(0xf6, 0xd6, 0xd2);
                        cd->clrText = RGB(0, 0, 0);
                    }
                }
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        /* the read-only labels sit on the page colour */
        HDC dc = (HDC)wp;
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_DESTROY:
        a2600vid_tab_free(v->tab);
        free(v->rf.hot);
        free(v->lf.hot);
        free(v->wf.hot);
        free(v);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void register_classes(HINSTANCE inst)
{
    static int done;
    WNDCLASSW wc;
    if (done) return;
    done = 1;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = page_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = PAGE_CLASS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = pic_proc;
    wc.lpszClassName = PIC_CLASS;
    wc.hbrBackground = NULL;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    wc.lpfnWndProc = col_proc;
    wc.lpszClassName = COL_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = 0;
    RegisterClassW(&wc);
}

static HWND mk_list(vidwin *v, int id, const wchar_t *const *names, int n, DWORD extra)
{
    HWND lv = mk(v, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP | extra,
                 id, v->mono);
    LVCOLUMNW col;
    int i;
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                 LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    memset(&col, 0, sizeof col);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    for (i = 0; i < n; i++) {
        col.pszText = (LPWSTR)names[i];
        col.cx = 80;
        SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)i, (LPARAM)&col);
    }
    return lv;
}

HWND dbg_video_create(HWND parent, a2600session *session, HFONT ui, HFONT mono, int dpi)
{
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE);
    vidwin *v = calloc(1, sizeof *v);
    int n, i;
    const a2600vid_overlay_info *od;
    const int *zv;
    TCITEMW item;
    static const wchar_t *const regcols[3] = { L"Register", L"Value", L"Meaning" };
    static const wchar_t *const linecols[9] = { L"Row", L"Line", L"BK PF P0 P1", L"PF0 PF1 PF2 CT", L"GRP0 GRP1", L"NUSIZ", L"M0M1BL", L"P0  P1  M0  M1  BL", L"Wr" };
    static const wchar_t *const writecols[6] = { L"Register", L"Value", L"Old", L"Clock", L"Row", L"Line" };
    if (!v) return NULL;
    register_classes(inst);
    v->ui = ui;
    v->mono = mono;
    v->dpi = dpi > 0 ? dpi : 96;
    v->pal_hover = -1;
    v->reg_selected = -1;
    v->tab = a2600vid_tab_new(session);
    if (!v->tab) {
        free(v);
        return NULL;
    }
    memset(&v->bmi, 0, sizeof v->bmi);
    v->bmi.bmiHeader.biSize = sizeof v->bmi.bmiHeader;
    v->bmi.bmiHeader.biPlanes = 1;
    v->bmi.bmiHeader.biBitCount = 32;
    v->bmi.bmiHeader.biCompression = BI_RGB;
    v->page = CreateWindowExW(0, PAGE_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent,
                              NULL, inst, NULL);
    SetWindowLongPtrW(v->page, GWLP_USERDATA, (LONG_PTR)v);

    v->label_view = mk(v, L"STATIC", L"View", SS_LEFT, IDV_LABEL_VIEW, ui);
    v->view = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_VIEW, ui);
    for (i = 0; i < A2600VID_V_COUNT; i++) {
        wchar_t *w = wide(a2600vid_view_name(i));
        SendMessageW(v->view, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    v->label_source = mk(v, L"STATIC", L"Source", SS_LEFT, IDV_LABEL_SOURCE, ui);
    v->source = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_SOURCE, ui);
    for (i = 0; i < A2600VID_D_COUNT; i++) {
        wchar_t *w = wide(a2600vid_source_name(i));
        SendMessageW(v->source, CB_ADDSTRING, 0, (LPARAM)w);
        free(w);
    }
    v->label_zoom = mk(v, L"STATIC", L"Zoom", SS_LEFT, IDV_LABEL_ZOOM, ui);
    v->zoom = mk(v, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDV_ZOOM, ui);
    zv = a2600vid_zooms(&n);
    for (i = 0; i < n && i < MAX_ZOOMS; i++) {
        wchar_t lbl[16];
        v->zoom_val[v->nzoom++] = zv[i];
        if (zv[i] == 0) wcscpy(lbl, L"Fit");
        else swprintf(lbl, 16, L"%d×", zv[i]);
        SendMessageW(v->zoom, CB_ADDSTRING, 0, (LPARAM)lbl);
    }
    v->brk = mk(v, L"BUTTON", L"Break on writes", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK, ui);
    od = a2600vid_overlays(&n);
    for (i = 0; i < n && i < MAX_OVERLAYS; i++) {
        wchar_t *w = wide(od[i].label);
        v->ov[i] = mk(v, L"BUTTON", w, BS_AUTOCHECKBOX | WS_TABSTOP, IDV_OV0 + i, ui);
        v->ov_bit[i] = od[i].bit;
        v->nov++;
        free(w);
    }
    v->status = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_STATUS, mono);
    v->pic = CreateWindowExW(WS_EX_CLIENTEDGE, PIC_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL, 0, 0, 10, 10,
                             v->page, (HMENU)(INT_PTR)IDV_PIC, inst, NULL);
    SetWindowLongPtrW(v->pic, GWLP_USERDATA, (LONG_PTR)v);
    v->note = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDV_NOTE, ui);

    v->sec = mk(v, WC_TABCONTROLW, L"", WS_CLIPSIBLINGS | WS_TABSTOP, IDV_SEC, ui);
    memset(&item, 0, sizeof item);
    item.mask = TCIF_TEXT;
    for (i = 0; i < A2600VID_S_COUNT; i++) {
        wchar_t *w = wide(a2600vid_section_name(i));
        item.pszText = w;
        SendMessageW(v->sec, TCM_INSERTITEMW, (WPARAM)i, (LPARAM)&item);
        free(w);
    }
    SetWindowPos(v->sec, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);   /* under the section controls */
    v->pixel = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_PIXEL, mono);
    v->unpin = mk(v, L"BUTTON", L"Unpin", BS_PUSHBUTTON | WS_TABSTOP, IDV_UNPIN, ui);
    v->regs = mk_list(v, IDV_REGS, regcols, 3, 0);
    v->lines = mk_list(v, IDV_LINES, linecols, 9, 0);
    v->writes = mk_list(v, IDV_WRITES, writecols, 6, LVS_OWNERDATA);
    v->brk_write = mk(v, L"BUTTON", L"Break on register write", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK_WRITE, ui);
    v->cols = CreateWindowExW(0, COL_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, v->page,
                              (HMENU)(INT_PTR)IDV_COLS, inst, NULL);
    SetWindowLongPtrW(v->cols, GWLP_USERDATA, (LONG_PTR)v);
    v->brk_reg = mk(v, L"BUTTON", L"Break on register write", BS_PUSHBUTTON | WS_TABSTOP, IDV_BREAK_REG, ui);
    v->beam = mk(v, PROGRESS_CLASSW, L"", PBS_SMOOTH, IDV_BEAM, NULL);
    SendMessageW(v->beam, PBM_SETRANGE32, 0, 1000);
    v->timing = mk(v, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, IDV_TIMING, mono);

    /* start from the controller's state (the A2600_DEBUGGER_VIDEO_* variables) */
    v->updating = 1;
    SendMessageW(v->view, CB_SETCURSEL, (WPARAM)a2600vid_tab_view(v->tab), 0);
    SendMessageW(v->source, CB_SETCURSEL, (WPARAM)a2600vid_tab_source(v->tab), 0);
    EnableWindow(v->source, a2600vid_tab_view(v->tab) == A2600VID_V_DECODED);
    for (i = 0; i < v->nzoom; i++)
        if (v->zoom_val[i] == a2600vid_tab_zoom(v->tab)) SendMessageW(v->zoom, CB_SETCURSEL, (WPARAM)i, 0);
    for (i = 0; i < v->nov; i++)
        SendMessageW(v->ov[i], BM_SETCHECK, (a2600vid_tab_overlays(v->tab) & v->ov_bit[i]) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(v->sec, TCM_SETCURSEL, (WPARAM)a2600vid_tab_env_section(v->tab), 0);
    v->updating = 0;
    show_section(v);
    return v->page;
}

void dbg_video_place(HWND page, int x, int y, int w, int h)
{
    MoveWindow(page, x, y, w > 1 ? w : 1, h > 1 ? h : 1, TRUE);
}

void dbg_video_fonts(HWND page, HFONT ui, HFONT mono, int dpi)
{
    vidwin *v = (vidwin *)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (!v) return;
    v->ui = ui;
    v->mono = mono;
    v->dpi = dpi > 0 ? dpi : 96;
    layout(v);
    InvalidateRect(v->page, NULL, TRUE);
}
