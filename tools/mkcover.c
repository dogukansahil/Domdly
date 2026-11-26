/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mkcover: draws the 1280 x 640 repository cover (README banner and GitHub
 * social preview) and writes it as PNG. Everything in the picture is drawn
 * here: the logo comes from icon_art.h, the window mock-up and icons are
 * simple shapes, and the text is set with fonts that ship with Windows. The
 * PNG encoder is the one built into Windows (GDI+), so no third-party code
 * or artwork is involved.
 *
 * Windows only. Build and run with "build.bat cover", which writes
 * docs/images/cover.png.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "icon_art.h"

#define W 1280
#define H 640

static DWORD *g_px; /* 0x00RRGGBB, top-down */
static HDC g_dc;

/* ---- pixels ------------------------------------------------------------- */

static double clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static void blend(int x, int y, COLORREF c, double a)
{
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    if (a > 1) a = 1;
    DWORD *p = &g_px[y * W + x];
    double r0 = (double)((*p >> 16) & 255), g0 = (double)((*p >> 8) & 255), b0 = (double)(*p & 255);
    double r = r0 + ((double)GetRValue(c) - r0) * a;
    double g = g0 + ((double)GetGValue(c) - g0) * a;
    double b = b0 + ((double)GetBValue(c) - b0) * a;
    *p = ((DWORD)(r + 0.5) << 16) | ((DWORD)(g + 0.5) << 8) | (DWORD)(b + 0.5);
}

/* ---- shapes as signed distance functions ------------------------------- */

enum { SH_RRECT, SH_CIRCLE, SH_SEG };

typedef struct shape {
    int kind;
    double cx, cy, hw, hh, r; /* rounded rectangle / circle */
    double x1, y1, x2, y2;    /* segment (r = half thickness) */
} shape;

static double sd_shape(const shape *s, double x, double y)
{
    if (s->kind == SH_RRECT) return sd_round_rect(x, y, s->cx, s->cy, s->hw, s->hh, s->r);
    if (s->kind == SH_CIRCLE) return sqrt((x - s->cx) * (x - s->cx) + (y - s->cy) * (y - s->cy)) - s->r;
    double dx = s->x2 - s->x1, dy = s->y2 - s->y1, px = x - s->x1, py = y - s->y1;
    double len2 = dx * dx + dy * dy, t = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    double ex = px - dx * t, ey = py - dy * t;
    return sqrt(ex * ex + ey * ey) - s->r;
}

/* Fills a shape; feather > 1 gives a soft edge (used for shadows). */
static void fill(const shape *s, COLORREF c, double alpha, double feather)
{
    double ext = (s->kind == SH_SEG ? s->r : 0) + feather + 2;
    double x0, y0, x1, y1;
    if (s->kind == SH_SEG) {
        x0 = fmin(s->x1, s->x2) - ext;
        x1 = fmax(s->x1, s->x2) + ext;
        y0 = fmin(s->y1, s->y2) - ext;
        y1 = fmax(s->y1, s->y2) + ext;
    } else {
        double hw = s->kind == SH_CIRCLE ? s->r : s->hw, hh = s->kind == SH_CIRCLE ? s->r : s->hh;
        x0 = s->cx - hw - ext;
        x1 = s->cx + hw + ext;
        y0 = s->cy - hh - ext;
        y1 = s->cy + hh + ext;
    }
    for (int y = (int)y0; y <= (int)y1; y++)
        for (int x = (int)x0; x <= (int)x1; x++) {
            double d = sd_shape(s, x + 0.5, y + 0.5);
            blend(x, y, c, alpha * clamp01(0.5 - d / feather));
        }
}

static void rrect(double x, double y, double w, double h, double r, COLORREF c, double a)
{
    shape s = {SH_RRECT, x + w / 2, y + h / 2, w / 2, h / 2, r, 0, 0, 0, 0};
    fill(&s, c, a, 1);
}

static void circle(double cx, double cy, double r, COLORREF c, double a)
{
    shape s = {SH_CIRCLE, cx, cy, 0, 0, r, 0, 0, 0, 0};
    fill(&s, c, a, 1);
}

static void line(double x1, double y1, double x2, double y2, double width, COLORREF c, double a)
{
    shape s = {SH_SEG, 0, 0, 0, 0, width / 2, x1, y1, x2, y2};
    fill(&s, c, a, 1);
}

/* ---- text (GDI on the same pixels) -------------------------------------- */

static void text(int x, int y, const WCHAR *t, const WCHAR *face, int px, int weight, COLORREF c)
{
    GdiFlush();
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    HGDIOBJ old = SelectObject(g_dc, f);
    SetBkMode(g_dc, TRANSPARENT);
    SetTextColor(g_dc, c);
    TextOutW(g_dc, x, y, t, (int)wcslen(t));
    SelectObject(g_dc, old);
    DeleteObject(f);
    GdiFlush();
}

static int text_width(const WCHAR *t, const WCHAR *face, int px, int weight)
{
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                          CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    HGDIOBJ old = SelectObject(g_dc, f);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(g_dc, t, (int)wcslen(t), &sz);
    SelectObject(g_dc, old);
    DeleteObject(f);
    return sz.cx;
}

/* A rounded label with an outline. */
static int pill(int x, int y, const WCHAR *t)
{
    int w = text_width(t, L"Segoe UI", 21, FW_SEMIBOLD) + 36;
    rrect(x, y, w, 42, 21, RGB(120, 160, 255), 0.13);
    rrect(x + 1, y + 1, w - 2, 40, 20, RGB(16, 26, 48), 0.85);
    text(x + 18, y + 9, t, L"Segoe UI", 21, FW_SEMIBOLD, RGB(214, 226, 248));
    return x + w + 14;
}

/* ---- the logo ----------------------------------------------------------- */

static void logo(int x0, int y0, int size)
{
    const int ss = 4;
    for (int py = -6; py < size + 24; py++)
        for (int px = -6; px < size + 6; px++) {
            /* soft glow below the tile */
            double gy = (py - size * 0.62) / (size * 0.55), gx = (px - size * 0.5) / (size * 0.62);
            double glow = 1 - sqrt(gx * gx + gy * gy);
            if (glow > 0) blend(x0 + px, y0 + py + 10, RGB(47, 111, 235), glow * glow * 0.35);
        }
    for (int py = 0; py < size; py++)
        for (int px = 0; px < size; px++) {
            double bg = 0, fg = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    double x = (px + (sx + 0.5) / ss) / size, y = (py + (sy + 0.5) / ss) / size;
                    if (sd_round_rect(x, y, 0.5, 0.5, 0.5, 0.5, icon_radius) <= 0) {
                        bg += 1;
                        if (in_mark(x, y)) fg += 1;
                    }
                }
            bg /= ss * ss;
            fg /= ss * ss;
            double t = (double)py / (size - 1);
            double r = icon_top[0] + (icon_bottom[0] - icon_top[0]) * t;
            double g = icon_top[1] + (icon_bottom[1] - icon_top[1]) * t;
            double b = icon_top[2] + (icon_bottom[2] - icon_top[2]) * t;
            double k = bg > 0 ? fg / bg : 0;
            r = r * (1 - k) + k;
            g = g * (1 - k) + k;
            b = b * (1 - k) + k;
            blend(x0 + px, y0 + py, RGB((int)(r * 255), (int)(g * 255), (int)(b * 255)), bg);
        }
}

/* ---- the window mock-up -------------------------------------------------- */

static void half_sun_moon(double cx, double cy, double s, COLORREF c)
{
    for (int y = (int)(cy - s); y <= (int)(cy + s); y++)
        for (int x = (int)(cx - s); x <= (int)(cx + s); x++) {
            double dx = x + 0.5 - cx, dy = y + 0.5 - cy, r = sqrt(dx * dx + dy * dy);
            double sun = fmax(r - s * 0.25, dx + s * 0.05);
            for (int i = 0; i < 3; i++) {
                double a = 3.14159265 * (0.75 + 0.25 * i);
                shape ray = {SH_SEG, 0, 0, 0, 0, s * 0.055, cx + cos(a) * s * 0.36, cy + sin(a) * s * 0.36,
                             cx + cos(a) * s * 0.47, cy + sin(a) * s * 0.47};
                sun = fmin(sun, sd_shape(&ray, x + 0.5, y + 0.5));
            }
            double cut = sqrt((dx + s * 0.02) * (dx + s * 0.02) + dy * dy) - s * 0.20;
            double moon = fmax(fmax(r - s * 0.36, -cut), -dx + s * 0.05);
            blend(x, y, c, clamp01(0.5 - fmin(sun, moon)));
        }
}

static void mockup(int x, int y, int w, int h)
{
    const COLORREF page = RGB(22, 24, 29), bar = RGB(30, 33, 39), border = RGB(52, 58, 70);
    const COLORREF fg = RGB(230, 237, 243), muted = RGB(120, 130, 146), accent = RGB(88, 150, 255);

    shape shadow = {SH_RRECT, x + w / 2.0, y + h / 2.0 + 24, w / 2.0, h / 2.0, 18, 0, 0, 0, 0};
    fill(&shadow, RGB(0, 0, 0), 0.55, 48);
    rrect(x - 1, y - 1, w + 2, h + 2, 17, border, 1);
    rrect(x, y, w, h, 16, page, 1);

    /* header */
    rrect(x, y, w, 52, 16, bar, 1);
    rrect(x, y + 30, w, 22, 0, bar, 1);
    line(x, y + 52, x + w, y + 52, 1, border, 1);
    for (int i = -1; i <= 1; i++) line(x + 22, y + 26 + i * 6, x + 38, y + 26 + i * 6, 2.2, fg, 1);
    /* files sidebar button: a window outline with a left column */
    rrect(x + 52, y + 18, 22, 17, 4, muted, 1);
    rrect(x + 54, y + 20, 18, 13, 2.5, bar, 1);
    line(x + 59.5, y + 20, x + 59.5, y + 33, 2, muted, 1);
    /* tabs: the active one takes the page colour */
    int t1 = text_width(L"notes.md", L"Segoe UI", 17, FW_SEMIBOLD) + 44;
    rrect(x + 86, y + 10, t1, 32, 8, border, 1);
    rrect(x + 87, y + 11, t1 - 2, 30, 7, page, 1);
    text(x + 98, y + 16, L"notes.md", L"Segoe UI", 17, FW_SEMIBOLD, fg);
    circle(x + 86 + t1 - 16, y + 26, 4, accent, 1);
    int t2x = x + 86 + t1 + 8;
    text(t2x + 12, y + 16, L"todo.md", L"Segoe UI", 17, FW_NORMAL, muted);
    int px2 = t2x + text_width(L"todo.md", L"Segoe UI", 17, FW_NORMAL) + 26;
    line(px2, y + 26, px2 + 14, y + 26, 2, fg, 1);
    line(px2 + 7, y + 19, px2 + 7, y + 33, 2, fg, 1);
    rrect(x + w - 126, y + 11, 104, 31, 8, border, 1);
    rrect(x + w - 125, y + 12, 102, 29, 7, bar, 1);
    line(x + w - 108, y + 18, x + w - 108, y + 29, 2, fg, 1);
    line(x + w - 113, y + 25, x + w - 108, y + 30, 2, fg, 1);
    line(x + w - 103, y + 25, x + w - 108, y + 30, 2, fg, 1);
    line(x + w - 116, y + 33, x + w - 100, y + 33, 2, fg, 1);
    text(x + w - 92, y + 16, L"Save as", L"Segoe UI", 16, FW_NORMAL, fg);

    /* document */
    int lx = x + 40, cy = y + 80;
    text(lx, cy, L"Project notes", L"Segoe UI", 34, FW_BOLD, fg);
    line(lx, cy + 52, x + w - 40, cy + 52, 1.2, border, 1);
    cy += 72;
    /* a paragraph being typed: bold stays bold, no Markdown marks */
    int tx = lx;
    text(tx, cy, L"Bold stays ", L"Segoe UI", 19, FW_NORMAL, fg);
    tx += text_width(L"Bold stays ", L"Segoe UI", 19, FW_NORMAL);
    text(tx, cy, L"bold", L"Segoe UI", 19, FW_BOLD, fg);
    tx += text_width(L"bold", L"Segoe UI", 19, FW_BOLD);
    line(tx + 1, cy + 2, tx + 1, cy + 24, 2, accent, 1);
    text(tx + 4, cy, L" while you type.", L"Segoe UI", 19, FW_NORMAL, fg);
    rrect(lx, cy + 36, 330, 9, 4.5, muted, 0.55);
    rrect(lx, cy + 60, 220, 9, 4.5, muted, 0.55);
    cy += 92;

    /* a task list, drawn as it reads */
    text(lx, cy, L"Tasks", L"Segoe UI", 24, FW_BOLD, fg);
    cy += 42;
    static const WCHAR *tasks[2] = {L"A tab for every document", L"Save a copy as PDF"};
    for (int i = 0; i < 2; i++) {
        int by = cy + i * 30;
        rrect(lx + 2, by + 4, 16, 16, 3, fg, 1);
        rrect(lx + 4, by + 6, 12, 12, 2, page, 1);
        if (i == 0) {
            line(lx + 6, by + 12, lx + 9, by + 16, 2, accent, 1);
            line(lx + 9, by + 16, lx + 15, by + 7, 2, accent, 1);
        }
        text(lx + 30, by, tasks[i], L"Segoe UI", 18, FW_NORMAL, fg);
    }
    cy += 76;

    /* a small table */
    int tw = 300, th = 31;
    rrect(lx, cy, tw, th, 0, RGB(34, 39, 46), 1);
    for (int r = 0; r <= 3; r++) line(lx, cy + r * th, lx + tw, cy + r * th, 1.2, border, 1);
    line(lx, cy, lx, cy + 3 * th, 1.2, border, 1);
    line(lx + 150, cy, lx + 150, cy + 3 * th, 1.2, border, 1);
    line(lx + tw, cy, lx + tw, cy + 3 * th, 1.2, border, 1);
    text(lx + 12, cy + 5, L"Format", L"Segoe UI", 17, FW_SEMIBOLD, fg);
    text(lx + 162, cy + 5, L"Saved as", L"Segoe UI", 17, FW_SEMIBOLD, fg);
    text(lx + 12, cy + th + 5, L"Markdown", L"Segoe UI", 17, FW_NORMAL, fg);
    text(lx + 162, cy + th + 5, L".md", L"Segoe UI", 17, FW_NORMAL, fg);
    text(lx + 12, cy + 2 * th + 5, L"PDF", L"Segoe UI", 17, FW_NORMAL, fg);
    text(lx + 162, cy + 2 * th + 5, L".pdf", L"Segoe UI", 17, FW_NORMAL, fg);

    /* status bar */
    int sy = y + h - 46;
    rrect(x, sy, w, 46, 16, bar, 1);
    rrect(x, sy, w, 20, 0, bar, 1);
    line(x, sy, x + w, sy, 1, border, 1);
    text(x + 22, sy + 13, L"Edit right here", L"Segoe UI", 15, FW_NORMAL, muted);
    int seg = x + w - 196;
    rrect(seg, sy + 9, 124, 28, 14, RGB(42, 46, 54), 1);
    rrect(seg + 64, sy + 11, 58, 24, 12, RGB(66, 72, 84), 1);
    text(seg + 16, sy + 14, L"Edit", L"Segoe UI", 15, FW_NORMAL, muted);
    text(seg + 76, sy + 14, L"Read", L"Segoe UI", 15, FW_BOLD, fg);
    half_sun_moon(x + w - 38, sy + 23, 22, fg);
}

/* ---- PNG through GDI+ (part of Windows) ---------------------------------- */

typedef struct {
    UINT32 version;
    void *debug_callback;
    BOOL suppress_thread;
    BOOL suppress_codecs;
} gdiplus_startup_input;

int WINAPI GdiplusStartup(ULONG_PTR *token, const gdiplus_startup_input *input, void *output);
void WINAPI GdiplusShutdown(ULONG_PTR token);
int WINAPI GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT format, BYTE *scan0, void **bitmap);
int WINAPI GdipSaveImageToFile(void *image, const WCHAR *file, const CLSID *encoder, const void *params);
int WINAPI GdipDisposeImage(void *image);

static int save_png(const WCHAR *path)
{
    static const CLSID png = {0x557CF406, 0x1A04, 0x11D3, {0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E}};
    gdiplus_startup_input in = {1, NULL, FALSE, FALSE};
    ULONG_PTR token;
    if (GdiplusStartup(&token, &in, NULL) != 0) return 0;
    void *bmp = NULL;
    int ok = GdipCreateBitmapFromScan0(W, H, W * 4, 0x00022009 /* 32bpp RGB */, (BYTE *)g_px, &bmp) == 0 &&
             GdipSaveImageToFile(bmp, path, &png, NULL) == 0;
    if (bmp) GdipDisposeImage(bmp);
    GdiplusShutdown(token);
    return ok;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc != 2) {
        fwprintf(stderr, L"usage: mkcover <output.png>\n");
        return 2;
    }
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_dc = CreateCompatibleDC(NULL);
    HBITMAP dib = CreateDIBSection(g_dc, &bi, DIB_RGB_COLORS, (void **)&g_px, NULL, 0);
    if (!dib || !g_px) return 1;
    SelectObject(g_dc, dib);

    /* background: deep navy, lighter towards the bottom right, a blue glow
     * behind the window and a faint dot grid */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double t = (x / (double)W) * 0.55 + (y / (double)H) * 0.45;
            double r = 10 + 14 * t, g = 16 + 22 * t, b = 32 + 42 * t;
            double gx = (x - 990) / 560.0, gy = (y - 330) / 420.0;
            double glow = 1 - sqrt(gx * gx + gy * gy);
            if (glow > 0) {
                glow = glow * glow * 0.45;
                r += (47 - r) * glow;
                g += (111 - g) * glow;
                b += (235 - b) * glow;
            }
            g_px[y * W + x] = ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
        }
    for (int y = 20; y < H; y += 32)
        for (int x = 20; x < W; x += 32) circle(x, y, 1.3, RGB(255, 255, 255), 0.05);

    mockup(748, 66, 470, 512);

    /* left column */
    logo(96, 118, 112);
    text(232, 112, L"Domdly", L"Segoe UI", 104, FW_SEMIBOLD, RGB(245, 248, 252));
    text(98, 278, L"Markdown reader and editor", L"Segoe UI", 40, FW_SEMIBOLD, RGB(230, 237, 246));
    text(98, 334, L"Written from scratch in plain C for Windows and Linux,", L"Segoe UI", 25, FW_NORMAL,
         RGB(152, 170, 200));
    text(98, 366, L"no libraries, no browser engine, no telemetry.", L"Segoe UI", 25, FW_NORMAL, RGB(152, 170, 200));
    int px = 98;
    px = pill(px, 430, L"Edit like a document");
    px = pill(px, 430, L"Tabs");
    pill(px, 430, L"PDF export");
    text(100, 520, L"Windows 10 and 11   \x00B7   Linux (Debian, GTK 3)   \x00B7   GPL-3.0", L"Segoe UI", 19,
         FW_NORMAL, RGB(120, 138, 168));

    int ok = save_png(argv[1]);
    DeleteObject(dib);
    DeleteDC(g_dc);
    if (!ok) {
        fwprintf(stderr, L"mkcover: could not write %ls\n", argv[1]);
        return 1;
    }
    return 0;
}
