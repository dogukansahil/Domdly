/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Windows front end. Pure Win32: the document area is a RichEdit control
 * provided by the operating system (msftedit.dll) that switches between an
 * "Edit" view (plain Markdown) and a "Read" view (RTF rendered by the
 * portable core). The window chrome (header, status bar, buttons, icons) is
 * drawn by this file with anti-aliased shapes computed from simple geometry.
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <ole2.h>
#include <richedit.h>
#include <richole.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tom.h>
#include <uxtheme.h>
#include <winhttp.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "core/markdown.h"
#include "core/mdbuf.h"
#include "core/render_html.h"
#include "core/render_rtf.h"
#include "core/md_write.h"
#include "core/md_html.h"
#include "install.h"
#include "olecb.h"
#include "resource.h"
#include "settings.h"
#include "util.h"
#include "version.h"

#define MAIN_CLASS   L"Domdly.Main"
#define TEXT_CLASS   L"Domdly.Text"
#define DROP_CLASS   L"Domdly.Drop"
#define TIMER_RENDER 1
#define TIMER_RESIZE 2
#define TIMER_WORDS 3
#define TIMER_SYNC 4
#define RENDER_DELAY 220
#define WM_APP_OPEN    (WM_APP + 1)
#define WM_APP_IMAGES  (WM_APP + 7) /* lParam: dropped_images *, dropped pictures to link */
#define WM_APP_TREE    (WM_APP + 2) /* open a file picked in the sidebar */
#define WM_APP_UPDATE  (WM_APP + 3) /* the update check has an answer */

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#define DWMWA_CAPTION_COLOR_ 35
#define DWMWA_TEXT_COLOR_    36

enum { HOT_NONE, HOT_MENU, HOT_ZOOM, HOT_EDIT, HOT_READ, HOT_THEME, HOT_SAVE, HOT_NEWTAB, HOT_SIDEBTN, HOT_SIDEHEAD, HOT_SPLIT, HOT_VER, HOT_PATH };
enum { ICON_MENU, ICON_SUN, ICON_MOON, ICON_HALF, ICON_SAVE, ICON_PLUS, ICON_CLOSE, ICON_SIDEBAR, ICON_SIDEBAR_ON, ICON_CHEVRON,
       ICON_BULLETS, ICON_NUMBERS, ICON_TASK, ICON_QUOTE, ICON_LINK, ICON_HR };

typedef struct palette {
    COLORREF doc_bg, doc_fg, bar_bg, bar_border, bar_fg, bar_muted, hover, track, pill, pill_border, accent, drop_bg;
} palette;

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */

static HINSTANCE g_inst;
static HWND g_main, g_edit, g_prev, g_find, g_tip, g_drop;
static HACCEL g_accel;
static HMENU g_menubar, g_recent_menu;
static app_settings S;
static palette P;
static int g_dark;
static int g_night; /* the black "Night" variant of the dark theme */
static int g_dpi = 96;
static int g_words;
static int g_preview_dirty = 1;
static int g_last_render_width;
static ITextDocument *g_tom; /* editor's Text Object Model */

static RECT g_rc_side, g_rc_sidehead, g_rc_sidebtn, g_rc_split, g_rc_ver;
static RECT g_rc_path; /* full path of the open file, left end of the status bar */

/* Formatting toolbar: a slim strip under the tabs. Each button runs the same
 * command as its menu item and shortcut; id 0 is a gap between groups. */
enum { FB_ICON, FB_BOLD, FB_ITALIC, FB_STRIKE, FB_MONO, FB_HEAD };
typedef struct fmt_btn {
    int id, look, icon;
    const WCHAR *text, *tip;
} fmt_btn;
static const fmt_btn g_fmt[] = {
    {IDM_BOLD, FB_BOLD, 0, L"B", L"Bold (Ctrl+B)"},
    {IDM_ITALIC, FB_ITALIC, 0, L"I", L"Italic (Ctrl+I)"},
    {IDM_STRIKE, FB_STRIKE, 0, L"S", L"Strikethrough (Ctrl+Shift+X)"},
    {IDM_CODE, FB_MONO, 0, L"</>", L"Inline code (Ctrl+`)"},
    {0, 0, 0, NULL, NULL},
    {IDM_H1, FB_HEAD, 0, L"H1", L"Heading 1 (Ctrl+1)"},
    {IDM_H2, FB_HEAD, 0, L"H2", L"Heading 2 (Ctrl+2)"},
    {IDM_H3, FB_HEAD, 0, L"H3", L"Heading 3 (Ctrl+3)"},
    {0, 0, 0, NULL, NULL},
    {IDM_BULLET, FB_ICON, ICON_BULLETS, NULL, L"Bulleted list (Ctrl+Shift+8)"},
    {IDM_NUMBERED, FB_ICON, ICON_NUMBERS, NULL, L"Numbered list (Ctrl+Shift+7)"},
    {IDM_TASK, FB_ICON, ICON_TASK, NULL, L"Task list (Ctrl+Shift+9)"},
    {IDM_QUOTE, FB_ICON, ICON_QUOTE, NULL, L"Quote (Ctrl+Shift+Q)"},
    {0, 0, 0, NULL, NULL},
    {IDM_LINK, FB_ICON, ICON_LINK, NULL, L"Link (Ctrl+K)"},
    {IDM_CODEBLOCK, FB_MONO, 0, L"{}", L"Code block (Ctrl+Shift+`)"},
    {IDM_HR, FB_ICON, ICON_HR, NULL, L"Horizontal rule"},
};
#define FMT_COUNT ((int)(sizeof g_fmt / sizeof g_fmt[0]))
static RECT g_rc_fmt, g_rc_fbtn[FMT_COUNT];
static HWND g_tree;          /* files sidebar (a tree view) */
static WCHAR g_tree_root[DOMDLY_PATH_MAX]; /* folder the sidebar shows */
static int g_tree_drag;      /* resizing the sidebar */
static int g_tree_quiet;     /* selection changed by Domdly, not the user */
static RECT g_rc_save, g_rc_top, g_rc_bottom, g_rc_menu, g_rc_zoom, g_rc_seg, g_rc_edit, g_rc_read, g_rc_theme;
static int g_hot, g_pressed, g_tracking;
static WCHAR g_status[256];

static HFONT g_font_ui, g_font_bold, g_font_small, g_font_big, g_font_tiny;
/* letters on the formatting toolbar: bold B, italic I, struck S, code, headings */
static HFONT g_font_fb, g_font_fi, g_font_fs, g_font_fm, g_font_fh;
static WCHAR g_ui_family[LF_FACESIZE] = L"Segoe UI";
static WCHAR g_mono_family[LF_FACESIZE] = L"Consolas";
static char g_ui_family_a[LF_FACESIZE] = "Segoe UI";
static char g_mono_family_a[LF_FACESIZE] = "Consolas";
static int g_tab_twips = 484;

static WCHAR g_path[DOMDLY_PATH_MAX];
static int g_has_path;
static int g_crlf;
static text_encoding g_enc = ENC_UTF8;

/* Tabs. Each open document has its own editor control (so undo history,
 * selection and scroll position stay with it); the globals above always
 * describe the active tab and are copied in and out on a switch. The Read
 * view is shared and re-rendered for the active tab. */
#define MAX_TABS 64
typedef struct doc_tab {
    HWND edit;
    ITextDocument *tom;
    WCHAR path[DOMDLY_PATH_MAX];
    int has_path, crlf;
    text_encoding enc;
    POINT read_scroll;
} doc_tab;
static doc_tab g_tabs[MAX_TABS];
static int g_ntabs = 1, g_cur;
static RECT g_rc_tab[MAX_TABS], g_rc_tabx[MAX_TABS], g_rc_newtab;
#define HOT_TAB 100  /* + tab index */
#define HOT_TABX 200 /* + tab index: the close button */
#define HOT_FMT 300  /* + index into g_fmt: the formatting toolbar */
#define COPYDATA_OPEN 0x444D4430 /* another Domdly process passes a file */

static int tab_new(void);
static int tab_is_blank(void);
static int tab_find(const WCHAR *full);
static void tab_activate(int i);


static WCHAR g_drop_name[DOMDLY_PATH_MAX];

static UINT g_find_msg;
static FINDREPLACEW g_fr;
static WCHAR g_find_text[256], g_replace_text[256];

/* Block markers: source line and position in the Read view, in order. */
static int *g_mline;
static LONG *g_mcp;
static int g_mcount, g_mcap;

/* Editing in the Read view: the top-level block(s) on source lines
 * g_raw_line .. g_raw_line + g_raw_count - 1 are shown as Markdown source
 * and can be edited in place; every change is copied to the editor, which
 * stays the one true copy of the document. */
static int g_raw_line = -1, g_raw_count;
static WCHAR *g_raw_last;   /* region text as last copied to the editor */
static int g_rendering;     /* streaming into the Read view */
static int g_syncing;       /* editor changes made by Domdly itself */
static int g_raw_cmd;       /* running an editor command for the Read view */
static int g_hold_redraw;   /* caller already froze the Read view */
static int g_link_clicked;
static int g_welcome_shown;
static int g_raw_shown;     /* the Read view shows the region for g_raw_line */

static const char welcome_md[] =
    "# Welcome to Domdly\n\n"
    "There is nothing to read yet. Click anywhere here and start typing, as in a word processor.\n\n"
    "- **Ctrl+O** opens files, each in its own tab; **Ctrl+S** saves.\n"
    "- Drop `.md` files anywhere on this window.\n"
    "- **Ctrl+B**, **Ctrl+I**, **Ctrl+1** to **3** and the list shortcuts format text; **Ctrl+E** shows the Markdown.\n"
    "- **Menu \xE2\x86\x92 Help \xE2\x86\x92 Markdown Sample** shows everything Domdly can render.\n";

static int S_(int px) { return util_scale(px, g_dpi); }
static float SF(float px) { return px * (float)g_dpi / 96.0f; }
static int is_read(void) { return S.view == VIEW_PREVIEW; }
static void tree_sync(int refresh);
static void tree_toggle(void);
static void tree_pick_folder(void);
static void tree_apply_theme(void);
static void cmd_check_updates(void);

/* ------------------------------------------------------------------------ */
/* RichEdit helpers                                                         */
/* ------------------------------------------------------------------------ */

typedef struct stream_src {
    const char *p;
    size_t len, pos;
} stream_src;

static DWORD CALLBACK stream_in_cb(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG *pcb)
{
    stream_src *s = (stream_src *)cookie;
    size_t left = s->len - s->pos;
    size_t n = left < (size_t)cb ? left : (size_t)cb;
    memcpy(buf, s->p + s->pos, n);
    s->pos += n;
    *pcb = (LONG)n;
    return 0;
}

static void stream_in(HWND h, const void *data, size_t len, UINT format)
{
    stream_src src;
    src.p = (const char *)data;
    src.len = len;
    src.pos = 0;
    EDITSTREAM es;
    es.dwCookie = (DWORD_PTR)&src;
    es.dwError = 0;
    es.pfnCallback = stream_in_cb;
    SendMessageW(h, EM_STREAMIN, format, (LPARAM)&es);
}

static WCHAR *control_text(HWND h, int *out_len)
{
    GETTEXTLENGTHEX gl;
    gl.flags = GTL_NUMCHARS | GTL_PRECISE;
    gl.codepage = 1200;
    int n = (int)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    WCHAR *w = (WCHAR *)malloc(((size_t)n + 2) * sizeof(WCHAR));
    if (!w) return NULL;
    GETTEXTEX gt;
    memset(&gt, 0, sizeof gt);
    gt.cb = (DWORD)(((size_t)n + 1) * sizeof(WCHAR));
    gt.flags = GT_DEFAULT;
    gt.codepage = 1200;
    int got = (int)SendMessageW(h, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)w);
    if (got < 0) got = 0;
    if (got > n) got = n;
    w[got] = 0;
    if (out_len) *out_len = got;
    return w;
}

/* Editor text as UTF-8 with '\n' line breaks. */
static char *editor_utf8(int *len)
{
    int n = 0;
    WCHAR *w = control_text(g_edit, &n);
    if (!w) return NULL;
    for (int i = 0; i < n; i++)
        if (w[i] == '\r') w[i] = '\n';
    char *u = util_wide_to_utf8(w, n, len);
    free(w);
    return u;
}

static WCHAR *get_range(HWND h, LONG a, LONG b)
{
    if (b < a) b = a;
    WCHAR *buf = (WCHAR *)malloc(((size_t)(b - a) + 2) * sizeof(WCHAR));
    if (!buf) return NULL;
    buf[0] = 0;
    TEXTRANGEW tr;
    tr.chrg.cpMin = a;
    tr.chrg.cpMax = b;
    tr.lpstrText = buf;
    SendMessageW(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    return buf;
}

static void set_sel(HWND h, LONG a, LONG b)
{
    CHARRANGE cr;
    cr.cpMin = a;
    cr.cpMax = b;
    SendMessageW(h, EM_EXSETSEL, 0, (LPARAM)&cr);
}

static CHARRANGE get_sel(HWND h)
{
    CHARRANGE cr;
    SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&cr);
    return cr;
}

/* WM_SETREDRAW TRUE also sets WS_VISIBLE, so only freeze visible controls. */
static int freeze(HWND h)
{
    if (!IsWindowVisible(h)) return 0;
    SendMessageW(h, WM_SETREDRAW, FALSE, 0);
    return 1;
}

static void thaw(HWND h, int frozen)
{
    if (!frozen) return;
    SendMessageW(h, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(h, NULL, TRUE);
}

static void replace_sel(const WCHAR *text)
{
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)text);
}

static void scroll_so_char_at(HWND h, LONG cp, int y)
{
    POINTL pt;
    SendMessageW(h, EM_POSFROMCHAR, (WPARAM)&pt, cp);
    POINT cur;
    SendMessageW(h, EM_GETSCROLLPOS, 0, (LPARAM)&cur);
    POINT target;
    target.x = 0;
    target.y = cur.y + pt.y - y;
    if (target.y < 0) target.y = 0;
    SendMessageW(h, EM_SETSCROLLPOS, 0, (LPARAM)&target);
}

/* ------------------------------------------------------------------------ */
/* Paragraphs in the editor (source lines). RichEdit "lines" are display    */
/* lines that change with word wrap, so the Text Object Model is used.      */
/* ------------------------------------------------------------------------ */

/* {8CC497C0-A1DF-11CE-8098-00AA0047BE5D} */
static const IID iid_text_document = {0x8CC497C0, 0xA1DF, 0x11CE, {0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D}};

/* Bounds of the source line containing cp (end excludes the line break)
 * and its 0-based index. */
static void para_at(LONG cp, LONG *start, LONG *end, long *index)
{
    long s = cp, e = cp, idx = 1;
    ITextRange *r = NULL;
    if (g_tom && SUCCEEDED(g_tom->lpVtbl->Range(g_tom, cp, cp, &r)) && r) {
        r->lpVtbl->GetIndex(r, tomParagraph, &idx);
        r->lpVtbl->Expand(r, tomParagraph, NULL);
        r->lpVtbl->GetStart(r, &s);
        r->lpVtbl->GetEnd(r, &e);
        r->lpVtbl->Release(r);
        if (e > s) {
            WCHAR *last = get_range(g_edit, e - 1, e);
            if (last && last[0] == '\r') e--;
            free(last);
        }
    }
    if (start) *start = s;
    if (end) *end = e;
    if (index) *index = idx > 0 ? idx - 1 : 0;
}

static LONG para_start_of_line(long line)
{
    LONG s = 0;
    ITextRange *r = NULL;
    if (g_tom && SUCCEEDED(g_tom->lpVtbl->Range(g_tom, 0, 0, &r)) && r) {
        long moved = 0;
        if (line > 0) r->lpVtbl->Move(r, tomParagraph, line, &moved);
        r->lpVtbl->GetStart(r, &s);
        r->lpVtbl->Release(r);
    }
    return s;
}

/* ------------------------------------------------------------------------ */
/* Fonts and theme                                                          */
/* ------------------------------------------------------------------------ */

static int CALLBACK font_enum_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    (void)lf;
    (void)tm;
    (void)type;
    *(int *)lp = 1;
    return 0;
}

static int font_exists(const WCHAR *name)
{
    LOGFONTW lf;
    memset(&lf, 0, sizeof lf);
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(lf.lfFaceName, name, LF_FACESIZE - 1);
    HDC dc = GetDC(NULL);
    int found = 0;
    EnumFontFamiliesExW(dc, &lf, font_enum_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static void choose_families(void)
{
    if (font_exists(L"Segoe UI Variable Text")) wcscpy(g_ui_family, L"Segoe UI Variable Text");
    if (font_exists(L"Cascadia Mono")) {
        wcscpy(g_mono_family, L"Cascadia Mono");
        g_tab_twips = 528; /* 4 columns of 0.6 em at 11 pt */
    }
    WideCharToMultiByte(CP_ACP, 0, g_ui_family, -1, g_ui_family_a, LF_FACESIZE, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, g_mono_family, -1, g_mono_family_a, LF_FACESIZE, NULL, NULL);
}

static HFONT make_font(int px, int weight)
{
    return CreateFontW(-S_(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, g_ui_family);
}

static void create_fonts(void)
{
    if (g_font_ui) DeleteObject(g_font_ui);
    if (g_font_bold) DeleteObject(g_font_bold);
    if (g_font_small) DeleteObject(g_font_small);
    if (g_font_big) DeleteObject(g_font_big);
    if (g_font_tiny) DeleteObject(g_font_tiny);
    g_font_ui = make_font(13, FW_NORMAL);
    g_font_bold = make_font(13, FW_SEMIBOLD);
    g_font_small = make_font(12, FW_NORMAL);
    g_font_big = make_font(22, FW_SEMIBOLD);
    g_font_tiny = make_font(10, FW_NORMAL);
    HFONT *ff[] = {&g_font_fb, &g_font_fi, &g_font_fs, &g_font_fm, &g_font_fh};
    for (int i = 0; i < 5; i++)
        if (*ff[i]) DeleteObject(*ff[i]);
    g_font_fb = make_font(16, FW_BOLD);
    g_font_fi = CreateFontW(-S_(16), 0, 0, 0, FW_NORMAL, TRUE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_ROMAN, L"Georgia");
    g_font_fs = CreateFontW(-S_(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, TRUE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, g_ui_family);
    g_font_fm = CreateFontW(-S_(13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, g_mono_family);
    g_font_fh = make_font(13, FW_BOLD);
}

static int system_prefers_dark(void)
{
    DWORD v = 1, size = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS)
        return v == 0;
    return 0;
}

/* Popup menus follow the app theme through uxtheme's (unnamed) app-mode
 * entry points, which Windows 10 1903+ export by ordinal. Missing entry
 * points are simply skipped. */
static void set_menu_theme(int dark)
{
    HMODULE ux = GetModuleHandleW(L"uxtheme.dll");
    if (!ux) return;
    typedef int(WINAPI * set_mode_t)(int);
    typedef void(WINAPI * flush_t)(void);
    set_mode_t set_mode = (set_mode_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    flush_t flush = (flush_t)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set_mode) set_mode(dark ? 2 : 3); /* force dark / force light */
    if (flush) flush();
}

static void apply_editor_format(void)
{
    BOOL modified = (BOOL)SendMessageW(g_edit, EM_GETMODIFY, 0, 0);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_CHARSET | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE;
    cf.yHeight = 11 * 20;
    cf.crTextColor = P.doc_fg;
    cf.bCharSet = DEFAULT_CHARSET;
    cf.bPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcsncpy(cf.szFaceName, g_mono_family, LF_FACESIZE - 1);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);

    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof pf);
    pf.cbSize = sizeof pf;
    pf.dwMask = PFM_LINESPACING | PFM_TABSTOPS;
    pf.bLineSpacingRule = 5; /* dyLineSpacing / 20 lines */
    pf.dyLineSpacing = 27;
    pf.cTabCount = MAX_TAB_STOPS;
    for (int i = 0; i < MAX_TAB_STOPS; i++) pf.rgxTabs[i] = (i + 1) * g_tab_twips;
    SendMessageW(g_edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    SendMessageW(g_edit, EM_SETMODIFY, modified, 0);
}

static void render_preview(void);
static void update_tooltips(void);

/* Document colors for the black Night theme. */
static void night_colors(md_rtf_colors *c)
{
    c->text = 0xC8C8C8;
    c->heading = 0xE6E6E6;
    c->link = 0x6EA0F0;
    c->code_fg = 0xD0D0D0;
    c->code_bg = 0x141414;
    c->quote = 0x8C8C8C;
    c->rule = 0x2A2A2A;
    c->muted = 0x8C8C8C;
    c->table_head_bg = 0x111111;
    c->page_bg = 0x000000;
}

static void apply_theme(void)
{
    g_night = S.theme == THEME_NIGHT;
    g_dark = g_night || S.theme == THEME_DARK || (S.theme == THEME_SYSTEM && system_prefers_dark());
    if (g_night) {
        P.doc_bg = RGB(0, 0, 0);
        P.doc_fg = RGB(200, 200, 200);
        P.bar_bg = RGB(10, 10, 10);
        P.bar_border = RGB(30, 30, 30);
        P.bar_fg = RGB(214, 214, 214);
        P.bar_muted = RGB(128, 128, 128);
        P.hover = RGB(34, 34, 34);
        P.track = RGB(26, 26, 26);
        P.pill = RGB(52, 52, 52);
        P.pill_border = RGB(70, 70, 70);
        P.accent = RGB(96, 140, 220);
        P.drop_bg = RGB(12, 16, 26);
    } else if (g_dark) {
        P.doc_bg = RGB(22, 24, 29);
        P.doc_fg = RGB(215, 220, 226);
        P.bar_bg = RGB(30, 33, 39);
        P.bar_border = RGB(44, 48, 56);
        P.bar_fg = RGB(230, 237, 243);
        P.bar_muted = RGB(139, 148, 158);
        P.hover = RGB(46, 50, 59);
        P.track = RGB(42, 46, 54);
        P.pill = RGB(66, 72, 84);
        P.pill_border = RGB(80, 87, 100);
        P.accent = RGB(88, 150, 255);
        P.drop_bg = RGB(24, 31, 46);
    } else {
        P.doc_bg = RGB(255, 255, 255);
        P.doc_fg = RGB(31, 35, 40);
        P.bar_bg = RGB(246, 247, 249);
        P.bar_border = RGB(226, 229, 233);
        P.bar_fg = RGB(31, 35, 40);
        P.bar_muted = RGB(107, 114, 128);
        P.hover = RGB(232, 234, 238);
        P.track = RGB(229, 231, 235);
        P.pill = RGB(255, 255, 255);
        P.pill_border = RGB(209, 213, 219);
        P.accent = RGB(47, 111, 235);
        P.drop_bg = RGB(240, 245, 255);
    }
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_main, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    COLORREF caption = P.bar_bg, caption_text = P.bar_fg;
    DwmSetWindowAttribute(g_main, DWMWA_CAPTION_COLOR_, &caption, sizeof caption); /* Windows 11 */
    DwmSetWindowAttribute(g_main, DWMWA_TEXT_COLOR_, &caption_text, sizeof caption_text);
    set_menu_theme(g_dark);
    const WCHAR *theme = g_dark ? L"DarkMode_Explorer" : L"Explorer";
    SetWindowTheme(g_edit, theme, NULL);
    SetWindowTheme(g_prev, theme, NULL);
    SendMessageW(g_edit, EM_SETBKGNDCOLOR, 0, (LPARAM)P.doc_bg);
    SendMessageW(g_prev, EM_SETBKGNDCOLOR, 0, (LPARAM)P.doc_bg);
    apply_editor_format();
    tree_apply_theme();
    update_tooltips();
    InvalidateRect(g_main, NULL, FALSE);
    if (g_drop) InvalidateRect(g_drop, NULL, FALSE);
    g_preview_dirty = 1;
    if (is_read()) render_preview();
}

/* ------------------------------------------------------------------------ */
/* Anti-aliased drawing. Shapes are signed distance functions evaluated per */
/* pixel, then alpha-blended. Pure geometry, no image assets.               */
/* ------------------------------------------------------------------------ */

typedef float (*sdf_fn)(float x, float y, const void *ctx);

typedef struct rrect_t { float cx, cy, hw, hh, r; } rrect_t;
typedef struct ring_t { rrect_t box; float width; } ring_t;
typedef struct seg_t { float ax, ay, bx, by, r; } seg_t;
typedef struct icon_t { int kind; float cx, cy, s; } icon_t;


static float sd_rrect(float x, float y, const rrect_t *b)
{
    float qx = fabsf(x - b->cx) - (b->hw - b->r);
    float qy = fabsf(y - b->cy) - (b->hh - b->r);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float inside = qx > qy ? qx : qy;
    if (inside > 0) inside = 0;
    return sqrtf(ox * ox + oy * oy) + inside - b->r;
}

static float sd_seg(float x, float y, const seg_t *s)
{
    float px = x - s->ax, py = y - s->ay, dx = s->bx - s->ax, dy = s->by - s->ay;
    float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ex = px - dx * t, ey = py - dy * t;
    return sqrtf(ex * ex + ey * ey) - s->r;
}

static float sdf_rrect(float x, float y, const void *c) { return sd_rrect(x, y, (const rrect_t *)c); }

static float sdf_ring(float x, float y, const void *c)
{
    const ring_t *g = (const ring_t *)c;
    float d = sd_rrect(x, y, &g->box) + g->width / 2;
    return fabsf(d) - g->width / 2;
}

static float dmin(float a, float b) { return a < b ? a : b; }

/* Formatting toolbar icons, drawn from lines and boxes like the others. */
static float sdf_format_icon(float x, float y, const icon_t *ic)
{
    float s = ic->s, cx = ic->cx, cy = ic->cy, w = s * 0.06f, d = 1e9f;
    switch (ic->kind) {
    case ICON_BULLETS:
        for (int i = -1; i <= 1; i++) {
            float ly = cy + (float)i * s * 0.34f, dx = x - (cx - s * 0.36f), dy = y - ly;
            seg_t line = {cx - s * 0.08f, ly, cx + s * 0.48f, ly, w};
            d = dmin(d, dmin(sd_seg(x, y, &line), sqrtf(dx * dx + dy * dy) - s * 0.085f));
        }
        return d;
    case ICON_NUMBERS: {
        /* two rows, numbered 1 and 2 with strokes large enough to read */
        float l = cx - s * 0.5f, r = cx - s * 0.24f, h = s * 0.17f, t = s * 0.05f;
        float y1 = cy - s * 0.25f, y2 = cy + s * 0.25f;
        seg_t g[] = {
            {cx - s * 0.04f, y1, cx + s * 0.48f, y1, w}, {cx - s * 0.04f, y2, cx + s * 0.48f, y2, w},
            {(l + r) / 2 + s * 0.03f, y1 - h, (l + r) / 2 + s * 0.03f, y1 + h, t},
            {(l + r) / 2 + s * 0.03f, y1 - h, l + s * 0.04f, y1 - h * 0.45f, t},
            {l, y2 - h, r, y2 - h, t}, {r, y2 - h, r, y2 - h * 0.1f, t},
            {r, y2 - h * 0.1f, l, y2 + h, t}, {l, y2 + h, r, y2 + h, t},
        };
        for (int i = 0; i < (int)(sizeof g / sizeof g[0]); i++) d = dmin(d, sd_seg(x, y, &g[i]));
        return d;
    }
    case ICON_TASK: {
        ring_t box = {{cx, cy, s * 0.38f, s * 0.38f, s * 0.1f}, w};
        seg_t a = {cx - s * 0.2f, cy + s * 0.01f, cx - s * 0.05f, cy + s * 0.16f, w};
        seg_t b = {cx - s * 0.05f, cy + s * 0.16f, cx + s * 0.22f, cy - s * 0.16f, w};
        return dmin(sdf_ring(x, y, &box), dmin(sd_seg(x, y, &a), sd_seg(x, y, &b)));
    }
    case ICON_QUOTE: {
        seg_t bar = {cx - s * 0.4f, cy - s * 0.38f, cx - s * 0.4f, cy + s * 0.38f, s * 0.075f};
        d = sd_seg(x, y, &bar);
        for (int i = -1; i <= 1; i++) {
            float ly = cy + (float)i * s * 0.3f;
            seg_t line = {cx - s * 0.14f, ly, cx + (i == 1 ? s * 0.22f : s * 0.46f), ly, w};
            d = dmin(d, sd_seg(x, y, &line));
        }
        return d;
    }
    case ICON_LINK: {
        /* two chain links rising to the right */
        float k = 0.70710678f, px = (x - cx) * k - (y - cy) * k, py = (x - cx) * k + (y - cy) * k;
        ring_t a = {{-s * 0.2f, 0, s * 0.3f, s * 0.17f, s * 0.17f}, s * 0.075f};
        ring_t b = {{s * 0.2f, 0, s * 0.3f, s * 0.17f, s * 0.17f}, s * 0.075f};
        return dmin(sdf_ring(px, py, &a), sdf_ring(px, py, &b));
    }
    default: { /* ICON_HR: a rule between two lines of text */
        seg_t top = {cx - s * 0.3f, cy - s * 0.34f, cx + s * 0.3f, cy - s * 0.34f, s * 0.04f};
        seg_t mid = {cx - s * 0.48f, cy, cx + s * 0.48f, cy, s * 0.075f};
        seg_t bot = {cx - s * 0.3f, cy + s * 0.34f, cx + s * 0.3f, cy + s * 0.34f, s * 0.04f};
        return dmin(sd_seg(x, y, &mid), dmin(sd_seg(x, y, &top), sd_seg(x, y, &bot)));
    }
    }
}

static float sdf_icon(float x, float y, const void *c)
{
    const icon_t *ic = (const icon_t *)c;
    float s = ic->s, cx = ic->cx, cy = ic->cy;
    if (ic->kind == ICON_MENU) {
        float d = 1e9f;
        for (int i = -1; i <= 1; i++) {
            seg_t line = {cx - s * 0.5f, cy + i * s * 0.32f, cx + s * 0.5f, cy + i * s * 0.32f, s * 0.065f};
            float k = sd_seg(x, y, &line);
            if (k < d) d = k;
        }
        return d;
    }
    if (ic->kind == ICON_SUN) {
        float dx = x - cx, dy = y - cy;
        float d = sqrtf(dx * dx + dy * dy) - s * 0.22f;
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 3.14159265f / 4.0f;
            seg_t ray = {cx + cosf(a) * s * 0.36f, cy + sinf(a) * s * 0.36f, cx + cosf(a) * s * 0.47f,
                         cy + sinf(a) * s * 0.47f, s * 0.055f};
            float k = sd_seg(x, y, &ray);
            if (k < d) d = k;
        }
        return d;
    }
    if (ic->kind == ICON_HALF) {
        /* left: half a sun with three rays; right: half a crescent moon */
        float dx = x - cx, dy = y - cy;
        float r = sqrtf(dx * dx + dy * dy);
        float sun = r - s * 0.25f;
        float left = dx + s * 0.05f;
        if (left > sun) sun = left;
        for (int i = 0; i < 3; i++) {
            float a = 3.14159265f * (0.75f + 0.25f * (float)i);
            seg_t ray = {cx + cosf(a) * s * 0.36f, cy + sinf(a) * s * 0.36f, cx + cosf(a) * s * 0.47f,
                         cy + sinf(a) * s * 0.47f, s * 0.055f};
            float k = sd_seg(x, y, &ray);
            if (k < sun) sun = k;
        }
        /* right half: the outer edge of a crescent moon */
        float outer = r - s * 0.36f;
        float ex = x - (cx - s * 0.02f), ey = y - cy;
        float cut = sqrtf(ex * ex + ey * ey) - s * 0.20f;
        float moon = outer > -cut ? outer : -cut;
        float right = -dx + s * 0.05f; /* a thin gap between the halves */
        if (right > moon) moon = right;
        return sun < moon ? sun : moon;
    }
    if (ic->kind == ICON_PLUS || ic->kind == ICON_CLOSE) {
        float k = ic->kind == ICON_PLUS ? 0.42f : 0.30f, w = s * 0.07f;
        seg_t a = {cx - s * k, cy, cx + s * k, cy, w}, b = {cx, cy - s * k, cx, cy + s * k, w};
        if (ic->kind == ICON_CLOSE) {
            seg_t d1 = {cx - s * k, cy - s * k, cx + s * k, cy + s * k, w};
            seg_t d2 = {cx - s * k, cy + s * k, cx + s * k, cy - s * k, w};
            a = d1;
            b = d2;
        }
        float da = sd_seg(x, y, &a), db = sd_seg(x, y, &b);
        return da < db ? da : db;
    }
    if (ic->kind == ICON_SIDEBAR || ic->kind == ICON_SIDEBAR_ON) {
        /* a window outline with a left column, filled while the sidebar is open */
        rrect_t box = {cx, cy, s * 0.46f, s * 0.38f, s * 0.12f};
        ring_t frame = {box, s * 0.075f};
        float d = sdf_ring(x, y, &frame);
        seg_t bar = {cx - s * 0.14f, cy - s * 0.34f, cx - s * 0.14f, cy + s * 0.34f, s * 0.04f};
        float k = sd_seg(x, y, &bar);
        if (k < d) d = k;
        if (ic->kind == ICON_SIDEBAR_ON) {
            rrect_t col = {cx - s * 0.29f, cy, s * 0.15f, s * 0.36f, s * 0.06f};
            k = sd_rrect(x, y, &col);
            if (k < d) d = k;
        }
        return d;
    }
    if (ic->kind == ICON_CHEVRON) {
        float w = s * 0.07f;
        seg_t a = {cx - s * 0.3f, cy - s * 0.12f, cx, cy + s * 0.16f, w}, b = {cx + s * 0.3f, cy - s * 0.12f, cx, cy + s * 0.16f, w};
        float da = sd_seg(x, y, &a), db = sd_seg(x, y, &b);
        return da < db ? da : db;
    }
    if (ic->kind == ICON_SAVE) {
        /* a tray with a downward arrow */
        float w = s * 0.06f, d = 1e9f, k;
        seg_t parts[5] = {
            {cx, cy - s * 0.42f, cx, cy + s * 0.12f, w},
            {cx - s * 0.2f, cy - s * 0.08f, cx, cy + s * 0.12f, w},
            {cx + s * 0.2f, cy - s * 0.08f, cx, cy + s * 0.12f, w},
            {cx - s * 0.42f, cy + s * 0.12f, cx - s * 0.42f, cy + s * 0.38f, w},
            {cx + s * 0.42f, cy + s * 0.12f, cx + s * 0.42f, cy + s * 0.38f, w},
        };
        for (int i = 0; i < 5; i++)
            if ((k = sd_seg(x, y, &parts[i])) < d) d = k;
        seg_t base = {cx - s * 0.42f, cy + s * 0.38f, cx + s * 0.42f, cy + s * 0.38f, w};
        if ((k = sd_seg(x, y, &base)) < d) d = k;
        return d;
    }
    if (ic->kind >= ICON_BULLETS) return sdf_format_icon(x, y, ic);
    /* moon: a disc with an offset disc cut away */
    float dx = x - cx, dy = y - cy;
    float outer = sqrtf(dx * dx + dy * dy) - s * 0.42f;
    float ex = x - (cx + s * 0.22f), ey = y - (cy - s * 0.18f);
    float cut = sqrtf(ex * ex + ey * ey) - s * 0.34f;
    return outer > -cut ? outer : -cut;
}

static void aa_draw(HDC dc, const RECT *box, sdf_fn f, const void *ctx, COLORREF col, int alpha)
{
    int w = box->right - box->left, h = box->bottom - box->top;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) return;
    HDC mdc = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mdc, bmp);
    DWORD *px = (DWORD *)bits;
    unsigned r = GetRValue(col), g = GetGValue(col), b = GetBValue(col);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float d = f((float)x + 0.5f, (float)y + 0.5f, ctx);
            float cov = 0.5f - d;
            if (cov < 0) cov = 0;
            if (cov > 1) cov = 1;
            unsigned a = (unsigned)(cov * (float)alpha + 0.5f);
            px[y * w + x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
        }
    }
    BLENDFUNCTION bf = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    AlphaBlend(dc, box->left, box->top, w, h, mdc, 0, 0, w, h, bf);
    SelectObject(mdc, old);
    DeleteDC(mdc);
    DeleteObject(bmp);
}

static void fill_round(HDC dc, const RECT *r, float radius, COLORREF col)
{
    rrect_t s = {(float)(r->right - r->left) / 2, (float)(r->bottom - r->top) / 2, (float)(r->right - r->left) / 2,
                 (float)(r->bottom - r->top) / 2, radius};
    aa_draw(dc, r, sdf_rrect, &s, col, 255);
}

static void stroke_round(HDC dc, const RECT *r, float radius, float width, COLORREF col)
{
    ring_t s = {{(float)(r->right - r->left) / 2, (float)(r->bottom - r->top) / 2, (float)(r->right - r->left) / 2,
                 (float)(r->bottom - r->top) / 2, radius},
                width};
    aa_draw(dc, r, sdf_ring, &s, col, 255);
}

static void draw_icon(HDC dc, const RECT *r, int kind, float size, COLORREF col)
{
    icon_t ic = {kind, (float)(r->right - r->left) / 2, (float)(r->bottom - r->top) / 2, size};
    aa_draw(dc, r, sdf_icon, &ic, col, 255);
}

static void fill_rect(HDC dc, const RECT *r, COLORREF col)
{
    HBRUSH b = CreateSolidBrush(col);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void draw_text(HDC dc, const WCHAR *t, RECT *r, HFONT font, COLORREF col, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);
    DrawTextW(dc, t, -1, r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

static int text_width(HDC dc, const WCHAR *t, HFONT font)
{
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

/* ------------------------------------------------------------------------ */
/* Window chrome: header (menu button, title, version) and status bar       */
/* (status text, zoom, Edit/Read switch, theme button).                     */
/* ------------------------------------------------------------------------ */

static const WCHAR *tab_name(int i)
{
    const doc_tab *t = &g_tabs[i];
    int has = i == g_cur ? g_has_path : t->has_path;
    return has ? util_path_name(i == g_cur ? g_path : t->path) : L"Untitled";
}

static int tab_modified(int i)
{
    HWND e = i == g_cur ? g_edit : g_tabs[i].edit;
    return e && (SendMessageW(e, EM_GETMODIFY, 0, 0) || (i == g_cur && g_raw_line >= 0 && SendMessageW(g_edit, EM_GETMODIFY, 0, 0)));
}

/* Places the tabs between the menu button and the Save as button. When they
 * do not fit, they shrink; when even that is not enough, the strip scrolls
 * so that the active tab stays visible. */
static void layout_tabs(HDC dc)
{
    int x0 = g_rc_sidebtn.right + S_(6), x1 = g_rc_save.left - S_(22) - text_width(dc, L"v" DOMDLY_VERSION_WSTR, g_font_tiny) - S_(10);
    int h = S_(30), top = (g_rc_top.bottom - h) / 2;
    int widths[MAX_TABS], total = 0;
    for (int i = 0; i < g_ntabs; i++) {
        int w = text_width(dc, tab_name(i), g_font_bold) + S_(50);
        if (w < S_(96)) w = S_(96);
        if (w > S_(220)) w = S_(220);
        widths[i] = w;
        total += w + S_(4);
    }
    int avail = x1 - x0;
    if (total > avail && g_ntabs > 0) {
        int each = avail / g_ntabs - S_(4);
        if (each < S_(72)) each = S_(72);
        total = 0;
        for (int i = 0; i < g_ntabs; i++) {
            if (widths[i] > each) widths[i] = each;
            total += widths[i] + S_(4);
        }
    }
    /* scroll so that the active tab is inside [x0, x1] */
    int before = 0;
    for (int i = 0; i < g_cur && i < g_ntabs; i++) before += widths[i] + S_(4);
    int shift = 0;
    if (g_cur >= 0 && g_cur < g_ntabs && before + widths[g_cur] > avail) shift = before + widths[g_cur] - avail;
    int x = x0 - shift;
    for (int i = 0; i < g_ntabs; i++) {
        SetRect(&g_rc_tab[i], x, top, x + widths[i], top + h);
        if (x < x0 || x + widths[i] > x1) SetRectEmpty(&g_rc_tab[i]);
        RECT *xr = &g_rc_tabx[i];
        SetRect(xr, x + widths[i] - S_(28), top + S_(5), x + widths[i] - S_(6), top + h - S_(5));
        if (IsRectEmpty(&g_rc_tab[i])) SetRectEmpty(xr);
        x += widths[i] + S_(4);
    }
    int nx = x < x1 ? x : x1;
    if (nx < x0) nx = x0;
    SetRect(&g_rc_newtab, nx, top, nx + S_(30), top + h);
}

static void paint_chrome(HDC dc)
{
    RECT line;
    /* header */
    fill_rect(dc, &g_rc_top, P.bar_bg);
    line = g_rc_top;
    line.top = line.bottom - 1;
    fill_rect(dc, &line, P.bar_border);

    if (g_hot == HOT_MENU || g_pressed == HOT_MENU) fill_round(dc, &g_rc_menu, SF(6), P.hover);
    draw_icon(dc, &g_rc_menu, ICON_MENU, SF(16), P.bar_fg);
    if (g_hot == HOT_SIDEBTN) fill_round(dc, &g_rc_sidebtn, SF(6), P.hover);
    draw_icon(dc, &g_rc_sidebtn, S.tree_open ? ICON_SIDEBAR_ON : ICON_SIDEBAR, SF(16),
              S.tree_open || g_hot == HOT_SIDEBTN ? P.bar_fg : P.bar_muted);

    /* files sidebar: folder name on top, the tree view below it */
    if (S.tree_open && g_rc_side.right > g_rc_side.left) {
        fill_rect(dc, &g_rc_side, P.bar_bg);
        line = g_rc_side;
        line.left = line.right - 1;
        fill_rect(dc, &line, g_tree_drag ? P.accent : P.bar_border);
        RECT hr = g_rc_sidehead;
        InflateRect(&hr, -S_(6), -S_(4));
        if (g_hot == HOT_SIDEHEAD) fill_round(dc, &hr, SF(6), P.hover);
        RECT cr = hr;
        cr.left = cr.right - S_(22);
        draw_icon(dc, &cr, ICON_CHEVRON, SF(12), P.bar_muted);
        RECT nr = hr;
        nr.left += S_(10);
        nr.right = cr.left;
        draw_text(dc, g_tree_root[0] ? util_path_name(g_tree_root) : L"Files", &nr, g_font_bold, P.bar_fg,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (!g_tree_root[0]) {
            RECT br = g_rc_side;
            br.top = g_rc_sidehead.bottom + S_(8);
            br.left += S_(16);
            br.right -= S_(16);
            draw_text(dc, L"Open a file or choose a folder", &br, g_font_ui, P.bar_muted, DT_WORDBREAK);
        }
    }

    WCHAR ver[32];
    swprintf(ver, 32, L"v%s", DOMDLY_VERSION_WSTR);
    RECT vr = g_rc_top;
    vr.right = g_rc_save.left - S_(14);
    vr.left = vr.right - S_(80);

    /* Save as button (Markdown or PDF) */
    if (g_hot == HOT_SAVE || g_pressed == HOT_SAVE) fill_round(dc, &g_rc_save, SF(7), P.hover);
    stroke_round(dc, &g_rc_save, SF(7), SF(1), P.bar_border);
    RECT si = g_rc_save;
    si.left += S_(8);
    si.right = si.left + S_(18);
    draw_icon(dc, &si, ICON_SAVE, SF(14), P.bar_fg);
    RECT st = g_rc_save;
    st.left = si.right + S_(6);
    draw_text(dc, L"Save as", &st, g_font_ui, P.bar_fg, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    /* the version is a small link to the project on GitHub */
    int vw = text_width(dc, ver, g_font_tiny);
    SetRect(&g_rc_ver, vr.right - vw - S_(6), (vr.top + vr.bottom) / 2 - S_(10), vr.right + S_(6), (vr.top + vr.bottom) / 2 + S_(10));
    if (g_hot == HOT_VER) fill_round(dc, &g_rc_ver, SF(5), P.hover);
    draw_text(dc, ver, &vr, g_font_tiny, g_hot == HOT_VER ? P.bar_fg : P.bar_muted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

    /* tabs */
    layout_tabs(dc);
    for (int i = 0; i < g_ntabs; i++) {
        RECT r = g_rc_tab[i];
        if (r.right <= r.left) continue;
        int active = i == g_cur, hot = g_hot == HOT_TAB + i || g_hot == HOT_TABX + i;
        if (active) {
            fill_round(dc, &r, SF(8), P.doc_bg);
            stroke_round(dc, &r, SF(8), SF(1), P.bar_border);
        } else if (hot) {
            fill_round(dc, &r, SF(8), P.hover);
        }
        RECT nr = r;
        nr.left += S_(12);
        nr.right = g_rc_tabx[i].left - S_(2);
        draw_text(dc, tab_name(i), &nr, active ? g_font_bold : g_font_ui, active ? P.bar_fg : P.bar_muted,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT xr = g_rc_tabx[i];
        if (hot || (active && !tab_modified(i))) {
            if (g_hot == HOT_TABX + i) fill_round(dc, &xr, SF(5), active ? P.hover : P.track);
            draw_icon(dc, &xr, ICON_CLOSE, SF(13), hot || active ? P.bar_fg : P.bar_muted);
        } else if (tab_modified(i)) {
            int d = S_(8);
            RECT dot = {(xr.left + xr.right - d) / 2, (xr.top + xr.bottom - d) / 2, (xr.left + xr.right + d) / 2,
                        (xr.top + xr.bottom + d) / 2};
            fill_round(dc, &dot, (float)d / 2, P.accent);
        }
    }
    if (g_hot == HOT_NEWTAB) fill_round(dc, &g_rc_newtab, SF(7), P.hover);
    draw_icon(dc, &g_rc_newtab, ICON_PLUS, SF(14), P.bar_fg);

    /* formatting toolbar */
    if (!IsRectEmpty(&g_rc_fmt)) {
        fill_rect(dc, &g_rc_fmt, P.doc_bg);
        line = g_rc_fmt;
        line.top = line.bottom - 1;
        fill_rect(dc, &line, P.bar_border);
        for (int i = 0; i < FMT_COUNT; i++) {
            RECT r = g_rc_fbtn[i];
            if (IsRectEmpty(&r)) continue;
            if (!g_fmt[i].id) { /* a thin divider between groups */
                RECT dv = {(r.left + r.right) / 2, r.top + S_(6), (r.left + r.right) / 2 + 1, r.bottom - S_(6)};
                fill_rect(dc, &dv, P.bar_border);
                continue;
            }
            int hot = g_hot == HOT_FMT + i, down = hot && g_pressed == HOT_FMT + i;
            if (hot) fill_round(dc, &r, SF(6), down ? P.track : P.hover);
            COLORREF fg = hot ? P.bar_fg : P.bar_muted;
            if (g_fmt[i].look == FB_ICON) {
                draw_icon(dc, &r, g_fmt[i].icon, SF(16), fg);
            } else {
                HFONT f = g_fmt[i].look == FB_BOLD     ? g_font_fb
                          : g_fmt[i].look == FB_ITALIC ? g_font_fi
                          : g_fmt[i].look == FB_STRIKE ? g_font_fs
                          : g_fmt[i].look == FB_MONO   ? g_font_fm
                                                       : g_font_fh;
                draw_text(dc, g_fmt[i].text, &r, f, fg, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            }
        }
    }

    /* status bar */
    fill_rect(dc, &g_rc_bottom, P.bar_bg);
    line = g_rc_bottom;
    line.bottom = line.top + 1;
    fill_rect(dc, &line, P.bar_border);

    RECT sr = g_rc_bottom;
    sr.left += S_(16);
    sr.right = g_rc_zoom.left - S_(8);
    /* While the pointer rests on a status bar control, its shortcut is shown
     * here at once (the tooltip follows after the usual delay). */
    const WCHAR *hint = NULL;
    switch (g_hot) {
    case HOT_SAVE: hint = L"Save as Markdown or PDF: Ctrl+Shift+S   \x00B7   Save: Ctrl+S"; break;
    case HOT_ZOOM: hint = L"Zoom: Ctrl+Plus / Ctrl+Minus / Ctrl+0 (or Ctrl+wheel)"; break;
    case HOT_EDIT: hint = L"Edit view: Ctrl+E switches between Edit and Read"; break;
    case HOT_READ: hint = L"Read view: Ctrl+E switches between Edit and Read"; break;
    case HOT_THEME:
        hint = g_night ? L"Night theme. Next: Light (Ctrl+Shift+L)"
               : g_dark ? L"Dark navy theme. Next: Night (Ctrl+Shift+L)"
                        : L"Light theme. Next: Dark navy (Ctrl+Shift+L)";
        break;
    }
    if (g_hot == HOT_SIDEBTN) hint = S.tree_open ? L"Hide the files sidebar: Ctrl+Shift+B" : L"Show the files sidebar: Ctrl+Shift+B";
    if (g_hot == HOT_SIDEHEAD) hint = g_tree_root[0] ? g_tree_root : L"Choose a folder";
    if (g_hot == HOT_VER) hint = L"Open Domdly on GitHub   \x00B7   Help menu: Check for Updates on GitHub";
    if (g_hot == HOT_SPLIT) hint = L"Drag to resize the sidebar";
    if (g_hot >= HOT_FMT && g_hot < HOT_FMT + FMT_COUNT) hint = g_fmt[g_hot - HOT_FMT].tip;
    if (g_hot == HOT_NEWTAB) hint = L"New tab: Ctrl+T   \x00B7   Next / previous tab: Ctrl+Tab / Ctrl+Shift+Tab";
    if (g_hot >= HOT_TABX && g_hot < HOT_TABX + g_ntabs) hint = L"Close tab: Ctrl+W (or middle-click the tab)   \x00B7   Closing the last empty tab closes the window";
    if (g_hot >= HOT_TAB && g_hot < HOT_TAB + g_ntabs) {
        int i = g_hot - HOT_TAB;
        int has = i == g_cur ? g_has_path : g_tabs[i].has_path;
        hint = has ? (i == g_cur ? g_path : g_tabs[i].path) : L"Untitled document (not saved yet)";
    }
    /* The full path of the open file comes first; a long one is shortened in
     * the middle so that the folder and the file name both stay readable.
     * Clicking it shows the file in Explorer. */
    SetRectEmpty(&g_rc_path);
    if (!hint && g_has_path) {
        int avail = sr.right - sr.left;
        int pw = text_width(dc, g_path, g_font_ui);
        if (pw > avail / 2) pw = avail / 2;
        int cy = (sr.top + sr.bottom) / 2;
        SetRect(&g_rc_path, sr.left - S_(6), cy - S_(11), sr.left + pw + S_(6), cy + S_(11));
        if (g_hot == HOT_PATH) fill_round(dc, &g_rc_path, SF(5), P.hover);
        RECT pr = sr;
        pr.right = pr.left + pw;
        draw_text(dc, g_path, &pr, g_font_ui, g_hot == HOT_PATH ? P.bar_fg : P.bar_muted,
                  DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS);
        sr.left = g_rc_path.right + S_(6);
        draw_text(dc, L"\x00B7", &sr, g_font_ui, P.bar_muted, DT_SINGLELINE | DT_VCENTER);
        sr.left += text_width(dc, L"\x00B7", g_font_ui) + S_(10);
        if (g_hot == HOT_PATH) hint = L"Click to show this file in its folder";
    }
    draw_text(dc, hint ? hint : g_status, &sr, hint ? g_font_bold : g_font_ui, hint ? P.bar_fg : P.bar_muted,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    WCHAR zoom[16];
    swprintf(zoom, 16, L"%d%%", S.zoom);
    if (g_hot == HOT_ZOOM) fill_round(dc, &g_rc_zoom, SF(6), P.hover);
    RECT zr = g_rc_zoom;
    draw_text(dc, zoom, &zr, g_font_ui, P.bar_muted, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    /* Edit / Read segmented switch */
    float h = (float)(g_rc_seg.bottom - g_rc_seg.top);
    fill_round(dc, &g_rc_seg, h / 2, P.track);
    RECT sel = is_read() ? g_rc_read : g_rc_edit;
    InflateRect(&sel, -S_(2), -S_(2));
    fill_round(dc, &sel, (float)(sel.bottom - sel.top) / 2, P.pill);
    stroke_round(dc, &sel, (float)(sel.bottom - sel.top) / 2, SF(1), P.pill_border);
    RECT er = g_rc_edit, rr = g_rc_read;
    draw_text(dc, L"Edit", &er, is_read() ? g_font_ui : g_font_bold,
              is_read() ? (g_hot == HOT_EDIT ? P.bar_fg : P.bar_muted) : P.bar_fg, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    draw_text(dc, L"Read", &rr, is_read() ? g_font_bold : g_font_ui,
              is_read() ? P.bar_fg : (g_hot == HOT_READ ? P.bar_fg : P.bar_muted), DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    /* theme button: sun = light, half sun half moon = dark navy, moon = night */
    if (g_hot == HOT_THEME || g_pressed == HOT_THEME) fill_round(dc, &g_rc_theme, SF(6), P.hover);
    draw_icon(dc, &g_rc_theme, g_night ? ICON_MOON : g_dark ? ICON_HALF : ICON_SUN, g_dark && !g_night ? SF(20) : SF(17),
              P.bar_fg);
}

static int hit_test(int x, int y)
{
    POINT pt = {x, y};
    if (PtInRect(&g_rc_menu, pt)) return HOT_MENU;
    if (PtInRect(&g_rc_sidebtn, pt)) return HOT_SIDEBTN;
    if (S.tree_open && PtInRect(&g_rc_split, pt)) return HOT_SPLIT;
    if (S.tree_open && PtInRect(&g_rc_sidehead, pt)) return HOT_SIDEHEAD;
    if (PtInRect(&g_rc_top, pt)) {
        HDC dc = GetDC(g_main);
        layout_tabs(dc);
        ReleaseDC(g_main, dc);
        for (int i = 0; i < g_ntabs; i++) {
            if (PtInRect(&g_rc_tabx[i], pt)) return HOT_TABX + i;
            if (PtInRect(&g_rc_tab[i], pt)) return HOT_TAB + i;
        }
        if (PtInRect(&g_rc_newtab, pt)) return HOT_NEWTAB;
    }
    if (PtInRect(&g_rc_save, pt)) return HOT_SAVE;
    if (PtInRect(&g_rc_ver, pt)) return HOT_VER;
    if (PtInRect(&g_rc_path, pt)) return HOT_PATH;
    if (PtInRect(&g_rc_fmt, pt))
        for (int i = 0; i < FMT_COUNT; i++)
            if (g_fmt[i].id && PtInRect(&g_rc_fbtn[i], pt)) return HOT_FMT + i;
    if (PtInRect(&g_rc_zoom, pt)) return HOT_ZOOM;
    if (PtInRect(&g_rc_edit, pt)) return HOT_EDIT;
    if (PtInRect(&g_rc_read, pt)) return HOT_READ;
    if (PtInRect(&g_rc_theme, pt)) return HOT_THEME;
    return HOT_NONE;
}

static void invalidate_chrome(void)
{
    InvalidateRect(g_main, &g_rc_top, FALSE);
    InvalidateRect(g_main, &g_rc_bottom, FALSE);
    InvalidateRect(g_main, &g_rc_fmt, FALSE);
    if (S.tree_open) InvalidateRect(g_main, &g_rc_side, FALSE);
}

static void add_tool(int id, const RECT *r, const WCHAR *text)
{
    TOOLINFOW ti;
    memset(&ti, 0, sizeof ti);
    ti.cbSize = sizeof ti;
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = g_main;
    ti.uId = (UINT_PTR)id;
    ti.rect = *r;
    TOOLINFOW old = ti;
    old.lpszText = NULL;
    if (SendMessageW(g_tip, TTM_GETTOOLINFOW, 0, (LPARAM)&old)) {
        /* TTM_NEWTOOLRECT returns nothing, so ask first instead of adding the
         * same tool again on every layout */
        ti.lpszText = (LPWSTR)text;
        SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    } else {
        ti.lpszText = (LPWSTR)text;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
}

static void update_tooltips(void)
{
    if (!g_tip) return;
    add_tool(HOT_MENU, &g_rc_menu, L"Menu (Alt or F10)");
    add_tool(HOT_SIDEBTN, &g_rc_sidebtn, L"Files sidebar (Ctrl+Shift+B)");
    for (int i = 0; i < FMT_COUNT; i++)
        if (g_fmt[i].id) add_tool(HOT_FMT + i, &g_rc_fbtn[i], g_fmt[i].tip);
    add_tool(HOT_SAVE, &g_rc_save, L"Save a copy as Markdown or PDF (Ctrl+Shift+S)");
    add_tool(HOT_ZOOM, &g_rc_zoom, L"Zoom with Ctrl+Plus / Ctrl+Minus; click to reset (Ctrl+0)");
    add_tool(HOT_EDIT, &g_rc_edit, L"Edit the Markdown source (Ctrl+E)");
    add_tool(HOT_READ, &g_rc_read, L"Read and edit the formatted document (Ctrl+E)");
    add_tool(HOT_THEME, &g_rc_theme,
             g_night ? L"Night theme; click for Light (Ctrl+Shift+L)"
             : g_dark ? L"Dark navy theme; click for Night (Ctrl+Shift+L)"
                      : L"Light theme; click for Dark navy (Ctrl+Shift+L)");
}

/* ------------------------------------------------------------------------ */
/* Layout                                                                   */
/* ------------------------------------------------------------------------ */

static void set_inner_rect(HWND h, int pad_x, int pad_y)
{
    RECT rc;
    GetClientRect(h, &rc);
    rc.left += pad_x;
    rc.right -= pad_x;
    rc.top += pad_y;
    rc.bottom -= pad_y;
    if (rc.right <= rc.left) rc.right = rc.left + 1;
    if (rc.bottom <= rc.top) rc.bottom = rc.top + 1;
    SendMessageW(h, EM_SETRECT, 0, (LPARAM)&rc);
}

static void layout(void)
{
    RECT rc;
    GetClientRect(g_main, &rc);
    int w = rc.right, h = rc.bottom;
    int th = S_(42), bh = S_(36);
    SetRect(&g_rc_top, 0, 0, w, th);
    SetRect(&g_rc_bottom, 0, h - bh, w, h);
    int mh = S_(30);
    SetRect(&g_rc_menu, S_(8), (th - mh) / 2, S_(8) + S_(36), (th - mh) / 2 + mh);
    SetRect(&g_rc_sidebtn, g_rc_menu.right + S_(2), g_rc_menu.top, g_rc_menu.right + S_(2) + S_(34), g_rc_menu.bottom);
    int sv = S_(28);
    SetRect(&g_rc_save, w - S_(16) - S_(94), (th - sv) / 2, w - S_(16), (th - sv) / 2 + sv);

    int by = g_rc_bottom.top + 1;
    int bb = h;
    int ch = S_(26);
    int cy = by + (bb - by - ch) / 2;
    SetRect(&g_rc_theme, w - S_(10) - S_(34), cy, w - S_(10), cy + ch);
    int sw = S_(124), sh = S_(26);
    SetRect(&g_rc_seg, g_rc_theme.left - S_(8) - sw, by + (bb - by - sh) / 2, g_rc_theme.left - S_(8),
            by + (bb - by - sh) / 2 + sh);
    g_rc_edit = g_rc_seg;
    g_rc_edit.right = (g_rc_seg.left + g_rc_seg.right) / 2;
    g_rc_read = g_rc_seg;
    g_rc_read.left = g_rc_edit.right;
    SetRect(&g_rc_zoom, g_rc_seg.left - S_(8) - S_(52), cy, g_rc_seg.left - S_(8), cy + ch);

    int fh = S.fmt_bar ? S_(38) : 0; /* formatting toolbar under the tabs */
    int top = th, height = h - th - bh;
    if (height < 0) height = 0;

    /* files sidebar on the left; the document takes the rest */
    int side_w = 0;
    if (S.tree_open) {
        side_w = S_(S.tree_width);
        if (side_w < S_(160)) side_w = S_(160);
        if (side_w > w / 2) side_w = w / 2;
    }
    SetRect(&g_rc_side, 0, top, side_w, top + height);
    SetRect(&g_rc_sidehead, 0, top, side_w, top + (side_w ? S_(40) : 0));
    SetRect(&g_rc_split, side_w - S_(6), top, side_w, top + height);
    if (g_tree) {
        int ty = g_rc_sidehead.bottom, tw = side_w - S_(6);
        MoveWindow(g_tree, 0, ty, tw > 0 ? tw : 0, top + height - ty, TRUE);
        ShowWindow(g_tree, S.tree_open && g_tree_root[0] ? SW_SHOWNA : SW_HIDE);
    }
    w -= side_w;

    int max_col = S_(860);
    int pad = w > max_col + S_(64) ? (w - max_col) / 2 : S_(32);
    SetRectEmpty(&g_rc_fmt);
    for (int i = 0; i < FMT_COUNT; i++) SetRectEmpty(&g_rc_fbtn[i]);
    if (fh && height > fh) {
        /* the buttons line up with the left edge of the text column */
        SetRect(&g_rc_fmt, side_w, top, side_w + w, top + fh);
        int x = side_w + pad - S_(8), bw = S_(32), bh2 = S_(28), by2 = top + (fh - bh2) / 2;
        for (int i = 0; i < FMT_COUNT; i++) {
            int cw = g_fmt[i].id ? bw : S_(13);
            if (x + cw > side_w + w - S_(8)) break; /* a narrow window shows what fits */
            SetRect(&g_rc_fbtn[i], x, by2, x + cw, by2 + bh2);
            x += cw + S_(2);
        }
        top += fh;
        height -= fh;
    }

    MoveWindow(g_edit, side_w, top, w, height, !is_read());
    MoveWindow(g_prev, side_w, top, w, height, is_read());
    ShowWindow(is_read() ? g_edit : g_prev, SW_HIDE);
    ShowWindow(is_read() ? g_prev : g_edit, SW_SHOWNA);

    set_inner_rect(g_edit, pad, S_(24));
    set_inner_rect(g_prev, pad, S_(24));
    if (g_drop) MoveWindow(g_drop, side_w, top, w, height, TRUE);

    if (w - 2 * pad != g_last_render_width) {
        g_preview_dirty = 1;
        if (is_read()) SetTimer(g_main, TIMER_RESIZE, 120, NULL);
    }
    update_tooltips();
    InvalidateRect(g_main, NULL, FALSE);
}

/* ------------------------------------------------------------------------ */
/* Status and title                                                         */
/* ------------------------------------------------------------------------ */

static void update_title(void)
{
    WCHAR title[DOMDLY_PATH_MAX + 64];
    BOOL modified = (BOOL)SendMessageW(g_edit, EM_GETMODIFY, 0, 0);
    swprintf(title, DOMDLY_PATH_MAX + 64, L"%s%s - Domdly", modified ? L"\x25CF " : L"",
             g_has_path ? util_path_name(g_path) : L"Untitled");
    SetWindowTextW(g_main, title);
    InvalidateRect(g_main, &g_rc_top, FALSE);
}

static void update_status(void)
{
    if (is_read()) {
        int minutes = (g_words + 199) / 200;
        swprintf(g_status, 256, L"%d word%s   \x00B7   %d min read   \x00B7   %s", g_words, g_words == 1 ? L"" : L"s",
                 minutes < 1 ? 1 : minutes,
                 g_raw_line >= 0 ? L"Editing this block as Markdown, Esc to finish"
                                 : L"Edit right here   \x00B7   Ctrl+click opens a link   \x00B7   Ctrl+E shows the Markdown");
    } else {
        CHARRANGE cr = get_sel(g_edit);
        LONG start;
        long line;
        para_at(cr.cpMax, &start, NULL, &line);
        swprintf(g_status, 256, L"Ln %ld, Col %ld   \x00B7   %d word%s   \x00B7   %s   \x00B7   %s", line + 1,
                 cr.cpMax - start + 1, g_words, g_words == 1 ? L"" : L"s", util_encoding_name(g_enc),
                 g_crlf ? L"CRLF" : L"LF");
    }
    InvalidateRect(g_main, &g_rc_bottom, FALSE);
}

static int count_words(const char *s, int n)
{
    int words = 0, in = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        int sp = c == ' ' || c == '\n' || c == '\t' || c == '\r';
        if (!sp && !in) words++;
        in = !sp;
    }
    return words;
}

static void update_word_count(void)
{
    int len = 0;
    char *t = editor_utf8(&len);
    if (!t) return;
    g_words = count_words(t, len);
    free(t);
}

/* ------------------------------------------------------------------------ */
/* Local paths and images                                                   */
/* ------------------------------------------------------------------------ */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int has_scheme(const char *u)
{
    const char *c = u;
    while ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '+' ||
           *c == '-' || *c == '.')
        c++;
    return *c == ':' && c - u > 1;
}

/* Resolves a relative or file: URL against the document folder. */
static int resolve_local(const char *url, WCHAR *out, size_t cap)
{
    const char *u = url;
    if (_strnicmp(u, "file:///", 8) == 0) u += 8;
    else if (_strnicmp(u, "file://", 7) == 0) u += 7;
    else if (has_scheme(u)) return 0;

    char tmp[DOMDLY_PATH_MAX * 3];
    size_t n = 0;
    for (; *u && *u != '?' && *u != '#' && n + 1 < sizeof tmp; u++) {
        if (*u == '%' && hexval(u[1]) >= 0 && hexval(u[2]) >= 0) {
            tmp[n++] = (char)(hexval(u[1]) * 16 + hexval(u[2]));
            u += 2;
        } else {
            tmp[n++] = *u;
        }
    }
    tmp[n] = 0;
    if (!n) return 0;
    WCHAR *w = util_utf8_to_wide(tmp, -1);
    if (!w) return 0;
    for (WCHAR *p = w; *p; p++)
        if (*p == '/') *p = '\\';

    WCHAR combined[DOMDLY_PATH_MAX * 2];
    int absolute = (w[0] && w[1] == ':') || (w[0] == '\\' && w[1] == '\\');
    if (absolute) {
        swprintf(combined, DOMDLY_PATH_MAX * 2, L"%s", w);
    } else {
        WCHAR base[DOMDLY_PATH_MAX];
        if (g_has_path) util_path_dir(g_path, base, DOMDLY_PATH_MAX);
        else GetCurrentDirectoryW(DOMDLY_PATH_MAX, base);
        const WCHAR *rel = w;
        while (*rel == '\\') rel++;
        swprintf(combined, DOMDLY_PATH_MAX * 2, L"%s\\%s", base, rel);
    }
    free(w);
    DWORD r = GetFullPathNameW(combined, (DWORD)cap, out, NULL);
    return r > 0 && r < cap;
}

/* Image cache. Entries used by the render in progress are pinned (never
 * evicted) because the pending picture list points into them. */
#define IMAGE_CACHE 64
#define IMAGE_MAX_BYTES (16u * 1024 * 1024)

typedef struct image_entry {
    WCHAR path[DOMDLY_PATH_MAX];
    FILETIME mtime;
    unsigned char *data;
    size_t len;
    DWORD used;
    DWORD render;
} image_entry;

static image_entry g_images[IMAGE_CACHE];
static DWORD g_image_clock, g_render_gen;

static const image_entry *load_image(const char *url)
{
    WCHAR path[DOMDLY_PATH_MAX];
    if (!resolve_local(url, path, DOMDLY_PATH_MAX)) return NULL;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return NULL;
    if (fa.nFileSizeHigh || fa.nFileSizeLow > IMAGE_MAX_BYTES) return NULL;
    g_image_clock++;
    int victim = -1;
    for (int i = 0; i < IMAGE_CACHE; i++) {
        image_entry *e = &g_images[i];
        if (e->data && wcscmp(e->path, path) == 0 && CompareFileTime(&e->mtime, &fa.ftLastWriteTime) == 0) {
            e->used = g_image_clock;
            e->render = g_render_gen;
            return e;
        }
        if (!e->data) {
            if (victim < 0 || g_images[victim].data) victim = i;
        } else if (e->render != g_render_gen &&
                   (victim < 0 || (g_images[victim].data && e->used < g_images[victim].used))) {
            victim = i;
        }
    }
    if (victim < 0) return NULL;
    char *buf;
    size_t n;
    if (!util_read_file(path, &buf, &n)) return NULL;
    image_entry *e = &g_images[victim];
    free(e->data);
    wcscpy(e->path, path);
    e->mtime = fa.ftLastWriteTime;
    e->data = (unsigned char *)buf;
    e->len = n;
    e->used = g_image_clock;
    e->render = g_render_gen;
    return e;
}

typedef struct pending_image {
    const image_entry *img;
    long width_himetric, height_himetric;
    BSTR alt;
} pending_image;

static pending_image g_pending[IMAGE_CACHE];
static int g_npending;

static void clear_pending_images(void)
{
    for (int i = 0; i < g_npending; i++) SysFreeString(g_pending[i].alt);
    g_npending = 0;
}

static int image_placeholder_cb(void *user, const char *url, const char *alt, int max_width_twips)
{
    (void)user;
    if (g_npending >= IMAGE_CACHE) return 0;
    const image_entry *e = load_image(url);
    long w, h;
    if (!e || !md_image_size(e->data, e->len, &w, &h)) return 0;
    WCHAR *walt = util_utf8_to_wide(alt && *alt ? alt : "image", -1);
    BSTR balt = SysAllocString(walt ? walt : L"image"); /* InsertImage rejects a NULL alt text */
    free(walt);
    if (!balt) return 0;
    double tw = (double)w * 15.0, th = (double)h * 15.0; /* 96 DPI pixels -> twips */
    if (max_width_twips > 0 && tw > max_width_twips) {
        th = th * max_width_twips / tw;
        tw = max_width_twips;
    }
    pending_image *p = &g_pending[g_npending++];
    p->img = e;
    p->width_himetric = (long)(tw * 2540.0 / 1440.0);
    p->height_himetric = (long)(th * 2540.0 / 1440.0);
    p->alt = balt;
    return 1;
}

/* {C241F5E0-7206-11D8-A2C7-00A0D1D6C6B3} */
static const IID iid_text_document2 = {0xC241F5E0, 0x7206, 0x11D8, {0xA2, 0xC7, 0x00, 0xA0, 0xD1, 0xD6, 0xC6, 0xB3}};

/* Replaces the placeholders in the Read view with pictures. The RichEdit
 * control decodes PNG, JPEG, GIF and BMP itself. */
static void insert_pending_images(HWND h)
{
    if (!g_npending) return;
    IUnknown *ole = NULL;
    ITextDocument2 *doc = NULL;
    if (!SendMessageW(h, EM_GETOLEINTERFACE, 0, (LPARAM)&ole) || !ole) goto done;
    if (FAILED(ole->lpVtbl->QueryInterface(ole, &iid_text_document2, (void **)&doc)) || !doc) goto done;
    SendMessageW(h, EM_SETREADONLY, FALSE, 0);
    LONG from = 0;
    for (int i = 0; i < g_npending; i++) {
        static const WCHAR placeholder[2] = {MD_RTF_IMAGE_PLACEHOLDER, 0};
        FINDTEXTEXW ft;
        ft.chrg.cpMin = from;
        ft.chrg.cpMax = -1;
        ft.lpstrText = placeholder;
        LONG cp = (LONG)SendMessageW(h, EM_FINDTEXTEXW, FR_DOWN | FR_MATCHCASE, (LPARAM)&ft);
        if (cp < 0) break;
        from = cp + 1;
        ITextRange2 *range = NULL;
        if (FAILED(doc->lpVtbl->Range2(doc, cp, cp + 1, &range)) || !range) continue;
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, g_pending[i].img->len);
        IStream *stream = NULL;
        if (mem) {
            void *dst = GlobalLock(mem);
            memcpy(dst, g_pending[i].img->data, g_pending[i].img->len);
            GlobalUnlock(mem);
            if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
                GlobalFree(mem);
                stream = NULL;
            }
        }
        if (stream) {
            range->lpVtbl->SetText(range, NULL); /* remove the placeholder */
            range->lpVtbl->InsertImage(range, g_pending[i].width_himetric, g_pending[i].height_himetric, 0,
                                       TA_BOTTOM, g_pending[i].alt, stream);
            stream->lpVtbl->Release(stream);
        }
        range->lpVtbl->Release(range);
    }
    SendMessageW(h, EM_SETREADONLY, TRUE, 0);
done:
    if (doc) doc->lpVtbl->Release(doc);
    if (ole) ole->lpVtbl->Release(ole);
    clear_pending_images();
}

/* ------------------------------------------------------------------------ */
/* Read view rendering and block mapping                                    */
/* ------------------------------------------------------------------------ */

/* What each block marker stands for, so the Read view can be edited like a
 * document and the changes written back as Markdown. */
enum { BK_NONE, BK_PARA, BK_HEADING, BK_ITEM, BK_ROW, BK_CODE, BK_VERBATIM };
typedef struct blk {
    int kind, editable;
    int line, end;   /* source lines of the block's own text */
    int cline, cend; /* code blocks: the content lines (without fences) */
    int level, setext, task;
    char pfx[128];   /* first line prefix, the block's own syntax included */
    char cpfx[128];  /* container prefix only ("> ", list indentation) */
    char cont[128];  /* prefix of continuation lines */
} blk;
static blk *g_blk; /* parallel to g_mline / g_mcp */
static int g_node_set;
static const char *g_src; /* source text while rendering */
static size_t g_src_len;

static int grow_markers(void)
{
    if (g_mcount < g_mcap) return 1;
    int cap = g_mcap ? g_mcap * 2 : 256;
    int *nl = (int *)realloc(g_mline, (size_t)cap * sizeof *nl);
    if (!nl) return 0;
    g_mline = nl;
    LONG *nc = (LONG *)realloc(g_mcp, (size_t)cap * sizeof *nc);
    if (!nc) return 0;
    g_mcp = nc;
    blk *nb = (blk *)realloc(g_blk, (size_t)cap * sizeof *nb);
    if (!nb) return 0;
    g_blk = nb;
    g_mcap = cap;
    return 1;
}

static void marker_cb(void *user, int line)
{
    (void)user;
    if (!grow_markers()) return;
    if (!g_node_set) {
        memset(&g_blk[g_mcount], 0, sizeof g_blk[0]); /* the raw region */
        g_blk[g_mcount].line = g_blk[g_mcount].end = line;
    }
    g_node_set = 0;
    g_mline[g_mcount++] = line;
}

static const char *src_line(int line, size_t *len)
{
    size_t i = 0;
    for (int cur = 0; cur < line; cur++) {
        while (i < g_src_len && g_src[i] != '\n') i++;
        if (i >= g_src_len) {
            *len = 0;
            return NULL;
        }
        i++;
    }
    size_t e = i;
    while (e < g_src_len && g_src[e] != '\n') e++;
    *len = e - i;
    return g_src + i;
}

static size_t skip_blanks(const char *s, size_t n, size_t i)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* The list marker of an item's first line: "- ", "12. ", "- [x] " ... */
static size_t skip_item_marker(const char *s, size_t n, size_t i, int task)
{
    i = skip_blanks(s, n, i);
    if (i < n && s[i] >= '0' && s[i] <= '9') {
        while (i < n && s[i] >= '0' && s[i] <= '9') i++;
        if (i < n && (s[i] == '.' || s[i] == ')')) i++;
    } else if (i < n && (s[i] == '-' || s[i] == '+' || s[i] == '*')) {
        i++;
    }
    i = skip_blanks(s, n, i);
    if (task && i + 2 < n && s[i] == '[' && s[i + 2] == ']') i = skip_blanks(s, n, i + 3);
    return i;
}

/* Length of the container prefix of `line` for a block inside node's
 * ancestors (block quotes and list items). */
static size_t skip_containers(const char *s, size_t n, const md_node *node, int line)
{
    const md_node *chain[64];
    int depth = 0;
    for (const md_node *p = node->parent; p && p->type != MD_DOCUMENT && depth < 64; p = p->parent) chain[depth++] = p;
    size_t i = 0;
    while (depth-- > 0) {
        const md_node *a = chain[depth];
        if (a->type == MD_BLOCKQUOTE) {
            size_t j = i, k = 0;
            while (j < n && s[j] == ' ' && k < 3) j++, k++;
            if (j < n && s[j] == '>') {
                j++;
                if (j < n && s[j] == ' ') j++;
                i = j;
            }
        } else if (a->type == MD_ITEM) {
            i = a->line == line ? skip_item_marker(s, n, i, a->task) : skip_blanks(s, n, i);
        }
    }
    return i;
}

static int has_unsafe_inline(const md_node *n)
{
    for (const md_node *c = n->first; c; c = c->next)
        if (c->type == MD_IMAGE || c->type == MD_HTML_INLINE || c->from_html || has_unsafe_inline(c)) return 1;
    return 0;
}

static void set_prefix(char *dst, const char *s, size_t n)
{
    if (n > 127) n = 127;
    memcpy(dst, s, n);
    dst[n] = 0;
}

/* Fills in the metadata for the next marker from its node. */
static void block_node_cb(void *user, const md_node *n)
{
    (void)user;
    if (!grow_markers()) return;
    blk *b = &g_blk[g_mcount];
    memset(b, 0, sizeof *b);
    g_node_set = 1;
    b->line = n->line;
    b->end = n->end_line >= n->line ? n->end_line : n->line;
    b->editable = !g_welcome_shown;
    const md_node *inl = n; /* the node holding the inline content */
    switch (n->type) {
    case MD_PARAGRAPH: b->kind = BK_PARA; break;
    case MD_HEADING:
        b->kind = BK_HEADING;
        b->level = n->level;
        break;
    case MD_ITEM:
        b->kind = BK_ITEM;
        b->task = n->task;
        if (n->first && n->first->type == MD_PARAGRAPH && n->first->line == n->line) {
            inl = n->first;
            b->end = n->first->end_line;
        } else if (n->first) {
            b->editable = 0; /* an item that starts with a code block or a list */
        } else {
            b->end = n->line;
        }
        break;
    case MD_TABLE_ROW: b->kind = BK_ROW; break;
    case MD_CODE_BLOCK: b->kind = BK_CODE; break;
    case MD_HTML_BLOCK:
        b->kind = BK_VERBATIM;
        b->editable = 0; /* shown formatted: edited as Markdown source */
        break;
    default: b->editable = 0; break;
    }
    if (inl && has_unsafe_inline(inl)) b->editable = 0; /* pictures and HTML: edited as Markdown */

    size_t len;
    const char *s = src_line(b->line, &len);
    if (!s) {
        b->editable = 0;
        return;
    }
    size_t c = skip_containers(s, len, n, b->line), own = c;
    set_prefix(b->cpfx, s, c);
    switch (b->kind) {
    case BK_HEADING: {
        size_t j = skip_blanks(s, len, c);
        if (j < len && s[j] == '#') {
            while (j < len && s[j] == '#') j++;
            own = skip_blanks(s, len, j);
        } else {
            b->setext = 1;
            own = j;
        }
        break;
    }
    case BK_ITEM: own = skip_item_marker(s, len, c, b->task); break;
    case BK_PARA:
    case BK_ROW: own = skip_blanks(s, len, c); break;
    case BK_CODE: {
        size_t j = skip_blanks(s, len, c);
        if (j + 2 < len && ((s[j] == '`' && s[j + 1] == '`' && s[j + 2] == '`') || (s[j] == '~' && s[j + 1] == '~' && s[j + 2] == '~'))) {
            /* fenced: the content sits between the fence lines */
            b->cline = b->line + 1;
            b->cend = b->end;
            size_t el;
            const char *e = src_line(b->end, &el);
            size_t k = e ? skip_blanks(e, el, 0) : 0;
            if (b->end > b->line && e && k < el && (e[k] == '`' || e[k] == '~')) b->cend = b->end - 1;
            set_prefix(b->cont, s, j);
        } else {
            b->cline = b->line;
            b->cend = b->end;
            char tmp[128];
            snprintf(tmp, sizeof tmp, "%s    ", b->cpfx);
            set_prefix(b->cont, tmp, strlen(tmp));
        }
        break;
    }
    case BK_VERBATIM:
        b->cline = b->line;
        b->cend = b->end;
        set_prefix(b->cont, b->cpfx, strlen(b->cpfx));
        break;
    default: break;
    }
    if (own > 127) {
        b->editable = 0;
        return;
    }
    set_prefix(b->pfx, s, own);
    if (b->kind != BK_CODE && b->kind != BK_VERBATIM) {
        /* continuation lines keep the quote markers and indent the rest */
        size_t k;
        for (k = 0; b->pfx[k]; k++) b->cont[k] = b->pfx[k] == '>' ? '>' : ' ';
        b->cont[k] = 0;
    }
}

/* Finds where each hidden marker ended up in the rendered text. EM_FINDTEXT
 * reports real character positions; plain-text offsets do not match them
 * once tables are present. */
static void locate_markers(void)
{
    static const WCHAR mark[2] = {MD_RTF_BLOCK_MARKER, 0};
    LONG from = 0;
    int k = 0;
    while (k < g_mcount) {
        FINDTEXTEXW ft;
        ft.chrg.cpMin = from;
        ft.chrg.cpMax = -1;
        ft.lpstrText = mark;
        LONG cp = (LONG)SendMessageW(g_prev, EM_FINDTEXTEXW, FR_DOWN | FR_MATCHCASE, (LPARAM)&ft);
        if (cp < 0) break;
        g_mcp[k++] = cp;
        from = cp + 1;
    }
    g_mcount = k;
}

/* Index of the last marker at or before a source line / Read view position. */
static int marker_for_line(long line)
{
    int best = -1;
    for (int k = 0; k < g_mcount; k++)
        if (g_mline[k] <= line && (best < 0 || g_mline[k] >= g_mline[best])) best = k;
    return best;
}

static int marker_for_cp(LONG cp)
{
    int best = -1;
    for (int k = 0; k < g_mcount && g_mcp[k] <= cp; k++) best = k;
    return best;
}

static int preview_width_twips(void)
{
    RECT fr;
    SendMessageW(g_prev, EM_GETRECT, 0, (LPARAM)&fr);
    int px = fr.right - fr.left;
    g_last_render_width = px;
    if (px < 100) px = 100;
    long tw = (long)px * 1440 / g_dpi;
    tw = tw * 100 / (S.zoom > 0 ? S.zoom : 100);
    return (int)(tw - 60);
}

static int blk_range(int k, LONG *a, LONG *b);
static void wys_flush(void);
static void wys_place_caret(int line, long off);
static long vis_count(LONG a, LONG cp);
static int editor_line_count(void);
static int g_wys_dirty = -1; /* block whose text changed in the Read view */
static int g_pend = -1;      /* block followed by a new, still empty paragraph
                                (-2: the document is empty, typing starts it) */
static int g_welcome_off;    /* the user clicked into the welcome page */
static LONG g_pend_cp = -1;  /* where that paragraph is */
static int g_making_pend;    /* that paragraph is being inserted right now */
static int g_force_line = -1;
static long g_force_off;
static int g_eat_char;       /* swallow the WM_CHAR that follows a handled key */
static ITextDocument2 *g_pdoc;

/* ---- the region edited in place ---------------------------------------- */

static LONG find_char(HWND h, WCHAR c, LONG from)
{
    WCHAR s[2] = {c, 0};
    FINDTEXTEXW ft;
    ft.chrg.cpMin = from;
    ft.chrg.cpMax = -1;
    ft.lpstrText = s;
    return (LONG)SendMessageW(h, EM_FINDTEXTEXW, FR_DOWN | FR_MATCHCASE, (LPARAM)&ft);
}

static WCHAR char_at(HWND h, LONG cp)
{
    WCHAR *t = get_range(h, cp, cp + 1);
    WCHAR c = t ? t[0] : 0;
    free(t);
    return c;
}

/* Editable text of the region is [*rs, *re): it sits between the paragraph
 * holding MD_RTF_RAW_START and the one holding MD_RTF_RAW_END. Returns 0 when
 * no region is shown or its frame was damaged. */
static int raw_bounds(LONG *rs, LONG *re)
{
    if (g_raw_line < 0 || !g_raw_shown) return 0;
    LONG a = find_char(g_prev, MD_RTF_RAW_START, 0);
    if (a < 0) return 0;
    LONG b = find_char(g_prev, MD_RTF_RAW_END, a);
    if (b < a + 3 || char_at(g_prev, a + 1) != '\r' || char_at(g_prev, b - 1) != '\r') return 0;
    *rs = a + 2;
    *re = b - 1;
    return 1;
}

static LONG clamp_cp(LONG cp, LONG lo, LONG hi) { return cp < lo ? lo : cp > hi ? hi : cp; }

/* Editor position of the Read view caret, when it is inside the region. */
static int raw_caret_to_source(LONG *a, LONG *b)
{
    LONG rs, re;
    if (!raw_bounds(&rs, &re)) return 0;
    CHARRANGE cr = get_sel(g_prev);
    if (cr.cpMax < rs || cr.cpMin > re) return 0;
    LONG base = para_start_of_line(g_raw_line);
    *a = base + clamp_cp(cr.cpMin, rs, re) - rs;
    if (b) *b = base + clamp_cp(cr.cpMax, rs, re) - rs;
    return 1;
}

/* Puts the Read view caret on an editor position inside the region. */
static void raw_caret_from_source(LONG a, LONG b)
{
    LONG rs, re;
    if (!raw_bounds(&rs, &re)) return;
    LONG base = para_start_of_line(g_raw_line);
    set_sel(g_prev, clamp_cp(rs + a - base, rs, re), clamp_cp(rs + b - base, rs, re));
}

static void render_preview(void);

/* Copies the region text into the editor, replacing the source lines it
 * came from. With may_render, a damaged region is rebuilt from the editor. */
static void sync_raw(int may_render)
{
    if (!g_rendering) wys_flush();
    if (g_raw_line < 0 || g_rendering) return;
    LONG rs, re;
    if (!raw_bounds(&rs, &re)) {
        if (may_render) render_preview();
        return;
    }
    WCHAR *t = get_range(g_prev, rs, re);
    if (!t) return;
    int lines = 1;
    for (WCHAR *p = t; *p; p++) {
        if (*p == 0x0B) *p = '\r'; /* soft line break */
        if (*p == '\r') lines++;
    }
    if (g_raw_last && wcscmp(t, g_raw_last) == 0) {
        free(t);
        return;
    }
    LONG es = para_start_of_line(g_raw_line), ee = es;
    para_at(para_start_of_line(g_raw_line + g_raw_count - 1), NULL, &ee, NULL);
    if (ee < es) ee = es;
    g_syncing = 1;
    set_sel(g_edit, es, ee);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)t);
    g_syncing = 0;
    int delta = lines - g_raw_count;
    for (int k = 0; k < g_mcount; k++)
        if (g_mline[k] > g_raw_line) g_mline[k] += delta;
    g_raw_count = lines;
    free(g_raw_last);
    g_raw_last = t;
    update_title();
    SetTimer(g_main, TIMER_WORDS, 400, NULL);
}

static void raw_reset(void)
{
    g_raw_line = -1;
    g_raw_count = 0;
    g_raw_shown = 0;
    free(g_raw_last);
    g_raw_last = NULL;
    if (g_prev) SendMessageW(g_prev, EM_SETREADONLY, TRUE, 0);
}

/* Byte offset of the start of a 0-based line in '\n' separated text. */
static size_t line_offset(const char *t, size_t n, int line)
{
    size_t i = 0;
    for (int cur = 0; cur < line && i < n; i++)
        if (t[i] == '\n') cur++;
    return i;
}

static void render_preview(void)
{
    if (!g_prev) return;
    LONG keep_a = -1, keep_b = -1;
    if (g_raw_line >= 0) {
        sync_raw(0);
        if (!raw_caret_to_source(&keep_a, &keep_b)) keep_a = -1;
    }
    /* Read view edits go to the source first; the caret keeps its place in
     * its block (or goes where an edit asked for it). */
    int keep_line = g_force_line;
    long keep_off = g_force_off;
    g_force_line = -1;
    if (!g_rendering) wys_flush();
    if (keep_line < 0 && g_raw_line < 0 && is_read() && g_mcount > 0) {
        CHARRANGE cr = get_sel(g_prev);
        int k = marker_for_cp(cr.cpMin);
        LONG a, b;
        if (k >= 0 && g_blk[k].editable && blk_range(k, &a, &b) && cr.cpMin >= a && cr.cpMin <= b) {
            keep_line = g_blk[k].line;
            keep_off = vis_count(a, cr.cpMin);
        }
    }
    g_pend = -1;
    int len = 0;
    char *text = editor_utf8(&len);
    if (!text) return;
    g_words = count_words(text, len);
    const char *src = text;
    size_t srclen = (size_t)len;
    int only_space = 1;
    for (int i = 0; i < len; i++)
        if (text[i] != ' ' && text[i] != '\n' && text[i] != '\t') {
            only_space = 0;
            break;
        }
    g_welcome_shown = only_space && !g_has_path && g_raw_line < 0 && !g_welcome_off;
    if (g_welcome_shown) {
        src = welcome_md;
        srclen = sizeof welcome_md - 1;
    }

    md_node *doc = md_parse(src, srclen);
    md_expand_html(doc); /* HTML in the document is shown, safely */
    md_rtf_options opt;
    md_rtf_default_options(&opt, g_dark);
    if (g_night) night_colors(&opt.colors);
    opt.font = g_ui_family_a;
    opt.mono_font = g_mono_family_a;
    opt.width_twips = preview_width_twips();
    opt.image_placeholder = image_placeholder_cb;
    opt.block_marker = marker_cb;
    opt.block_node = block_node_cb;
    g_src = src;
    g_src_len = srclen;
    g_node_set = 0;
    if (g_raw_line >= 0) {
        int lines = md_line_count(text, (size_t)len);
        int f = g_raw_line < lines ? g_raw_line : lines - 1;
        int l = f + (g_raw_count > 0 ? g_raw_count : 1) - 1;
        if (l >= lines) l = lines - 1;
        int e = lines;
        md_block_span(doc, text, (size_t)len, &f, &l, &e);
        g_raw_line = f;
        g_raw_count = l - f + 1;
        size_t a = line_offset(text, (size_t)len, f), b = line_offset(text, (size_t)len, l + 1);
        if (l + 1 < lines && b > a) b--; /* the line break after line l */
        opt.raw_line = f;
        opt.raw_end = e;
        opt.raw_text = text + a;
        opt.raw_len = b - a;
    }
    g_render_gen++;
    g_mcount = 0;
    clear_pending_images();
    mdbuf rtf;
    mdbuf_init(&rtf);
    md_render_rtf(doc, &opt, &rtf);
    md_free(doc);
    g_src = NULL;
    g_src_len = 0;
    free(text);

    POINT pos;
    SendMessageW(g_prev, EM_GETSCROLLPOS, 0, (LPARAM)&pos);
    int frozen = g_hold_redraw ? 0 : freeze(g_prev);
    g_rendering = 1;
    stream_in(g_prev, rtf.data, rtf.len, SF_RTF);
    mdbuf_free(&rtf);
    insert_pending_images(g_prev);
    locate_markers();
    SendMessageW(g_prev, EM_SETZOOM, (WPARAM)S.zoom, 100);
    SendMessageW(g_prev, EM_SETREADONLY, FALSE, 0); /* edits are checked in preview_proc */
    SendMessageW(g_prev, EM_EMPTYUNDOBUFFER, 0, 0);
    if (keep_line >= 0 && g_raw_line < 0) wys_place_caret(keep_line, keep_off);
    free(g_raw_last);
    g_raw_last = NULL;
    g_raw_shown = g_raw_line >= 0;
    LONG rs, re;
    if (raw_bounds(&rs, &re)) {
        g_raw_last = get_range(g_prev, rs, re);
        if (keep_a >= 0) raw_caret_from_source(keep_a, keep_b);
    }
    g_rendering = 0;
    SendMessageW(g_prev, EM_SETSCROLLPOS, 0, (LPARAM)&pos);
    thaw(g_prev, frozen);
    g_preview_dirty = 0;
    update_status();
}

/* Opens the block at source line `line` for editing in place (line < 0
 * closes the region). The block keeps its height on screen; the caret goes
 * to the start (caret 0), the end (1) or the point pt (2) of the region. */
static void raw_open(long line, int caret, POINT pt)
{
    sync_raw(0);
    long anchor = line >= 0 ? line : g_raw_line;
    int k = marker_for_line(anchor);
    int y = -1;
    if (k >= 0) {
        POINTL p;
        SendMessageW(g_prev, EM_POSFROMCHAR, (WPARAM)&p, g_mcp[k]);
        y = p.y;
    }
    if (line >= 0 && g_welcome_shown) line = 0;
    free(g_raw_last);
    g_raw_last = NULL;
    g_raw_shown = 0; /* the region on screen belongs to the old block */
    g_raw_line = (int)line;
    g_raw_count = 1;
    if (line < 0) raw_reset();

    int frozen = freeze(g_prev);
    g_hold_redraw = 1;
    render_preview();
    g_hold_redraw = 0;
    k = marker_for_line(g_raw_line >= 0 ? g_raw_line : anchor);
    if (k >= 0 && y >= 0) {
        scroll_so_char_at(g_prev, g_mcp[k], y);
        /* a document that fits on screen is never left scrolled */
        GETTEXTLENGTHEX gl = {GTL_NUMCHARS, 1200};
        LONG n = (LONG)SendMessageW(g_prev, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
        POINTL endp;
        POINT sp;
        RECT fr;
        SendMessageW(g_prev, EM_POSFROMCHAR, (WPARAM)&endp, n > 0 ? n - 1 : 0);
        SendMessageW(g_prev, EM_GETSCROLLPOS, 0, (LPARAM)&sp);
        SendMessageW(g_prev, EM_GETRECT, 0, (LPARAM)&fr);
        if (endp.y + sp.y < fr.bottom - S_(40)) {
            POINT zero = {0, 0};
            SendMessageW(g_prev, EM_SETSCROLLPOS, 0, (LPARAM)&zero);
        }
    }
    LONG rs, re;
    if (raw_bounds(&rs, &re)) {
        LONG c = caret == 1 ? re : rs;
        if (caret == 2) {
            POINTL p = {pt.x, pt.y};
            c = clamp_cp((LONG)SendMessageW(g_prev, EM_CHARFROMPOS, 0, (LPARAM)&p), rs, re);
        }
        set_sel(g_prev, c, c);
    }
    thaw(g_prev, frozen);
    SetFocus(g_prev);
    update_status();
}

/* Narrows the region to the block holding the caret, so that a block the
 * user has just finished (for example by pressing Enter twice) is shown
 * formatted right away. The caret keeps its place on screen. */
static void raw_refocus(void)
{
    sync_raw(0);
    LONG a, b;
    if (!raw_caret_to_source(&a, &b)) return;
    long line;
    para_at(a, NULL, NULL, &line);
    int len = 0;
    char *text = editor_utf8(&len);
    if (!text) return;
    md_node *doc = md_parse(text, (size_t)len);
    int f = (int)line, l = (int)line, e = 0;
    md_block_span(doc, text, (size_t)len, &f, &l, &e);
    md_free(doc);
    free(text);
    if (f == g_raw_line && l - f + 1 == g_raw_count) return;

    int frozen = freeze(g_prev);
    g_raw_shown = 0;
    g_raw_line = f;
    g_raw_count = l - f + 1;
    g_hold_redraw = 1;
    render_preview();
    g_hold_redraw = 0;
    raw_caret_from_source(a, b);
    SendMessageW(g_prev, EM_SCROLLCARET, 0, 0);
    thaw(g_prev, frozen);
}

/* After the caret moved with the keyboard: when it left the region, open
 * the block it moved into. */
/* Closes the raw region and moves on to the block on `line`: an editable
 * block simply gets the caret, one with pictures or HTML opens raw. */
static void raw_leave(long line, int at_end)
{
    POINT none = {0, 0};
    raw_open(-1, 0, none);
    int k = -1;
    for (int j = 0; j < g_mcount; j++)
        if (g_mline[j] == line) {
            k = j;
            break;
        }
    LONG a, b;
    if (k >= 0 && g_blk[k].editable && blk_range(k, &a, &b)) set_sel(g_prev, at_end ? b : a, at_end ? b : a);
    else if (k >= 0) raw_open(line, at_end, none);
}

/* The same for a click at pt outside the raw region. */
static void raw_leave_at(POINT pt)
{
    POINT none = {0, 0};
    raw_open(-1, 0, none);
    POINTL p = {pt.x, pt.y};
    LONG cp = (LONG)SendMessageW(g_prev, EM_CHARFROMPOS, 0, (LPARAM)&p);
    int k = marker_for_cp(cp);
    if (k >= 0 && !g_blk[k].editable && g_blk[k].kind != BK_NONE) raw_open(g_mline[k], 2, pt);
    else set_sel(g_prev, cp, cp);
}

static void raw_follow_caret(WPARAM key, LONG before)
{
    LONG rs, re;
    if (!raw_bounds(&rs, &re)) return;
    CHARRANGE cr = get_sel(g_prev);
    if (cr.cpMin != cr.cpMax) return;
    LONG cp = cr.cpMin;
    int dir;
    if (cp >= rs && cp <= re) {
        /* RichEdit cannot step onto the hidden frame paragraphs, so at the
         * region's edge the caret just stays put. */
        if (cp != before) {
            raw_refocus();
            return;
        }
        if (key == VK_UP || key == VK_PRIOR || (key == VK_LEFT && cp == rs)) dir = -1;
        else if (key == VK_DOWN || key == VK_NEXT || (key == VK_RIGHT && cp == re)) dir = 1;
        else return;
    } else {
        dir = cp > re ? 1 : -1;
    }
    int k = cp < rs || cp > re ? marker_for_cp(cp) : -1;
    if (dir > 0) {
        if (k < 0 || g_mcp[k] <= re) {
            k = -1;
            for (int i = 0; i < g_mcount; i++)
                if (g_mcp[i] > re) {
                    k = i;
                    break;
                }
        }
        if (k < 0) set_sel(g_prev, re, re);
        else raw_leave(g_mline[k], 0);
    } else {
        if (k < 0) k = marker_for_cp(rs); /* the region's own marker */
        if (k >= 0 && g_mcp[k] >= rs - 3) k--;
        if (k < 0) set_sel(g_prev, rs, rs);
        else raw_leave(g_mline[k], 1);
    }
}

/* Whether a key or paste may change the text: only inside the region. */
static int raw_edit_allowed(int key)
{
    LONG rs, re;
    if (!raw_bounds(&rs, &re)) return 0;
    CHARRANGE cr = get_sel(g_prev);
    if (cr.cpMin < rs || cr.cpMax > re) return 0;
    if (cr.cpMin == cr.cpMax) {
        if (key == VK_BACK && cr.cpMin == rs) return 0;
        if (key == VK_DELETE && cr.cpMin == re) return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Switching between Edit and Read. The block under the caret (or at the    */
/* top of the view) stays at the same height on screen.                     */
/* ------------------------------------------------------------------------ */

/* ------------------------------------------------------------------------ */
/* Editing the Read view like a document. Typing changes the formatted text */
/* directly (bold stays bold, a table stays a table); the changed block is  */
/* then read back run by run through the Text Object Model, written out as  */
/* Markdown (core/md_write.c) and put into the editor, which stays the one  */
/* true copy of the document. Structural edits (Enter, joining blocks,      */
/* pasting) are made in the source and the view is rendered again.          */
/* ------------------------------------------------------------------------ */

#define RUN_PARA 0x100 /* host-only run flags: paragraph and table cell ends */
#define RUN_CELL 0x200


static ITextDocument2 *prev_doc(void)
{
    if (!g_pdoc) {
        IUnknown *ole = NULL;
        if (SendMessageW(g_prev, EM_GETOLEINTERFACE, 0, (LPARAM)&ole) && ole) {
            ole->lpVtbl->QueryInterface(ole, &iid_text_document2, (void **)&g_pdoc);
            ole->lpVtbl->Release(ole);
        }
    }
    return g_pdoc;
}

/* Text of [a, b) exactly as RichEdit stores it (table cell and row marks
 * included; EM_GETTEXTRANGE turns cell marks into tabs). */
static WCHAR *prev_text(LONG a, LONG b)
{
    ITextDocument2 *d = prev_doc();
    ITextRange2 *r = NULL;
    BSTR t = NULL;
    WCHAR *out = NULL;
    if (d && b >= a && SUCCEEDED(d->lpVtbl->Range2(d, a, b, &r)) && r) {
        r->lpVtbl->GetText(r, &t);
        r->lpVtbl->Release(r);
    }
    size_t n = t ? SysStringLen(t) : 0;
    out = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
    if (out) {
        if (n) memcpy(out, t, n * sizeof(WCHAR));
        out[n] = 0;
    }
    if (t) SysFreeString(t);
    return out;
}

static LONG prev_len(void)
{
    GETTEXTLENGTHEX gl = {GTL_NUMCHARS | GTL_PRECISE, 1200};
    return (LONG)SendMessageW(g_prev, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
}

static int is_textual(int k)
{
    return k >= 0 && k < g_mcount && g_blk[k].editable &&
           (g_blk[k].kind == BK_PARA || g_blk[k].kind == BK_HEADING || g_blk[k].kind == BK_ITEM);
}

/* [*a, *b) is the editable text of block k in the Read view. */
static int blk_range(int k, LONG *a, LONG *b)
{
    if (k < 0 || k >= g_mcount) return 0;
    LONG start = g_mcp[k] + 1;
    LONG limit = k + 1 < g_mcount ? g_mcp[k + 1] : prev_len();
    if (limit < start) limit = start;
    WCHAR *t = prev_text(start, limit);
    if (!t) return 0;
    LONG n = (LONG)wcslen(t), i = 0;
    int kind = g_blk[k].kind;
    if (kind == BK_ITEM) { /* skip the bullet, number or check box and its tab */
        LONG j = 0;
        while (j < n && t[j] != '\t' && t[j] != '\r') j++;
        if (j < n && t[j] == '\t') i = j + 1;
    }
    LONG e = i;
    if (kind == BK_ROW) {
        while (e < n && t[e] != 0xFFFB) e++;
    } else if (kind == BK_CODE) {
        while (e < n && t[e] != 0x07) e++;
    } else {
        while (e < n && t[e] != '\r' && t[e] != 0x07 && t[e] != 0xFFFB && t[e] != 0xFFF9) e++;
    }
    free(t);
    *a = start + i;
    *b = start + e;
    return 1;
}

/* ---- reading styled runs ------------------------------------------------- */

typedef struct runvec {
    md_run *v;
    size_t n, cap;
} runvec;

static void rv_free(runvec *r)
{
    for (size_t i = 0; i < r->n; i++) {
        free((char *)r->v[i].text);
        free((char *)r->v[i].url);
    }
    free(r->v);
    memset(r, 0, sizeof *r);
}

static void rv_push(runvec *r, const WCHAR *w, int wl, unsigned flags, const char *url)
{
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 32;
        md_run *nv = (md_run *)realloc(r->v, cap * sizeof *nv);
        if (!nv) return;
        r->v = nv;
        r->cap = cap;
    }
    int len = 0;
    char *u = wl > 0 ? util_wide_to_utf8(w, wl, &len) : _strdup("");
    if (!u) return;
    md_run *x = &r->v[r->n++];
    x->text = u;
    x->len = (size_t)len;
    x->flags = flags;
    x->url = url ? _strdup(url) : NULL;
}

/* {C241F5E3-7206-11D8-A2C7-00A0D1D6C6B3} */
static const IID iid_text_font2 = {0xC241F5E3, 0x7206, 0x11D8, {0xA2, 0xC7, 0x00, 0xA0, 0xD1, 0xD6, 0xC6, 0xB3}};

/* Appends the runs of [a, b) to rv. Hidden text (block markers, link
 * addresses) is skipped; `ignore` drops style flags that belong to the
 * block itself (headings and table headers are bold anyway). */
static void collect_runs(LONG a, LONG b, unsigned ignore, runvec *rv)
{
    ITextDocument2 *d = prev_doc();
    if (!d) return;
    LONG pos = a;
    while (pos < b) {
        ITextRange2 *r = NULL;
        if (FAILED(d->lpVtbl->Range2(d, pos, pos, &r)) || !r) break;
        long moved = 0, e = pos;
        r->lpVtbl->MoveEnd(r, tomCharFormat, 1, &moved);
        r->lpVtbl->GetEnd(r, &e);
        if (e <= pos) {
            r->lpVtbl->Release(r);
            break;
        }
        if (e > b) {
            e = b;
            r->lpVtbl->SetEnd(r, b);
        }
        ITextFont *f = NULL;
        long bold = 0, ital = 0, strike = 0, hidden = 0, link = 0;
        BSTR name = NULL;
        if (SUCCEEDED(r->lpVtbl->GetFont(r, &f)) && f) {
            f->lpVtbl->GetBold(f, &bold);
            f->lpVtbl->GetItalic(f, &ital);
            f->lpVtbl->GetStrikeThrough(f, &strike);
            f->lpVtbl->GetHidden(f, &hidden);
            f->lpVtbl->GetName(f, &name);
            ITextFont2 *f2 = NULL;
            if (SUCCEEDED(f->lpVtbl->QueryInterface(f, &iid_text_font2, (void **)&f2)) && f2) {
                f2->lpVtbl->GetLinkType(f2, &link);
                f2->lpVtbl->Release(f2);
            }
            f->lpVtbl->Release(f);
        }
        if (hidden != tomTrue && link != 3) { /* 3: the hidden address part of a link */
            unsigned flags = 0;
            if (bold == tomTrue) flags |= MD_RUN_BOLD;
            if (ital == tomTrue) flags |= MD_RUN_ITALIC;
            if (strike == tomTrue) flags |= MD_RUN_STRIKE;
            if (name && _wcsicmp(name, g_mono_family) == 0) flags |= MD_RUN_CODE;
            char *url = NULL;
            if (link == 2) { /* text of a friendly link: its address is the URL */
                BSTR u = NULL;
                if (SUCCEEDED(r->lpVtbl->GetURL(r, &u)) && u) {
                    WCHAR *p = u;
                    while (*p == '"' || *p == ' ') p++;
                    size_t n = wcslen(p);
                    while (n && (p[n - 1] == '"' || p[n - 1] == ' ')) n--;
                    if (n) url = util_wide_to_utf8(p, (int)n, NULL);
                    SysFreeString(u);
                }
                if (url) flags |= MD_RUN_LINK;
            }
            flags &= ~ignore;
            BSTR t = NULL;
            r->lpVtbl->GetText(r, &t);
            int n = t ? (int)SysStringLen(t) : 0, s = 0;
            if (t && n > 11 && wcsncmp(t, L"HYPERLINK \"", 11) == 0) {
                /* a field instruction that came along with the link text */
                const WCHAR *q = wcschr(t + 11, L'"');
                if (q) {
                    s = (int)(q - t) + 1;
                    while (s < n && t[s] == ' ') s++;
                }
            }
            for (int i = s; i <= n; i++) {
                WCHAR c = i < n ? t[i] : 0;
                int special = i == n || c == 0x0B || c == '\r' || c == 0x07 || c == 0xFFF9 || c == 0xFFFB ||
                              c == 0xFFFC || (c >= 0xE000 && c <= 0xE003);
                if (!special) continue;
                if (i > s) rv_push(rv, t + s, i - s, flags, url);
                if (i < n) {
                    if (c == 0x0B) rv_push(rv, NULL, 0, MD_RUN_BREAK, NULL);
                    else if (c == '\r') rv_push(rv, NULL, 0, RUN_PARA, NULL);
                    else if (c == 0x07) rv_push(rv, NULL, 0, RUN_CELL, NULL);
                }
                s = i + 1;
            }
            if (t) SysFreeString(t);
            free(url);
        }
        if (name) SysFreeString(name);
        r->lpVtbl->Release(r);
        pos = e;
    }
    /* inline code is drawn with a no-break space on each side */
    for (size_t i = 0; i < rv->n; i++) {
        md_run *x = &rv->v[i];
        if (!(x->flags & MD_RUN_CODE)) continue;
        int first = i == 0 || !(rv->v[i - 1].flags & MD_RUN_CODE);
        int last = i + 1 == rv->n || !(rv->v[i + 1].flags & MD_RUN_CODE);
        char *s = (char *)x->text;
        if (first && x->len >= 2 && (unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xA0) {
            memmove(s, s + 2, x->len - 1);
            x->len -= 2;
        }
        if (last && x->len >= 2 && (unsigned char)s[x->len - 2] == 0xC2 && (unsigned char)s[x->len - 1] == 0xA0) {
            x->len -= 2;
            s[x->len] = 0;
        }
    }
}

/* Number of visible characters in [a, cp), and the way back. */
static long vis_count(LONG a, LONG cp)
{
    ITextDocument2 *d = prev_doc();
    long total = 0;
    LONG pos = a;
    while (d && pos < cp) {
        ITextRange2 *r = NULL;
        if (FAILED(d->lpVtbl->Range2(d, pos, pos, &r)) || !r) break;
        long moved = 0, e = pos, hidden = 0;
        r->lpVtbl->MoveEnd(r, tomCharFormat, 1, &moved);
        r->lpVtbl->GetEnd(r, &e);
        ITextFont *f = NULL;
        if (SUCCEEDED(r->lpVtbl->GetFont(r, &f)) && f) {
            f->lpVtbl->GetHidden(f, &hidden);
            f->lpVtbl->Release(f);
        }
        r->lpVtbl->Release(r);
        if (e <= pos) break;
        if (e > cp) e = cp;
        if (hidden != tomTrue) total += e - pos;
        pos = e;
    }
    return total;
}

static LONG cp_at_vis(LONG a, LONG b, long off)
{
    ITextDocument2 *d = prev_doc();
    LONG pos = a;
    while (d && pos < b) {
        ITextRange2 *r = NULL;
        if (FAILED(d->lpVtbl->Range2(d, pos, pos, &r)) || !r) break;
        long moved = 0, e = pos, hidden = 0;
        r->lpVtbl->MoveEnd(r, tomCharFormat, 1, &moved);
        r->lpVtbl->GetEnd(r, &e);
        ITextFont *f = NULL;
        if (SUCCEEDED(r->lpVtbl->GetFont(r, &f)) && f) {
            f->lpVtbl->GetHidden(f, &hidden);
            f->lpVtbl->Release(f);
        }
        r->lpVtbl->Release(r);
        if (e <= pos) break;
        if (e > b) e = b;
        if (hidden != tomTrue) {
            if (off <= e - pos) return pos + off;
            off -= e - pos;
        }
        pos = e;
    }
    return b;
}

/* ---- writing Markdown ---------------------------------------------------- */

static unsigned blk_ignore(int k)
{
    const blk *B = &g_blk[k];
    return B->kind == BK_HEADING || B->kind == BK_ROW ? MD_RUN_BOLD : 0;
}

/* The first line's prefix: setext headings become ATX headings. */
static void blk_lead(int k, mdbuf *o)
{
    const blk *B = &g_blk[k];
    if (B->kind == BK_HEADING && B->setext) {
        mdbuf_puts(o, B->cpfx);
        for (int i = 0; i < B->level; i++) mdbuf_putc(o, '#');
        mdbuf_putc(o, ' ');
    } else {
        mdbuf_puts(o, B->pfx);
    }
}

static void put_rstrip(mdbuf *o, const char *s)
{
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') n--;
    mdbuf_put(o, s, n);
}

/* The prefix for a new list item after block k ("[x]" becomes "[ ]"). */
static void next_item_prefix(int k, mdbuf *o)
{
    const char *p = g_blk[k].pfx;
    for (size_t i = 0; p[i]; i++) {
        if (p[i] == '[' && (p[i + 1] == 'x' || p[i + 1] == 'X') && p[i + 2] == ']') {
            mdbuf_puts(o, "[ ]");
            i += 2;
        } else {
            mdbuf_putc(o, p[i]);
        }
    }
}

/* Inline Markdown of [a, b) in block k. */
static void blk_inline(int k, LONG a, LONG b, int close, mdbuf *o)
{
    runvec rv = {0};
    collect_runs(a, b, blk_ignore(k), &rv);
    md_write_inline(rv.v, rv.n, 0, g_blk[k].cont, close, o);
    rv_free(&rv);
}

/* The complete source lines of block k as shown now ('\n' separated). */
static void serialize_block(int k, LONG a, LONG b, mdbuf *o)
{
    const blk *B = &g_blk[k];
    runvec rv = {0};
    collect_runs(a, b, blk_ignore(k), &rv);
    if (B->kind == BK_CODE || B->kind == BK_VERBATIM) {
        mdbuf_puts(o, B->cont);
        for (size_t i = 0; i < rv.n; i++) {
            if (rv.v[i].flags & (MD_RUN_BREAK | RUN_PARA)) {
                mdbuf_putc(o, '\n');
                mdbuf_puts(o, B->cont);
            } else {
                mdbuf_put(o, rv.v[i].text, rv.v[i].len);
            }
        }
    } else if (B->kind == BK_ROW) {
        mdbuf_puts(o, B->pfx);
        mdbuf_putc(o, '|');
        size_t s = 0;
        for (size_t i = 0; i < rv.n; i++) {
            if (!(rv.v[i].flags & RUN_CELL)) continue;
            mdbuf_putc(o, ' ');
            md_write_inline(rv.v + s, i - s, 1, NULL, 1, o);
            mdbuf_puts(o, " |");
            s = i + 1;
        }
    } else {
        blk_lead(k, o);
        size_t s = 0;
        int part = 0;
        for (size_t i = 0; i <= rv.n; i++) {
            if (i < rv.n && !(rv.v[i].flags & RUN_PARA)) continue;
            if (part > 0 && i > s) {
                mdbuf_putc(o, '\n');
                if (B->kind == BK_ITEM) {
                    next_item_prefix(k, o);
                } else {
                    put_rstrip(o, B->cpfx);
                    mdbuf_putc(o, '\n');
                    mdbuf_puts(o, B->cpfx);
                }
            }
            if (part == 0 || i > s) md_write_inline(rv.v + s, i - s, 0, B->cont, 1, o);
            part++;
            s = i + 1;
        }
    }
    rv_free(&rv);
}

/* ---- changing the source ------------------------------------------------- */

static int count_lines(const char *s)
{
    int n = 1;
    for (; *s; s++)
        if (*s == '\n') n++;
    return n;
}

/* Replaces source lines l1..l2 (l2 < l1: inserts before line l1) with
 * '\n' separated text and moves the blocks that follow. */
static void splice_source(int l1, int l2, const char *utf8)
{
    WCHAR *w = util_utf8_to_wide(utf8, -1);
    if (!w) return;
    for (WCHAR *p = w; *p; p++)
        if (*p == '\n') *p = '\r';
    int lines = editor_line_count();
    int insert = l2 < l1;
    LONG es, ee;
    WCHAR *text = w;
    WCHAR *tmp = NULL;
    if (l1 >= lines) { /* past the end: append as new lines */
        es = ee = GetWindowTextLengthW(g_edit);
        tmp = (WCHAR *)malloc((wcslen(w) + 2) * sizeof(WCHAR));
        if (tmp) {
            tmp[0] = '\r';
            wcscpy(tmp + 1, w);
            text = tmp;
        }
    } else if (insert) {
        es = ee = para_start_of_line(l1);
        tmp = (WCHAR *)malloc((wcslen(w) + 2) * sizeof(WCHAR));
        if (tmp) {
            wcscpy(tmp, w);
            wcscat(tmp, L"\r");
            text = tmp;
        }
    } else {
        es = para_start_of_line(l1);
        para_at(para_start_of_line(l2), NULL, &ee, NULL);
        if (ee < es) ee = es;
    }
    g_syncing = 1;
    set_sel(g_edit, es, ee);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)text);
    g_syncing = 0;
    free(tmp);
    free(w);
    int delta = count_lines(utf8) - (insert ? 0 : l2 - l1 + 1);
    int after = insert ? l1 - 1 : l2;
    for (int k = 0; k < g_mcount; k++) {
        blk *B = &g_blk[k];
        if (B->line > after) {
            B->line += delta;
            B->end += delta;
            B->cline += delta;
            B->cend += delta;
            g_mline[k] += delta;
        }
    }
    update_title();
    SetTimer(g_main, TIMER_WORDS, 400, NULL);
}

/* Writes the changed block back into the source. */
static void wys_flush(void)
{
    int k = g_wys_dirty;
    g_wys_dirty = -1;
    KillTimer(g_main, TIMER_SYNC);
    LONG a, b;
    if (k < 0 || k >= g_mcount || !blk_range(k, &a, &b)) return;
    blk *B = &g_blk[k];
    mdbuf md;
    mdbuf_init(&md);
    serialize_block(k, a, b, &md);
    const char *text = md.data ? md.data : "";
    if (B->kind == BK_CODE || B->kind == BK_VERBATIM) {
        int closed = B->end > B->cend;
        splice_source(B->cline, B->cend, text);
        B->cend = B->cline + count_lines(text) - 1;
        B->end = B->cend + (closed ? 1 : 0);
    } else {
        splice_source(B->line, B->end, text);
        B->end = B->line + count_lines(text) - 1;
    }
    mdbuf_free(&md);
}

/* Renders again and puts the caret at a visible offset of the block that
 * starts on source line `line`. */
static void wys_render_at(int line, long off)
{
    g_force_line = line;
    g_force_off = off;
    int frozen = freeze(g_prev);
    g_hold_redraw = 1;
    render_preview();
    g_hold_redraw = 0;
    SendMessageW(g_prev, EM_SCROLLCARET, 0, 0);
    thaw(g_prev, frozen);
    SetFocus(g_prev);
}

/* After a render: the caret back on (line, off). */
static void wys_place_caret(int line, long off)
{
    int k = -1;
    for (int j = 0; j < g_mcount; j++)
        if (g_mline[j] == line && (k < 0 || g_blk[j].editable)) {
            k = j;
            if (g_blk[j].editable) break;
        }
    if (k < 0) k = marker_for_line(line);
    LONG a, b;
    if (k < 0 || !blk_range(k, &a, &b)) return;
    LONG cp = cp_at_vis(a, b, off);
    set_sel(g_prev, cp, cp);
}

/* Which block a change at the selection would touch: the block of the
 * start (or -1), and in *k2 the block of the end. */
static int edit_target(LONG *sa, LONG *sb, int *k2)
{
    CHARRANGE cr = get_sel(g_prev);
    int k1 = marker_for_cp(cr.cpMin), kb = marker_for_cp(cr.cpMax);
    if (cr.cpMin == cr.cpMax && k1 >= 0 && cr.cpMin == g_mcp[k1]) {
        /* in front of the hidden marker: step behind it */
        cr.cpMin = cr.cpMax = g_mcp[k1] + 1;
        set_sel(g_prev, cr.cpMin, cr.cpMax);
    }
    LONG a, b;
    if (cr.cpMin != cr.cpMax) {
        /* A selection made with the mouse or Shift+Ctrl+Home/End can start on a
           hidden marker (or a bullet) and end in the closing paragraph of the
           document; it is narrowed to the text of the blocks it covers. */
        CHARRANGE was = cr;
        if (k1 >= 0 && blk_range(k1, &a, &b) && cr.cpMin < a) cr.cpMin = a;
        if (g_mcount > 0 && blk_range(g_mcount - 1, &a, &b) && cr.cpMax > b) {
            cr.cpMax = b;
            kb = g_mcount - 1;
        }
        if (cr.cpMax < cr.cpMin) cr.cpMax = cr.cpMin;
        if (cr.cpMin != was.cpMin || cr.cpMax != was.cpMax) set_sel(g_prev, cr.cpMin, cr.cpMax);
    }
    if (k1 < 0 || !g_blk[k1].editable || !blk_range(k1, &a, &b) || cr.cpMin < a || cr.cpMin > b) k1 = -1;
    if (kb < 0 || !g_blk[kb].editable || !blk_range(kb, &a, &b) || cr.cpMax < a || cr.cpMax > b) kb = -1;
    *sa = cr.cpMin;
    *sb = cr.cpMax;
    *k2 = kb;
    return k1;
}

/* New text typed right behind hidden text would be hidden too. */
static void fix_insert_format(LONG cp)
{
    ITextDocument2 *d = prev_doc();
    if (!d || cp <= 0) return;
    ITextRange2 *p = NULL, *nx = NULL;
    ITextFont *pf = NULL, *nf = NULL, *dup = NULL;
    ITextSelection *sel = NULL;
    long hidden = 0;
    if (SUCCEEDED(d->lpVtbl->Range2(d, cp - 1, cp, &p)) && p && SUCCEEDED(p->lpVtbl->GetFont(p, &pf)) && pf)
        pf->lpVtbl->GetHidden(pf, &hidden);
    if (hidden == tomTrue && SUCCEEDED(d->lpVtbl->Range2(d, cp, cp + 1, &nx)) && nx &&
        SUCCEEDED(nx->lpVtbl->GetFont(nx, &nf)) && nf && SUCCEEDED(nf->lpVtbl->GetDuplicate(nf, &dup)) && dup) {
        dup->lpVtbl->SetHidden(dup, tomFalse);
        if (SUCCEEDED(d->lpVtbl->GetSelection(d, &sel)) && sel) {
            sel->lpVtbl->SetFont(sel, dup);
            sel->lpVtbl->Release(sel);
        }
    }
    if (dup) dup->lpVtbl->Release(dup);
    if (nf) nf->lpVtbl->Release(nf);
    if (nx) nx->lpVtbl->Release(nx);
    if (pf) pf->lpVtbl->Release(pf);
    if (p) p->lpVtbl->Release(p);
}

/* Lets RichEdit make a change inside block k (or between blocks when
 * k < 0) and keeps the marker positions in step. */
static LRESULT native_edit(HWND h, UINT m, WPARAM w, LPARAM l, int k)
{
    if (g_wys_dirty >= 0 && g_wys_dirty != k) wys_flush();
    LONG before = prev_len();
    CHARRANGE cr = get_sel(h);
    if (cr.cpMin == cr.cpMax) fix_insert_format(cr.cpMin);
    LRESULT r = DefSubclassProc(h, m, w, l);
    LONG delta = prev_len() - before;
    if (delta)
        for (int j = 0; j < g_mcount; j++)
            if (g_mcp[j] > cr.cpMin) g_mcp[j] += delta;
    if (g_pend_cp > cr.cpMin) g_pend_cp += delta;
    if (k >= 0) {
        g_wys_dirty = k;
        SetTimer(g_main, TIMER_SYNC, 500, NULL);
    }
    return r;
}

static void native_replace(const WCHAR *s, int k)
{
    native_edit(g_prev, EM_REPLACESEL, TRUE, (LPARAM)s, k);
}

/* ---- structural edits (made in the source, then rendered again) ---------- */

static int lines_blank_between(int from, int to)
{
    int len = 0;
    char *t = editor_utf8(&len);
    if (!t) return 0;
    int ok = 1, line = 0;
    size_t i = 0;
    while (i < (size_t)len && line < to) {
        if (line >= from && t[i] != '\n' && t[i] != ' ' && t[i] != '\t') ok = 0;
        if (t[i] == '\n') line++;
        i++;
    }
    free(t);
    return ok;
}

/* Enter in the middle of a paragraph or heading: two blocks. */
static void op_split(int k, LONG a, LONG c, LONG b)
{
    const blk *B = &g_blk[k];
    mdbuf o;
    mdbuf_init(&o);
    blk_lead(k, &o);
    blk_inline(k, a, c, 1, &o);
    if (B->kind == BK_ITEM) {
        mdbuf_putc(&o, '\n');
        next_item_prefix(k, &o);
    } else {
        mdbuf_putc(&o, '\n');
        put_rstrip(&o, B->cpfx);
        mdbuf_putc(&o, '\n');
        mdbuf_puts(&o, B->cpfx);
    }
    blk_inline(k, c, b, 1, &o);
    int line = B->line, next = B->kind == BK_ITEM ? line + 1 : line + 2;
    splice_source(B->line, B->end, o.data ? o.data : "");
    mdbuf_free(&o);
    wys_render_at(next, 0);
}

/* Joins block k into block j (j before k): Backspace at the start of k or
 * Delete at the end of j. */
static void op_merge(int j, int k)
{
    if (!is_textual(j) || !is_textual(k) || !lines_blank_between(g_blk[j].end + 1, g_blk[k].line)) return;
    LONG ja, jb, ka, kb;
    if (!blk_range(j, &ja, &jb) || !blk_range(k, &ka, &kb)) return;
    long caret = vis_count(ja, jb);
    runvec rv = {0};
    collect_runs(ja, jb, blk_ignore(j), &rv);
    collect_runs(ka, kb, blk_ignore(k), &rv);
    mdbuf o;
    mdbuf_init(&o);
    blk_lead(j, &o);
    md_write_inline(rv.v, rv.n, 0, g_blk[j].cont, 1, &o);
    rv_free(&rv);
    int line = g_blk[j].line;
    splice_source(g_blk[j].line, g_blk[k].end, o.data ? o.data : "");
    mdbuf_free(&o);
    wys_render_at(line, caret);
}

/* Backspace at the start of a block. */
static void op_backspace_start(int k)
{
    blk *B = &g_blk[k];
    LONG a, b;
    if (!blk_range(k, &a, &b)) return;
    if (B->kind == BK_HEADING || B->kind == BK_ITEM) {
        /* a heading becomes a paragraph, a list item leaves its list */
        mdbuf o;
        mdbuf_init(&o);
        int line = B->line;
        if (B->kind == BK_ITEM) {
            put_rstrip(&o, B->cpfx);
            mdbuf_putc(&o, '\n');
            line++;
        }
        mdbuf_puts(&o, B->cpfx);
        blk_inline(k, a, b, 1, &o);
        splice_source(B->line, B->end, o.data ? o.data : "");
        mdbuf_free(&o);
        wys_render_at(line, 0);
        return;
    }
    if (B->kind == BK_PARA && k > 0) op_merge(k - 1, k);
}

/* Enter on a table row: a new empty row below. */
static void op_row_enter(int k)
{
    LONG a, b;
    if (!blk_range(k, &a, &b)) return;
    WCHAR *t = prev_text(a, b);
    int cols = 0;
    for (WCHAR *p = t; p && *p; p++)
        if (*p == 0x07) cols++;
    free(t);
    if (cols < 1) cols = 1;
    mdbuf o;
    mdbuf_init(&o);
    mdbuf_puts(&o, g_blk[k].pfx);
    mdbuf_putc(&o, '|');
    for (int i = 0; i < cols; i++) mdbuf_puts(&o, "   |");
    int line = g_blk[k].line;
    splice_source(line + 1, line, o.data);
    mdbuf_free(&o);
    wys_render_at(line + 1, 0);
}

/* Replaces the selection, which may cover several blocks, with Markdown
 * (pasted text) or with plain typed text. */
static int op_replace(int ka, LONG sa, int kb, LONG sb, const char *md, int is_markdown)
{
    if (!is_textual(ka) || !is_textual(kb)) return 0;
    LONG aa, ab, ba, bb;
    if (!blk_range(ka, &aa, &ab) || !blk_range(kb, &ba, &bb)) return 0;
    runvec left = {0}, right = {0};
    collect_runs(aa, sa, blk_ignore(ka), &left);
    collect_runs(sb, bb, blk_ignore(kb), &right);
    long caret = vis_count(aa, sa);
    mdbuf o;
    mdbuf_init(&o);
    blk_lead(ka, &o);
    if (is_markdown) {
        md_write_inline(left.v, left.n, 0, g_blk[ka].cont, 1, &o);
        mdbuf_puts(&o, md);
        md_write_inline(right.v, right.n, 0, g_blk[ka].cont, 1, &o);
        WCHAR *w = util_utf8_to_wide(md, -1);
        caret += w ? (long)wcslen(w) : 0;
        free(w);
    } else {
        runvec all = {0};
        for (size_t i = 0; i < left.n; i++) {
            WCHAR *w = util_utf8_to_wide(left.v[i].text, (int)left.v[i].len);
            rv_push(&all, w, w ? (int)wcslen(w) : 0, left.v[i].flags, left.v[i].url);
            free(w);
        }
        unsigned style = left.n ? left.v[left.n - 1].flags & ~(MD_RUN_BREAK | RUN_PARA | RUN_CELL) : 0;
        WCHAR *ins = util_utf8_to_wide(md, -1);
        if (ins && *ins) rv_push(&all, ins, (int)wcslen(ins), style, left.n ? left.v[left.n - 1].url : NULL);
        caret += ins ? (long)wcslen(ins) : 0;
        free(ins);
        for (size_t i = 0; i < right.n; i++) {
            WCHAR *w = util_utf8_to_wide(right.v[i].text, (int)right.v[i].len);
            rv_push(&all, w, w ? (int)wcslen(w) : 0, right.v[i].flags, right.v[i].url);
            free(w);
        }
        md_write_inline(all.v, all.n, 0, g_blk[ka].cont, 1, &o);
        rv_free(&all);
    }
    rv_free(&left);
    rv_free(&right);
    int line = g_blk[ka].line;
    splice_source(g_blk[ka].line, g_blk[kb].end, o.data ? o.data : "");
    mdbuf_free(&o);
    wys_render_at(line, caret);
    return 1;
}

/* The body text style for a new paragraph (after a heading, say). */
static void set_body_format(void)
{
    md_rtf_options opt;
    md_rtf_default_options(&opt, g_dark);
    if (g_night) night_colors(&opt.colors);
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_SIZE | CFM_FACE | CFM_COLOR | CFM_HIDDEN | CFM_UNDERLINE | CFM_LINK;
    cf.yHeight = opt.font_half_points * 10;
    cf.crTextColor = RGB((opt.colors.text >> 16) & 255, (opt.colors.text >> 8) & 255, opt.colors.text & 255);
    wcsncpy(cf.szFaceName, g_ui_family, LF_FACESIZE - 1);
    SendMessageW(g_prev, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof pf);
    pf.cbSize = sizeof pf;
    pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_SPACEBEFORE | PFM_SPACEAFTER;
    pf.dySpaceAfter = 200;
    SendMessageW(g_prev, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
}

/* A character typed into the new empty paragraph makes it a real block. */
static void op_pending_type(const char *utf8_md, int is_markdown)
{
    int k = g_pend;
    g_pend = -1;
    if (k == -2) { /* the first text of an empty document */
        mdbuf o;
        mdbuf_init(&o);
        long caret = 0;
        if (is_markdown) {
            mdbuf_puts(&o, utf8_md);
        } else {
            WCHAR *w = util_utf8_to_wide(utf8_md, -1);
            runvec rv = {0};
            if (w) rv_push(&rv, w, (int)wcslen(w), 0, NULL);
            md_write_inline(rv.v, rv.n, 0, NULL, 1, &o);
            rv_free(&rv);
            caret = w ? (long)wcslen(w) : 0;
            free(w);
        }
        splice_source(0, editor_line_count() - 1, o.data ? o.data : "");
        mdbuf_free(&o);
        wys_render_at(0, caret);
        return;
    }
    if (k < 0 || k >= g_mcount) return;
    const blk *B = &g_blk[k];
    mdbuf o;
    mdbuf_init(&o);
    put_rstrip(&o, B->cpfx);
    mdbuf_putc(&o, '\n');
    mdbuf_puts(&o, B->cpfx);
    long caret = 0;
    if (is_markdown) {
        mdbuf_puts(&o, utf8_md);
    } else {
        WCHAR *w = util_utf8_to_wide(utf8_md, -1);
        runvec rv = {0};
        if (w) rv_push(&rv, w, (int)wcslen(w), 0, NULL);
        md_write_inline(rv.v, rv.n, 0, B->cont, 1, &o);
        rv_free(&rv);
        caret = w ? (long)wcslen(w) : 0;
        free(w);
    }
    int line = B->end + 2;
    splice_source(B->end + 1, B->end, o.data ? o.data : "");
    mdbuf_free(&o);
    wys_render_at(line, caret);
}

/* Is the whole document selected (Ctrl+A, or a selection from the first block
 * to the last)? */
static int all_selected(void)
{
    if (g_mcount == 0 || g_raw_line >= 0) return 0;
    CHARRANGE cr = get_sel(g_prev);
    LONG fa, fb, la, lb;
    if (cr.cpMin == cr.cpMax || !blk_range(0, &fa, &fb) || !blk_range(g_mcount - 1, &la, &lb)) return 0;
    return cr.cpMin <= fa && cr.cpMax >= lb;
}

/* Replaces the whole document: with nothing (Delete, Backspace, Cut), or with
 * what was typed or pasted. Whatever kind the blocks were, the result is an
 * empty document in which typing starts, as in a word processor. One undo
 * step in the source brings everything back. */
static void op_replace_all(const char *md, int is_markdown)
{
    wys_flush();
    splice_source(0, editor_line_count() - 1, "");
    g_welcome_off = 1;
    wys_render_at(0, 0);
    CHARRANGE z = get_sel(g_prev);
    g_pend = -2;
    g_pend_cp = z.cpMin;
    if (md && *md) op_pending_type(md, is_markdown);
}

static void wys_enter(void)
{
    if (all_selected()) {
        op_replace_all("", 0);
        return;
    }
    LONG sa, sb;
    int k2, k = edit_target(&sa, &sb, &k2);
    if (g_pend != -1 && sa == sb && sa == g_pend_cp) return; /* one empty paragraph is enough */
    if (k < 0) return;
    if (sa != sb) {
        if (k2 == k && (g_blk[k].kind == BK_CODE || g_blk[k].kind == BK_VERBATIM)) {
            native_replace(L"\r", k);
            return;
        }
        if (!op_replace(k, sa, k2 < 0 ? k : k2, sb, "", 0)) return;
        wys_enter();
        return;
    }
    LONG a, b;
    if (!blk_range(k, &a, &b)) return;
    switch (g_blk[k].kind) {
    case BK_CODE:
    case BK_VERBATIM: native_replace(L"\r", k); break;
    case BK_ROW: wys_flush(); op_row_enter(k); break;
    case BK_ITEM:
        wys_flush();
        blk_range(k, &a, &b);
        if (a == b) { /* Enter on an empty item ends the list: a normal paragraph follows */
            int line = g_blk[k].line, prev = k > 0 ? g_blk[k - 1].line : -1;
            splice_source(g_blk[k].line, g_blk[k].end, "");
            wys_render_at(prev >= 0 ? prev : line, 1L << 30);
            CHARRANGE cr = get_sel(g_prev);
            int pk = marker_for_cp(cr.cpMin);
            LONG pa, pb;
            if (prev >= 0 && pk >= 0 && blk_range(pk, &pa, &pb) && is_textual(pk)) {
                set_sel(g_prev, pb, pb);
                g_making_pend = 1;
                native_replace(L"\r", -1);
                set_body_format();
                g_making_pend = 0;
                g_pend = pk;
                g_pend_cp = pb + 1;
            }
        } else {
            op_split(k, a, sa, b);
        }
        break;
    default:
        wys_flush();
        blk_range(k, &a, &b);
        if (sa >= b) { /* at the end: a new paragraph that exists once something is typed */
            set_sel(g_prev, b, b);
            g_making_pend = 1; /* behind the last block the caret must not be pulled back */
            native_replace(L"\r", -1);
            set_body_format();
            g_making_pend = 0;
            g_pend = k;
            g_pend_cp = b + 1;
        } else {
            op_split(k, a, sa, b);
        }
        break;
    }
}

/* Backspace / Delete. Returns 1 when handled here (or refused). */
static int wys_delete(WPARAM key)
{
    if (all_selected()) {
        op_replace_all("", 0);
        return 1;
    }
    LONG sa, sb;
    int k2, k = edit_target(&sa, &sb, &k2);
    if (g_pend != -1 && sa == sb && sa == g_pend_cp) {
        if (g_pend == -2) return 1; /* empty document: nothing to delete */
        if (key == VK_BACK) { /* drop the empty paragraph again */
            set_sel(g_prev, sa - 1, sa);
            native_replace(L"", -1);
        }
        g_pend = -1;
        return 1;
    }
    if (k < 0) return 1;
    int kind = g_blk[k].kind;
    if (sa != sb) {
        if (k2 == k) {
            if (kind == BK_ROW) { /* never delete the cell structure */
                WCHAR *t = prev_text(sa, sb);
                int cells = t && wcschr(t, 0x07);
                free(t);
                return cells;
            }
            return 0;
        }
        if (k2 >= 0) op_replace(k, sa, k2, sb, "", 0);
        return 1;
    }
    LONG a, b;
    if (!blk_range(k, &a, &b)) return 1;
    if (kind == BK_ROW) {
        WCHAR *t = prev_text(key == VK_BACK ? sa - 1 : sa, key == VK_BACK ? sa : sa + 1);
        int cell = t && t[0] == 0x07;
        free(t);
        if (cell || (key == VK_BACK && sa == a) || (key == VK_DELETE && sa >= b)) return 1;
        return 0;
    }
    if (key == VK_BACK && sa == a) {
        wys_flush();
        if (kind == BK_CODE || kind == BK_VERBATIM) return 1;
        op_backspace_start(k);
        return 1;
    }
    if (key == VK_DELETE && sa >= b) {
        wys_flush();
        if (is_textual(k) && k + 1 < g_mcount && g_blk[k + 1].kind == BK_PARA) op_merge(k, k + 1);
        return 1;
    }
    return 0;
}

static char *clipboard_utf8(void)
{
    if (!OpenClipboard(g_main)) return NULL;
    char *out = NULL;
    HANDLE hd = GetClipboardData(CF_UNICODETEXT);
    const WCHAR *w = hd ? (const WCHAR *)GlobalLock(hd) : NULL;
    if (w) {
        size_t n = wcslen(w);
        WCHAR *c = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
        if (c) {
            size_t k = 0;
            for (size_t i = 0; i < n; i++)
                if (w[i] != '\r') c[k++] = w[i];
            c[k] = 0;
            out = util_wide_to_utf8(c, -1, NULL);
            free(c);
        }
        GlobalUnlock(hd);
    }
    CloseClipboard();
    return out;
}

/* Pasted text is read as Markdown, so pasting "**x**" gives bold x. */
static void wys_paste(void)
{
    char *md = clipboard_utf8();
    if (!md) return;
    if (all_selected()) {
        op_replace_all(md, 1);
        free(md);
        return;
    }
    LONG sa, sb;
    int k2, k = edit_target(&sa, &sb, &k2);
    if (g_pend != -1 && sa == sb && sa == g_pend_cp) {
        op_pending_type(md, 1);
    } else if (k >= 0 && (g_blk[k].kind == BK_CODE || g_blk[k].kind == BK_VERBATIM) && k2 == k) {
        WCHAR *w = util_utf8_to_wide(md, -1);
        if (w) {
            for (WCHAR *p = w; *p; p++)
                if (*p == '\n') *p = '\r';
            native_replace(w, k);
            free(w);
        }
    } else if (k >= 0 && g_blk[k].kind == BK_ROW && k2 == k) {
        if (!strchr(md, '\n') && !strchr(md, '|')) {
            WCHAR *w = util_utf8_to_wide(md, -1);
            if (w) native_replace(w, k);
            free(w);
        }
    } else if (k >= 0) {
        wys_flush();
        k = edit_target(&sa, &sb, &k2);
        if (k >= 0) op_replace(k, sa, k2 < 0 ? k : k2, sb, md, 1);
    }
    free(md);
}

/* A printable character. Returns 1 when it was dealt with. */
static void wys_char(HWND h, UINT m, WPARAM w, LPARAM l)
{
    LONG sa, sb;
    int k2, k = edit_target(&sa, &sb, &k2);
    char u[8];
    WCHAR wc[2] = {(WCHAR)w, 0};
    WideCharToMultiByte(CP_UTF8, 0, wc, 1, u, sizeof u - 1, NULL, NULL);
    u[w < 0x80 ? 1 : w < 0x800 ? 2 : 3] = 0;
    if (all_selected()) {
        op_replace_all(u, 0);
        return;
    }
    if (g_pend != -1 && sa == sb && sa == g_pend_cp) {
        op_pending_type(u, 0);
        return;
    }
    if (k < 0) return;
    if (k2 != k || (sa != sb && k2 < 0)) {
        wys_flush();
        k = edit_target(&sa, &sb, &k2);
        if (k >= 0 && k2 >= 0) op_replace(k, sa, k2, sb, u, 0);
        return;
    }
    if (g_blk[k].kind == BK_ROW && sa != sb) {
        WCHAR *t = prev_text(sa, sb);
        int cells = t && wcschr(t, 0x07);
        free(t);
        if (cells) return;
    }
    native_edit(h, m, w, l, k);
}

/* Tab in a table moves to the next cell (Shift+Tab: the previous one). */
static void wys_tab(int back)
{
    CHARRANGE cr = get_sel(g_prev);
    int k = marker_for_cp(cr.cpMin);
    LONG a, b;
    if (k < 0 || g_blk[k].kind != BK_ROW || !blk_range(k, &a, &b)) {
        if (k >= 0 && (g_blk[k].kind == BK_CODE || g_blk[k].kind == BK_VERBATIM)) native_replace(L"\t", k);
        return;
    }
    WCHAR *t = prev_text(a, b);
    if (!t) return;
    LONG pos = cr.cpMin - a, n = (LONG)wcslen(t), target = -1;
    if (!back) {
        for (LONG i = pos; i < n; i++)
            if (t[i] == 0x07) {
                target = a + i + 1;
                break;
            }
        if (target >= b) {
            if (k + 1 < g_mcount && g_blk[k + 1].kind == BK_ROW) {
                target = g_mcp[k + 1] + 1;
            } else { /* Tab in the last cell adds a row, as in a word processor */
                free(t);
                wys_flush();
                op_row_enter(k);
                return;
            }
        }
    } else {
        LONG i = pos - 1;
        while (i >= 0 && t[i] != 0x07) i--; /* start of this cell */
        i--;
        while (i >= 0 && t[i] != 0x07) i--; /* start of the previous one */
        target = a + i + 1;
        if (pos == 0 && k > 0 && g_blk[k - 1].kind == BK_ROW) {
            LONG pa, pb;
            if (blk_range(k - 1, &pa, &pb)) target = pa;
        }
    }
    free(t);
    if (target >= 0) set_sel(g_prev, target, target);
}

/* The caret moved: write back a block it left. */
static void wys_selchange(void)
{
    if (g_rendering || g_raw_line >= 0) return;
    CHARRANGE cr = get_sel(g_prev);
    /* Below the last block there is only the thin closing paragraph of the
       rendered document; a click there (or Ctrl+End) would leave a tiny caret
       in it, so the caret goes to the end of the last block instead. */
    LONG la, lb;
    int in_pend = g_making_pend || (g_pend != -1 && cr.cpMin == g_pend_cp);    if (!in_pend && cr.cpMin == cr.cpMax && g_mcount > 0 && blk_range(g_mcount - 1, &la, &lb) && cr.cpMin > lb) {
        set_sel(g_prev, lb, lb);
        return;
    }
    if (g_pend >= 0 && cr.cpMin != g_pend_cp) g_pend = -1;
    if (g_pend == -2 && cr.cpMin != g_pend_cp) g_pend_cp = cr.cpMin; /* empty document: anywhere */
    if (g_wys_dirty >= 0) {
        LONG a, b;
        if (!blk_range(g_wys_dirty, &a, &b) || cr.cpMin < a || cr.cpMax > b) wys_flush();
    }
}

static void switch_to_read(void)
{
    if (is_read()) return;
    RECT fr;
    SendMessageW(g_edit, EM_GETRECT, 0, (LPARAM)&fr);
    CHARRANGE cr = get_sel(g_edit);
    POINTL pc;
    SendMessageW(g_edit, EM_POSFROMCHAR, (WPARAM)&pc, cr.cpMax);
    LONG anchor = cr.cpMax;
    int y = pc.y;
    if (pc.y < fr.top || pc.y >= fr.bottom) {
        POINTL tl = {fr.left + 1, fr.top + 1};
        anchor = (LONG)SendMessageW(g_edit, EM_CHARFROMPOS, 0, (LPARAM)&tl);
        SendMessageW(g_edit, EM_POSFROMCHAR, (WPARAM)&pc, anchor);
        y = pc.y;
    }
    long line;
    para_at(anchor, NULL, NULL, &line);

    S.view = VIEW_PREVIEW;
    layout();
    if (g_preview_dirty) render_preview();
    int k = marker_for_line(line);
    if (k >= 0) scroll_so_char_at(g_prev, g_mcp[k], y);
    else {
        POINT zero = {0, 0};
        SendMessageW(g_prev, EM_SETSCROLLPOS, 0, (LPARAM)&zero);
    }
    SetFocus(g_prev);
    update_status();
    settings_save(&S);
}

static void switch_to_edit(int at_point, int px, int py)
{
    if (!is_read()) return;
    LONG src_caret = -1;
    if (g_raw_line >= 0) {
        /* leaving an in-place edit: the editor opens at the same spot */
        sync_raw(0);
        CHARRANGE cr = get_sel(g_prev);
        POINTL cp;
        SendMessageW(g_prev, EM_POSFROMCHAR, (WPARAM)&cp, cr.cpMax);
        if (!at_point && raw_caret_to_source(&src_caret, NULL)) py = cp.y;
        raw_reset();
        g_preview_dirty = 1;
    }
    if (src_caret >= 0) {
        S.view = VIEW_EDITOR;
        layout();
        set_sel(g_edit, src_caret, src_caret);
        scroll_so_char_at(g_edit, src_caret, py);
        SetFocus(g_edit);
        update_status();
        settings_save(&S);
        return;
    }
    RECT fr;
    SendMessageW(g_prev, EM_GETRECT, 0, (LPARAM)&fr);
    POINTL p = {at_point ? px : fr.left + 1, at_point ? py : fr.top + 1};
    LONG cp = (LONG)SendMessageW(g_prev, EM_CHARFROMPOS, 0, (LPARAM)&p);
    int y = at_point ? py : fr.top;
    int k = marker_for_cp(cp);
    long line = k >= 0 ? g_mline[k] : 0;
    if (!at_point && k >= 0) {
        /* keep the block's on-screen position, not the click height */
        POINTL bp;
        SendMessageW(g_prev, EM_POSFROMCHAR, (WPARAM)&bp, g_mcp[k]);
        y = bp.y;
    }

    S.view = VIEW_EDITOR;
    layout();
    LONG lcp = para_start_of_line(line);
    if (at_point) set_sel(g_edit, lcp, lcp);
    scroll_so_char_at(g_edit, lcp, y);
    SetFocus(g_edit);
    update_status();
    settings_save(&S);
}

static void ensure_edit(void)
{
    if (g_raw_cmd) return; /* the command edits the region's source lines */
    if (is_read()) switch_to_edit(0, 0, 0);
}

static void schedule_render(void)
{
    SetTimer(g_main, TIMER_RENDER, RENDER_DELAY, NULL);
}

/* ------------------------------------------------------------------------ */
/* Files                                                                    */
/* ------------------------------------------------------------------------ */

static void editor_set_text(const WCHAR *text)
{
    raw_reset();
    int frozen = freeze(g_edit);
    stream_in(g_edit, text, wcslen(text) * sizeof(WCHAR), SF_TEXT | SF_UNICODE);
    apply_editor_format();
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    SendMessageW(g_edit, EM_EMPTYUNDOBUFFER, 0, 0);
    set_sel(g_edit, 0, 0);
    POINT zero = {0, 0};
    SendMessageW(g_edit, EM_SETSCROLLPOS, 0, (LPARAM)&zero);
    SendMessageW(g_prev, EM_SETSCROLLPOS, 0, (LPARAM)&zero);
    thaw(g_edit, frozen);
    g_preview_dirty = 1;
}

static int g_bulk_open; /* restoring tabs: render only at the end */

static void after_load(void)
{
    if (g_bulk_open) return;
    update_title();
    tree_sync(0);
    update_word_count();
    if (is_read()) {
        render_preview();
        POINT zero = {0, 0};
        SendMessageW(g_prev, EM_SETSCROLLPOS, 0, (LPARAM)&zero);
    }
    update_status();
}

static int open_file(const WCHAR *path)
{
    WCHAR full[DOMDLY_PATH_MAX];
    if (!GetFullPathNameW(path, DOMDLY_PATH_MAX, full, NULL)) wcsncpy(full, path, DOMDLY_PATH_MAX - 1);
    char *data;
    size_t len;
    if (!util_read_file(full, &data, &len)) {
        WCHAR msg[DOMDLY_PATH_MAX + 64];
        swprintf(msg, DOMDLY_PATH_MAX + 64, L"Could not open\n%s", full);
        MessageBoxW(g_main, msg, L"Domdly", MB_ICONERROR | MB_OK);
        settings_remove_recent(&S, full);
        return 0;
    }
    text_encoding enc;
    int crlf;
    WCHAR *text = util_decode_text(data, len, &enc, &crlf);
    free(data);
    if (!text) return 0;
    int open_in = tab_find(full);
    if (open_in >= 0) {
        /* already open: just show it */
        free(text);
        tab_activate(open_in);
        return 1;
    }
    if (!tab_is_blank() && !tab_new()) {
        free(text);
        return 0;
    }
    wcscpy(g_path, full);
    g_has_path = 1;
    g_enc = enc;
    g_crlf = crlf;
    editor_set_text(text);
    free(text);
    settings_add_recent(&S, full);
    settings_save(&S);
    after_load();
    return 1;
}

/* Replaces the active tab's document with new text. */
static void reset_document(const char *utf8)
{
    g_welcome_off = 0;
    WCHAR *text = util_utf8_to_wide(utf8 ? utf8 : "", -1);
    g_path[0] = 0;
    g_has_path = 0;
    g_enc = ENC_UTF8;
    g_crlf = 0;
    editor_set_text(text ? text : L"");
    free(text);
    after_load();
}

/* A new document, in a new tab unless the active one is still blank. */
static void new_document(const char *utf8)
{
    if (!tab_is_blank() && !tab_new()) return;
    reset_document(utf8);
}

static DWORD g_filter_index; /* 1-based filter the last save dialog ended on */

static int save_dialog(WCHAR *out, const WCHAR *filter, const WCHAR *ext, const WCHAR *initial)
{
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    wcsncpy(out, initial, DOMDLY_PATH_MAX - 1);
    out[DOMDLY_PATH_MAX - 1] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = DOMDLY_PATH_MAX;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    ofn.nFilterIndex = 1;
    int ok = GetSaveFileNameW(&ofn) != 0;
    g_filter_index = ofn.nFilterIndex;
    return ok;
}

static const WCHAR md_filter[] =
    L"Markdown (*.md;*.markdown)\0*.md;*.markdown;*.mdown;*.mkd\0Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
static const WCHAR save_as_filter[] =
    L"Markdown (*.md)\0*.md;*.markdown;*.mdown;*.mkd\0PDF document (*.pdf)\0*.pdf\0HTML page (*.html)\0*.html;*.htm\0"
    L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";

static int has_ext(const WCHAR *path, const WCHAR *ext)
{
    const WCHAR *dot = wcsrchr(path, L'.');
    const WCHAR *slash = wcsrchr(path, L'\\');
    return dot && (!slash || dot > slash) && _wcsicmp(dot + 1, ext) == 0;
}

/* The document's path with another extension, or Untitled.<ext>. */
static void path_with_ext(WCHAR *out, const WCHAR *ext)
{
    if (g_has_path) {
        wcscpy(out, g_path);
        WCHAR *dot = wcsrchr(out, L'.');
        WCHAR *slash = wcsrchr(out, L'\\');
        if (dot && (!slash || dot > slash)) *dot = 0;
    } else {
        wcscpy(out, L"Untitled");
    }
    wcscat(out, L".");
    wcscat(out, ext);
}

/* Prints the document, always in the light theme, to the "Microsoft Print
 * to PDF" printer that ships with Windows 10 and 11, writing straight to
 * path. A hidden RichEdit control lays the pages out (EM_FORMATRANGE). */
static int export_pdf(const WCHAR *path)
{
    HDC pdc = CreateDCW(L"WINSPOOL", L"Microsoft Print to PDF", NULL, NULL);
    if (!pdc) {
        MessageBoxW(g_main,
                    L"Domdly saves PDF files through the \"Microsoft Print to PDF\" printer, which is not installed.\n\n"
                    L"Add it in Settings > Bluetooth & devices > Printers & scanners, or in "
                    L"Windows Features (\"Microsoft Print to PDF\"), then try again.",
                    L"Domdly", MB_ICONWARNING | MB_OK);
        return 0;
    }
    int dpx = GetDeviceCaps(pdc, LOGPIXELSX), dpy = GetDeviceCaps(pdc, LOGPIXELSY);
    long page_w = MulDiv(GetDeviceCaps(pdc, PHYSICALWIDTH), 1440, dpx);
    long page_h = MulDiv(GetDeviceCaps(pdc, PHYSICALHEIGHT), 1440, dpy);
    long off_x = MulDiv(GetDeviceCaps(pdc, PHYSICALOFFSETX), 1440, dpx);
    long off_y = MulDiv(GetDeviceCaps(pdc, PHYSICALOFFSETY), 1440, dpy);
    long margin = 1080; /* 0.75 inch */

    int len = 0;
    char *text = editor_utf8(&len);
    if (!text) {
        DeleteDC(pdc);
        return 0;
    }
    md_node *doc = md_parse(text, (size_t)len);
    md_expand_html(doc);
    free(text);
    md_rtf_options opt;
    md_rtf_default_options(&opt, 0);
    opt.font = g_ui_family_a;
    opt.mono_font = g_mono_family_a;
    opt.width_twips = (int)(page_w - 2 * margin);
    opt.image_placeholder = image_placeholder_cb;
    clear_pending_images();
    mdbuf rtf;
    mdbuf_init(&rtf);
    md_render_rtf(doc, &opt, &rtf);
    md_free(doc);

    HWND re = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | ES_MULTILINE | ES_READONLY, 0, 0, 100, 100, g_main,
                              NULL, g_inst, NULL);
    int ok = 0;
    if (re) {
        olecb_attach(re);
        SendMessageW(re, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
        stream_in(re, rtf.data, rtf.len, SF_RTF);
        insert_pending_images(re);
        SendMessageW(re, EM_SETTARGETDEVICE, (WPARAM)pdc, opt.width_twips);

        WCHAR title[DOMDLY_PATH_MAX];
        wcscpy(title, g_has_path ? util_path_name(g_path) : L"Untitled");
        DOCINFOW di;
        memset(&di, 0, sizeof di);
        di.cbSize = sizeof di;
        di.lpszDocName = title;
        di.lpszOutput = path;
        if (StartDocW(pdc, &di) > 0) {
            FORMATRANGE fr;
            memset(&fr, 0, sizeof fr);
            fr.hdc = fr.hdcTarget = pdc;
            fr.rcPage.right = page_w;
            fr.rcPage.bottom = page_h;
            /* the printable area starts at the physical offset */
            fr.rc.left = margin - off_x;
            fr.rc.top = margin - off_y;
            fr.rc.right = page_w - margin - off_x;
            fr.rc.bottom = page_h - margin - off_y;
            GETTEXTLENGTHEX gl = {GTL_NUMCHARS, 1200};
            LONG total = (LONG)SendMessageW(re, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
            LONG cp = 0;
            ok = 1;
            int pages = 0;
            do {
                if (StartPage(pdc) <= 0) {
                    ok = 0;
                    break;
                }
                fr.chrg.cpMin = cp;
                fr.chrg.cpMax = -1;
                LONG next = (LONG)SendMessageW(re, EM_FORMATRANGE, TRUE, (LPARAM)&fr);
                EndPage(pdc);
                if (next <= cp) break; /* nothing more fits: stop rather than loop */
                cp = next;
            } while (cp < total && ++pages < 2000);
            SendMessageW(re, EM_FORMATRANGE, FALSE, 0);
            if (ok) EndDoc(pdc);
            else AbortDoc(pdc);
        }
        DestroyWindow(re);
    }
    mdbuf_free(&rtf);
    DeleteDC(pdc);
    if (!ok) {
        WCHAR msg[DOMDLY_PATH_MAX + 64];
        swprintf(msg, DOMDLY_PATH_MAX + 64, L"Could not save the PDF file\n%s", path);
        MessageBoxW(g_main, msg, L"Domdly", MB_ICONERROR | MB_OK);
    }
    return ok;
}

static void export_html_to(const WCHAR *path);

static int save_file(int save_as)
{
    sync_raw(0);
    WCHAR path[DOMDLY_PATH_MAX];
    if (save_as) {
        /* one dialog for every format: Markdown, PDF, HTML or text */
        WCHAR initial[DOMDLY_PATH_MAX];
        path_with_ext(initial, L"md");
        if (!save_dialog(path, save_as_filter, L"md", initial)) return 0;
        /* the chosen file type wins when the name has no matching extension */
        if (g_filter_index == 2 && !has_ext(path, L"pdf")) {
            WCHAR *dot = wcsrchr(path, L'.');
            if (dot && has_ext(path, L"md")) *dot = 0;
            if (wcslen(path) + 5 < DOMDLY_PATH_MAX) wcscat(path, L".pdf");
        } else if (g_filter_index == 3 && !has_ext(path, L"html") && !has_ext(path, L"htm")) {
            WCHAR *dot = wcsrchr(path, L'.');
            if (dot && has_ext(path, L"md")) *dot = 0;
            if (wcslen(path) + 6 < DOMDLY_PATH_MAX) wcscat(path, L".html");
        }
        if (has_ext(path, L"pdf")) return export_pdf(path);
        if (has_ext(path, L"html") || has_ext(path, L"htm")) {
            export_html_to(path);
            return 1;
        }
    } else if (!g_has_path) {
        if (!save_dialog(path, md_filter, L"md", L"Untitled.md")) return 0;
    } else {
        wcscpy(path, g_path);
    }
    int n = 0;
    WCHAR *w = control_text(g_edit, &n);
    if (!w) return 0;
    /* RichEdit uses CR internally; restore the document's line endings. */
    size_t cr = 0;
    for (int i = 0; i < n; i++)
        if (w[i] == '\r') cr++;
    WCHAR *out = (WCHAR *)malloc(((size_t)n + cr + 1) * sizeof(WCHAR));
    if (!out) {
        free(w);
        return 0;
    }
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        if (w[i] == '\r') {
            if (g_crlf) out[k++] = '\r';
            out[k++] = '\n';
        } else {
            out[k++] = w[i];
        }
    }
    out[k] = 0;
    free(w);
    int ulen = 0;
    char *u = util_wide_to_utf8(out, (int)k, &ulen);
    free(out);
    if (!u) return 0;
    char *data = u;
    size_t dlen = (size_t)ulen;
    char *withbom = NULL;
    if (g_enc == ENC_UTF8_BOM) {
        withbom = (char *)malloc(dlen + 3);
        if (withbom) {
            memcpy(withbom, "\xEF\xBB\xBF", 3);
            memcpy(withbom + 3, u, dlen);
            data = withbom;
            dlen += 3;
        }
    }
    int ok = util_write_file_atomic(path, data, dlen);
    free(withbom);
    free(u);
    if (!ok) {
        WCHAR msg[DOMDLY_PATH_MAX + 64];
        swprintf(msg, DOMDLY_PATH_MAX + 64, L"Could not save\n%s", path);
        MessageBoxW(g_main, msg, L"Domdly", MB_ICONERROR | MB_OK);
        return 0;
    }
    int path_changed = !g_has_path || wcscmp(path, g_path) != 0;
    wcscpy(g_path, path);
    g_has_path = 1;
    if (g_enc != ENC_UTF8_BOM) g_enc = ENC_UTF8;
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    settings_add_recent(&S, path);
    settings_save(&S);
    update_title();
    update_status();
    if (path_changed) {
        g_preview_dirty = 1; /* relative images now resolve */
        if (is_read()) render_preview();
    }
    tree_sync(1); /* a new file appears in the sidebar */
    return 1;
}

/* Returns 1 when it is fine to discard the current document. */
static int confirm_discard(void)
{
    sync_raw(0);
    if (!SendMessageW(g_edit, EM_GETMODIFY, 0, 0)) return 1;
    WCHAR msg[DOMDLY_PATH_MAX + 64];
    swprintf(msg, DOMDLY_PATH_MAX + 64, L"Do you want to save changes to %s?",
             g_has_path ? util_path_name(g_path) : L"Untitled");
    int r = MessageBoxW(g_main, msg, L"Domdly", MB_ICONWARNING | MB_YESNOCANCEL);
    if (r == IDYES) return save_file(0);
    return r == IDNO;
}

/* ------------------------------------------------------------------------ */
/* Tabs                                                                     */
/* ------------------------------------------------------------------------ */

static HWND create_richedit(HWND parent, DWORD style);
static void take_over_drop(HWND h);
static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref);

static HWND create_editor(ITextDocument **tom)
{
    HWND e = create_richedit(g_main, ES_NOHIDESEL | ES_WANTRETURN);
    *tom = NULL;
    if (!e) return NULL;
    SetWindowPos(e, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(e, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
    SendMessageW(e, EM_SETUNDOLIMIT, 1000, 0);
    SetWindowSubclass(e, edit_proc, 1, 0);
    take_over_drop(e);
    IUnknown *ole = NULL;
    if (SendMessageW(e, EM_GETOLEINTERFACE, 0, (LPARAM)&ole) && ole) {
        ole->lpVtbl->QueryInterface(ole, &iid_text_document, (void **)tom);
        ole->lpVtbl->Release(ole);
    }
    return e;
}

/* Colors, font, zoom and wrapping for the active editor (other tabs pick
 * them up when they are activated). */
static void style_editor(void)
{
    SetWindowTheme(g_edit, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    SendMessageW(g_edit, EM_SETBKGNDCOLOR, 0, (LPARAM)P.doc_bg);
    apply_editor_format();
    SendMessageW(g_edit, EM_SETZOOM, (WPARAM)S.zoom, 100);
    SendMessageW(g_edit, EM_SETTARGETDEVICE, 0, S.word_wrap ? 0 : 1);
    LONG_PTR style = GetWindowLongPtrW(g_edit, GWL_STYLE);
    style = S.word_wrap ? (style & ~WS_HSCROLL) : (style | WS_HSCROLL);
    SetWindowLongPtrW(g_edit, GWL_STYLE, style);
    SetWindowPos(g_edit, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

static void tab_store(void)
{
    if (g_cur < 0 || g_cur >= g_ntabs) return;
    doc_tab *t = &g_tabs[g_cur];
    t->edit = g_edit;
    t->tom = g_tom;
    wcscpy(t->path, g_path);
    t->has_path = g_has_path;
    t->crlf = g_crlf;
    t->enc = g_enc;
    if (is_read()) SendMessageW(g_prev, EM_GETSCROLLPOS, 0, (LPARAM)&t->read_scroll);
}

static void tab_activate(int i)
{
    if (i < 0 || i >= g_ntabs) return;
    if (i == g_cur && g_edit == g_tabs[i].edit) return;
    if (g_cur >= 0) wys_flush();
    if (g_raw_line >= 0) {
        sync_raw(0);
        raw_reset();
    }
    tab_store();
    if (g_edit) {
        ShowWindow(g_edit, SW_HIDE);
        SetWindowLongPtrW(g_edit, GWLP_ID, 0);
    }
    g_cur = i;
    doc_tab *t = &g_tabs[i];
    g_edit = t->edit;
    g_tom = t->tom;
    wcscpy(g_path, t->path);
    g_has_path = t->has_path;
    g_crlf = t->crlf;
    g_enc = t->enc;
    SetWindowLongPtrW(g_edit, GWLP_ID, 1); /* control id 1 = the active editor */
    style_editor();
    g_preview_dirty = 1;
    g_last_render_width = -1;
    layout();
    if (is_read() && !g_bulk_open) {
        render_preview();
        SendMessageW(g_prev, EM_SETSCROLLPOS, 0, (LPARAM)&t->read_scroll);
    }
    update_word_count();
    update_title();
    update_status();
    update_tooltips();
    invalidate_chrome();
    if (!g_bulk_open) tree_sync(0);
    SetFocus(is_read() ? g_prev : g_edit);
}

/* Opens an empty tab and makes it active. */
static int tab_new(void)
{
    if (g_ntabs >= MAX_TABS) {
        MessageBoxW(g_main, L"Domdly can keep 64 documents open at a time. Close a tab first.", L"Domdly",
                    MB_ICONINFORMATION | MB_OK);
        return 0;
    }
    ITextDocument *tom = NULL;
    HWND e = create_editor(&tom);
    if (!e) return 0;
    tab_store();
    int i = g_ntabs++;
    memset(&g_tabs[i], 0, sizeof g_tabs[i]);
    g_tabs[i].edit = e;
    g_tabs[i].tom = tom;
    g_tabs[i].enc = ENC_UTF8;
    tab_activate(i);
    return 1;
}

/* An untitled, empty, unchanged tab can be reused for the next document. */
static int tab_is_blank(void)
{
    return !g_has_path && GetWindowTextLengthW(g_edit) == 0 && !SendMessageW(g_edit, EM_GETMODIFY, 0, 0);
}

static int tab_find(const WCHAR *full)
{
    tab_store();
    for (int i = 0; i < g_ntabs; i++)
        if (g_tabs[i].has_path && _wcsicmp(g_tabs[i].path, full) == 0) return i;
    return -1;
}

/* Closed tabs, newest last, for Ctrl+Shift+T: saved documents by path,
 * never saved ones by their text. */
#define CLOSED_MAX 25
typedef struct closed_tab {
    WCHAR *path, *text;
} closed_tab;
static closed_tab g_closed[CLOSED_MAX];
static int g_nclosed;

static void remember_closed(void)
{
    if (!g_has_path && GetWindowTextLengthW(g_edit) == 0) return; /* nothing worth bringing back */
    if (g_nclosed == CLOSED_MAX) {
        free(g_closed[0].path);
        free(g_closed[0].text);
        memmove(g_closed, g_closed + 1, (CLOSED_MAX - 1) * sizeof g_closed[0]);
        g_nclosed--;
    }
    closed_tab *c = &g_closed[g_nclosed++];
    c->path = g_has_path ? _wcsdup(g_path) : NULL;
    c->text = g_has_path ? NULL : control_text(g_edit, NULL);
}

/* Reopens the most recently closed tab. A file that was moved or deleted
 * since is skipped (and said so in the status bar); the one before it is
 * tried instead. */
static void reopen_closed(void)
{
    WCHAR missing[DOMDLY_PATH_MAX] = L"";
    while (g_nclosed > 0) {
        closed_tab c = g_closed[--g_nclosed];
        int ok = 0;
        if (c.path) {
            if (GetFileAttributesW(c.path) != INVALID_FILE_ATTRIBUTES) ok = open_file(c.path);
            else wcsncpy(missing, util_path_name(c.path), DOMDLY_PATH_MAX - 1);
        } else if (c.text) {
            for (WCHAR *p = c.text; *p; p++)
                if (*p == '\r') *p = '\n';
            char *u = util_wide_to_utf8(c.text, -1, NULL);
            new_document(u ? u : "");
            free(u);
            SendMessageW(g_edit, EM_SETMODIFY, TRUE, 0); /* still unsaved */
            update_title();
            invalidate_chrome();
            ok = 1;
        }
        free(c.path);
        free(c.text);
        if (ok) break;
    }
    if (missing[0]) {
        swprintf(g_status, 256, L"%s was moved or deleted, so it could not be reopened.", missing);
        InvalidateRect(g_main, &g_rc_bottom, FALSE);
    }
}

static void tab_close(int i)
{
    if (i < 0 || i >= g_ntabs) return;
    tab_activate(i);
    /* Closing the one empty tab that is left closes the window, like a
       browser: Ctrl+W pressed once more after the last document is gone. */
    if (g_ntabs == 1 && !g_has_path && g_raw_line < 0 && GetWindowTextLengthW(g_edit) == 0 &&
        !SendMessageW(g_edit, EM_GETMODIFY, 0, 0)) {
        PostMessageW(g_main, WM_CLOSE, 0, 0);
        return;
    }
    if (!confirm_discard()) return;
    remember_closed();
    if (g_ntabs == 1) {
        reset_document(NULL); /* the window always keeps one tab */
        return;
    }
    if (g_raw_line >= 0) raw_reset();
    HWND dead = g_edit;
    ITextDocument *tom = g_tom;
    memmove(&g_tabs[i], &g_tabs[i + 1], (size_t)(g_ntabs - i - 1) * sizeof g_tabs[0]);
    g_ntabs--;
    g_cur = -1; /* nothing to store: the closed tab is gone */
    g_edit = NULL;
    tab_activate(i < g_ntabs ? i : g_ntabs - 1);
    if (tom) tom->lpVtbl->Release(tom);
    RevokeDragDrop(dead);
    DestroyWindow(dead);
}

/* Opens Explorer on the folder of the open file with the file selected. */
static void show_in_folder(void)
{
    if (!g_has_path) return;
    WCHAR args[DOMDLY_PATH_MAX + 16];
    swprintf(args, DOMDLY_PATH_MAX + 16, L"/select,\"%s\"", g_path);
    ShellExecuteW(g_main, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}

/* Asks about every unsaved tab; returns 0 when the user cancels. */
static int confirm_close_all(void)
{
    int start = g_cur;
    for (int i = 0; i < g_ntabs; i++) {
        if (!SendMessageW(g_tabs[i].edit, EM_GETMODIFY, 0, 0) && !(i == g_cur && g_raw_line >= 0)) continue;
        tab_activate(i);
        if (!confirm_discard()) return 0;
    }
    if (start < g_ntabs) tab_activate(start);
    return 1;
}

/* Remembers the open files so the next start brings them back. */
static void save_session(void)
{
    tab_store();
    S.session_count = 0;
    S.session_active = 0;
    for (int i = 0; i < g_ntabs && S.session_count < SESSION_MAX; i++) {
        if (!g_tabs[i].has_path) continue;
        if (i == g_cur) S.session_active = S.session_count;
        wcscpy(S.session[S.session_count++], g_tabs[i].path);
    }
}

static void cmd_open(void)
{
    /* several files may be picked; each opens in its own tab */
    static WCHAR path[32768];
    path[0] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = md_filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)(sizeof path / sizeof path[0]);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn)) return;
    const WCHAR *first = path + wcslen(path) + 1;
    if (!*first) {
        open_file(path); /* a single file: path is the full name */
        return;
    }
    for (const WCHAR *f = first; *f; f += wcslen(f) + 1) {
        WCHAR full[DOMDLY_PATH_MAX];
        if (swprintf(full, DOMDLY_PATH_MAX, L"%s\\%s", path, f) > 0) open_file(full);
    }
}

static void cmd_export_html(void)
{
    sync_raw(0);
    WCHAR initial[DOMDLY_PATH_MAX], path[DOMDLY_PATH_MAX];
    path_with_ext(initial, L"html");
    if (!save_dialog(path, L"HTML page (*.html)\0*.html;*.htm\0All files (*.*)\0*.*\0", L"html", initial)) return;
    export_html_to(path);
}

static void cmd_export_pdf(void)
{
    sync_raw(0);
    WCHAR initial[DOMDLY_PATH_MAX], path[DOMDLY_PATH_MAX];
    path_with_ext(initial, L"pdf");
    if (save_dialog(path, L"PDF document (*.pdf)\0*.pdf\0All files (*.*)\0*.*\0", L"pdf", initial)) export_pdf(path);
}

static void export_html_to(const WCHAR *path)
{
    int len = 0;
    char *text = editor_utf8(&len);
    if (!text) return;
    md_node *doc = md_parse(text, (size_t)len);
    free(text);
    WCHAR title_w[DOMDLY_PATH_MAX];
    wcscpy(title_w, g_has_path ? util_path_name(g_path) : L"Untitled");
    WCHAR *dot = wcsrchr(title_w, L'.');
    if (dot) *dot = 0;
    char *title = util_wide_to_utf8(title_w, -1, NULL);
    mdbuf html;
    mdbuf_init(&html);
    md_render_html_page(doc, title ? title : "Domdly", &html);
    md_free(doc);
    free(title);
    if (!util_write_file_atomic(path, html.data, html.len))
        MessageBoxW(g_main, L"Could not write the HTML file.", L"Domdly", MB_ICONERROR | MB_OK);
    mdbuf_free(&html);
}

static void cmd_copy_html(void)
{
    sync_raw(0);
    int len = 0;
    char *text = editor_utf8(&len);
    if (!text) return;
    md_node *doc = md_parse(text, (size_t)len);
    free(text);
    mdbuf html;
    mdbuf_init(&html);
    md_render_html(doc, &html);
    md_free(doc);
    WCHAR *w = util_utf8_to_wide(html.data ? html.data : "", (int)html.len);
    mdbuf_free(&html);
    if (!w) return;
    size_t n = wcslen(w), lf = 0;
    for (size_t i = 0; i < n; i++)
        if (w[i] == '\n') lf++;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (n + lf + 1) * sizeof(WCHAR));
    if (mem && OpenClipboard(g_main)) {
        WCHAR *dst = (WCHAR *)GlobalLock(mem);
        size_t k = 0;
        for (size_t i = 0; i < n; i++) {
            if (w[i] == '\n') dst[k++] = '\r';
            dst[k++] = w[i];
        }
        dst[k] = 0;
        GlobalUnlock(mem);
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        CloseClipboard();
    } else if (mem) {
        GlobalFree(mem);
    }
    free(w);
}

/* ------------------------------------------------------------------------ */
/* Links                                                                    */
/* ------------------------------------------------------------------------ */

static const md_node *find_heading(const md_node *n, const char *slug)
{
    for (const md_node *c = n->first; c; c = c->next) {
        if (c->type == MD_HEADING) {
            mdbuf s;
            mdbuf_init(&s);
            md_heading_slug(c, &s);
            int match = s.data && strcmp(s.data, slug) == 0;
            mdbuf_free(&s);
            if (match) return c;
        } else if (c->type == MD_BLOCKQUOTE || c->type == MD_LIST || c->type == MD_ITEM) {
            const md_node *r = find_heading(c, slug);
            if (r) return r;
        }
    }
    return NULL;
}

static void scroll_to_anchor(const WCHAR *anchor)
{
    char *slug = util_wide_to_utf8(anchor, -1, NULL);
    int len = 0;
    char *text = editor_utf8(&len);
    if (slug && text) {
        for (char *p = slug; *p; p++)
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        md_node *doc = md_parse(text, (size_t)len);
        const md_node *h = find_heading(doc, slug);
        if (h) {
            int k = marker_for_line(h->line);
            RECT fr;
            SendMessageW(g_prev, EM_GETRECT, 0, (LPARAM)&fr);
            if (k >= 0) scroll_so_char_at(g_prev, g_mcp[k], fr.top);
        }
        md_free(doc);
    }
    free(text);
    free(slug);
}

static int is_markdown_path(const WCHAR *p)
{
    return util_path_has_ext(p, L".md") || util_path_has_ext(p, L".markdown") || util_path_has_ext(p, L".mdown") ||
           util_path_has_ext(p, L".mkd") || util_path_has_ext(p, L".txt");
}

static void open_link(const WCHAR *url)
{
    if (!url || !*url) return;
    if (url[0] == '#') {
        scroll_to_anchor(url + 1);
        return;
    }
    if (_wcsnicmp(url, L"http://", 7) == 0 || _wcsnicmp(url, L"https://", 8) == 0 ||
        _wcsnicmp(url, L"mailto:", 7) == 0) {
        ShellExecuteW(g_main, L"open", url, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    char *u8 = util_wide_to_utf8(url, -1, NULL);
    if (!u8) return;
    WCHAR path[DOMDLY_PATH_MAX];
    int local = resolve_local(u8, path, DOMDLY_PATH_MAX);
    const char *hash = strchr(u8, '#');
    WCHAR *anchor = hash ? util_utf8_to_wide(hash + 1, -1) : NULL;
    free(u8);
    if (!local) {
        MessageBoxW(g_main, L"Domdly only opens web (http/https), e-mail and local file links.", L"Domdly",
                    MB_ICONINFORMATION | MB_OK);
        free(anchor);
        return;
    }
    if (util_file_exists(path)) {
        if (is_markdown_path(path)) {
            if (open_file(path) && anchor) scroll_to_anchor(anchor);
        } else {
            /* Never execute local files from a document: show them instead. */
            WCHAR args[DOMDLY_PATH_MAX + 16];
            swprintf(args, DOMDLY_PATH_MAX + 16, L"/select,\"%s\"", path);
            ShellExecuteW(g_main, L"open", L"explorer.exe", args, NULL, SW_SHOWNORMAL);
        }
    } else if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        ShellExecuteW(g_main, L"open", L"explorer.exe", path, NULL, SW_SHOWNORMAL);
    } else {
        WCHAR msg[DOMDLY_PATH_MAX + 64];
        swprintf(msg, DOMDLY_PATH_MAX + 64, L"The linked file was not found:\n%s", path);
        MessageBoxW(g_main, msg, L"Domdly", MB_ICONWARNING | MB_OK);
    }
    free(anchor);
}

/* RichEdit reports the whole hyperlink field: instruction + visible text. */
static void on_link_click(const ENLINK *el)
{
    g_link_clicked = 1;
    WCHAR *t = get_range(g_prev, el->chrg.cpMin, el->chrg.cpMax);
    if (!t) return;
    WCHAR *url = NULL;
    WCHAR *h = wcsstr(t, L"HYPERLINK");
    if (h) {
        WCHAR *q = wcschr(h, L'"');
        if (q) {
            WCHAR *e = wcschr(q + 1, L'"');
            if (e) {
                *e = 0;
                url = q + 1;
            }
        }
    }
    if (!url) url = t;
    while (*url == ' ') url++;
    for (size_t n = wcslen(url); n && url[n - 1] == ' '; n--) url[n - 1] = 0;
    open_link(url);
    free(t);
}

/* ------------------------------------------------------------------------ */
/* Editing commands                                                         */
/* ------------------------------------------------------------------------ */

static void cmd_wrap(const WCHAR *pre, const WCHAR *post, const WCHAR *placeholder)
{
    ensure_edit();
    CHARRANGE cr = get_sel(g_edit);
    size_t pl = wcslen(pre), ql = wcslen(post);
    WCHAR *sel = get_range(g_edit, cr.cpMin, cr.cpMax);
    if (!sel) return;
    size_t sl = wcslen(sel);
    size_t cap = sl + pl + ql + wcslen(placeholder) + 1;
    WCHAR *out = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!out) {
        free(sel);
        return;
    }
    int done = 0;
    if (sl >= pl + ql && wcsncmp(sel, pre, pl) == 0 && wcscmp(sel + sl - ql, post) == 0) {
        wcsncpy(out, sel + pl, sl - pl - ql);
        out[sl - pl - ql] = 0;
        replace_sel(out);
        set_sel(g_edit, cr.cpMin, cr.cpMin + (LONG)(sl - pl - ql));
        done = 1;
    } else if (cr.cpMin >= (LONG)pl) {
        WCHAR *before = get_range(g_edit, cr.cpMin - (LONG)pl, cr.cpMin);
        WCHAR *after = get_range(g_edit, cr.cpMax, cr.cpMax + (LONG)ql);
        if (before && after && wcscmp(before, pre) == 0 && wcscmp(after, post) == 0) {
            set_sel(g_edit, cr.cpMin - (LONG)pl, cr.cpMax + (LONG)ql);
            replace_sel(sel);
            set_sel(g_edit, cr.cpMin - (LONG)pl, cr.cpMin - (LONG)pl + (LONG)sl);
            done = 1;
        }
        free(before);
        free(after);
    }
    if (!done) {
        const WCHAR *inner = sl ? sel : placeholder;
        swprintf(out, cap, L"%s%s%s", pre, inner, post);
        replace_sel(out);
        set_sel(g_edit, cr.cpMin + (LONG)pl, cr.cpMin + (LONG)(pl + wcslen(inner)));
    }
    free(out);
    free(sel);
    SetFocus(g_edit);
}

static void cmd_link(int image)
{
    ensure_edit();
    CHARRANGE cr = get_sel(g_edit);
    WCHAR *sel = get_range(g_edit, cr.cpMin, cr.cpMax);
    if (!sel) return;
    const WCHAR *label = *sel ? sel : (image ? L"description" : L"text");
    const WCHAR *url = image ? L"image.png" : L"https://";
    size_t cap = wcslen(label) + wcslen(url) + 8;
    WCHAR *out = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (out) {
        swprintf(out, cap, L"%s[%s](%s)", image ? L"!" : L"", label, url);
        replace_sel(out);
        LONG base = cr.cpMin + (image ? 1 : 0);
        if (*sel) set_sel(g_edit, base + (LONG)wcslen(label) + 3, base + (LONG)(wcslen(label) + 3 + wcslen(url)));
        else set_sel(g_edit, base + 1, base + 1 + (LONG)wcslen(label));
        free(out);
    }
    free(sel);
    SetFocus(g_edit);
}

enum { PFX_HEADING, PFX_QUOTE, PFX_BULLET, PFX_NUMBER, PFX_TASK };

/* Length of an existing block prefix (heading, quote, list, task). */
static size_t block_prefix_len(const WCHAR *s)
{
    size_t i = 0;
    while (s[i] == ' ') i++;
    size_t k = i;
    while (s[k] == '#' && k - i < 6) k++;
    if (k > i && s[k] == ' ') return k + 1;
    if (s[i] == '>') return s[i + 1] == ' ' ? i + 2 : i + 1;
    if ((s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
        if (s[i + 2] == '[' && (s[i + 3] == ' ' || s[i + 3] == 'x' || s[i + 3] == 'X') && s[i + 4] == ']' &&
            s[i + 5] == ' ')
            return i + 6;
        return i + 2;
    }
    k = i;
    while (s[k] >= '0' && s[k] <= '9') k++;
    if (k > i && (s[k] == '.' || s[k] == ')') && s[k + 1] == ' ') return k + 2;
    return 0;
}

static int has_prefix(const WCHAR *line, int kind, int level)
{
    size_t i = 0;
    switch (kind) {
    case PFX_HEADING:
        while (line[i] == '#') i++;
        return (int)i == level && line[i] == ' ';
    case PFX_QUOTE:
        return line[0] == '>';
    case PFX_BULLET:
        return (line[0] == '-' || line[0] == '*' || line[0] == '+') && line[1] == ' ' && line[2] != '[';
    case PFX_NUMBER:
        while (line[i] >= '0' && line[i] <= '9') i++;
        return i > 0 && (line[i] == '.' || line[i] == ')') && line[i + 1] == ' ';
    case PFX_TASK:
        return line[0] == '-' && line[1] == ' ' && line[2] == '[' && line[4] == ']';
    }
    return 0;
}

static void cmd_line_prefix(int kind, int level)
{
    ensure_edit();
    CHARRANGE cr = get_sel(g_edit);
    LONG start, end, last_start;
    para_at(cr.cpMin, &start, NULL, NULL);
    LONG to = cr.cpMax;
    if (to > cr.cpMin) {
        para_at(to, &last_start, NULL, NULL);
        if (last_start == to) to--; /* selection ends at the start of a line */
    }
    para_at(to, NULL, &end, NULL);
    if (end < start) end = start;
    WCHAR *text = get_range(g_edit, start, end);
    if (!text) return;

    int all = 1, lines = 0;
    for (WCHAR *p = text;;) {
        WCHAR *e = wcschr(p, L'\r');
        WCHAR save = 0;
        if (e) {
            save = *e;
            *e = 0;
        }
        if (*p && !has_prefix(p, kind, level)) all = 0;
        lines++;
        if (e) *e = save;
        if (!e) break;
        p = e + 1;
    }

    mdbuf out; /* UTF-16 bytes */
    mdbuf_init(&out);
    int number = 1;
    for (WCHAR *p = text;;) {
        WCHAR *e = wcschr(p, L'\r');
        size_t n = e ? (size_t)(e - p) : wcslen(p);
        WCHAR line[4096];
        if (n >= 4000) n = 4000;
        wmemcpy(line, p, n);
        line[n] = 0;
        size_t skip = block_prefix_len(line);
        const WCHAR *body = line + skip;
        WCHAR prefix[32] = L"";
        if (!all && (*line || lines == 1)) {
            switch (kind) {
            case PFX_HEADING:
                for (int i = 0; i < level; i++) prefix[i] = '#';
                prefix[level] = ' ';
                prefix[level + 1] = 0;
                break;
            case PFX_QUOTE: wcscpy(prefix, L"> "); break;
            case PFX_BULLET: wcscpy(prefix, L"- "); break;
            case PFX_NUMBER: swprintf(prefix, 32, L"%d. ", number++); break;
            case PFX_TASK: wcscpy(prefix, L"- [ ] "); break;
            }
        } else if (!*line) {
            body = line;
        }
        mdbuf_put(&out, (const char *)prefix, wcslen(prefix) * sizeof(WCHAR));
        mdbuf_put(&out, (const char *)body, wcslen(body) * sizeof(WCHAR));
        if (!e) break;
        WCHAR cr_char = L'\r';
        mdbuf_put(&out, (const char *)&cr_char, sizeof cr_char);
        p = e + 1;
    }
    WCHAR zero = 0;
    mdbuf_put(&out, (const char *)&zero, sizeof zero);
    set_sel(g_edit, start, end);
    replace_sel((const WCHAR *)out.data);
    LONG new_len = (LONG)(out.len / sizeof(WCHAR)) - 1;
    if (cr.cpMin == cr.cpMax && lines == 1) set_sel(g_edit, start + new_len, start + new_len);
    else set_sel(g_edit, start, start + new_len);
    mdbuf_free(&out);
    free(text);
    SetFocus(g_edit);
}

static void insert_block(const WCHAR *before, const WCHAR *after, const WCHAR *placeholder)
{
    ensure_edit();
    CHARRANGE cr = get_sel(g_edit);
    LONG ls;
    para_at(cr.cpMin, &ls, NULL, NULL);
    WCHAR *sel = get_range(g_edit, cr.cpMin, cr.cpMax);
    if (!sel) return;
    const WCHAR *inner = *sel ? sel : placeholder;
    const WCHAR *lead = cr.cpMin > ls ? L"\r" : L"";
    size_t cap = wcslen(before) + wcslen(inner) + wcslen(after) + 4;
    WCHAR *out = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (out) {
        swprintf(out, cap, L"%s%s%s%s", lead, before, inner, after);
        replace_sel(out);
        LONG a = cr.cpMin + (LONG)(wcslen(lead) + wcslen(before));
        set_sel(g_edit, a, a + (LONG)wcslen(inner));
        free(out);
    }
    free(sel);
    SetFocus(g_edit);
}

/* Continues lists when Enter is pressed. Returns 1 if handled. */
static int handle_enter(void)
{
    CHARRANGE cr = get_sel(g_edit);
    if (cr.cpMin != cr.cpMax) return 0;
    LONG ls, le;
    para_at(cr.cpMin, &ls, &le, NULL);
    LONG ll = le - ls;
    if (ll <= 0 || ll > 4000) return 0;
    WCHAR *t = get_range(g_edit, ls, le);
    if (!t) return 0;
    LONG col = cr.cpMin - ls;
    size_t i = 0;
    while (t[i] == ' ') i++;
    size_t indent = i;
    WCHAR marker[24] = L"";
    size_t prefix = 0;
    if ((t[i] == '-' || t[i] == '*' || t[i] == '+') && t[i + 1] == ' ') {
        prefix = i + 2;
        swprintf(marker, 24, L"%c ", t[i]);
        if (t[i + 2] == '[' && (t[i + 3] == ' ' || t[i + 3] == 'x' || t[i + 3] == 'X') && t[i + 4] == ']' &&
            t[i + 5] == ' ') {
            prefix = i + 6;
            swprintf(marker, 24, L"%c [ ] ", t[i]);
        }
    } else {
        size_t k = i;
        long num = 0;
        while (t[k] >= '0' && t[k] <= '9' && k - i < 9) num = num * 10 + (t[k++] - '0');
        if (k > i && (t[k] == '.' || t[k] == ')') && t[k + 1] == ' ') {
            prefix = k + 2;
            swprintf(marker, 24, L"%ld%c ", num + 1, t[k]);
        }
    }
    int handled = 0;
    if (prefix && (size_t)col >= prefix) {
        int empty_item = 1;
        for (size_t k = prefix; t[k]; k++)
            if (t[k] != ' ') empty_item = 0;
        if (empty_item) {
            set_sel(g_edit, ls, le); /* Enter on an empty item ends the list */
            replace_sel(L"");
        } else {
            WCHAR ins[64];
            swprintf(ins, 64, L"\r%.*s%s", (int)(indent > 32 ? 32 : indent), L"                                ", marker);
            replace_sel(ins);
        }
        handled = 1;
    }
    free(t);
    return handled;
}

/* ------------------------------------------------------------------------ */
/* Find and replace                                                         */
/* ------------------------------------------------------------------------ */

static HWND find_target(void)
{
    return is_read() ? g_prev : g_edit;
}

static int find_next(const WCHAR *what, DWORD flags, int quiet)
{
    if (!what || !*what) return 0;
    HWND h = find_target();
    CHARRANGE cr = get_sel(h);
    int down = (flags & FR_DOWN) != 0;
    DWORD f = (down ? FR_DOWN : 0) | (flags & (FR_MATCHCASE | FR_WHOLEWORD));
    FINDTEXTEXW ft;
    ft.lpstrText = what;
    ft.chrg.cpMin = down ? cr.cpMax : cr.cpMin;
    ft.chrg.cpMax = down ? -1 : 0;
    LONG pos = (LONG)SendMessageW(h, EM_FINDTEXTEXW, f, (LPARAM)&ft);
    if (pos < 0) {
        ft.chrg.cpMin = 0;
        ft.chrg.cpMax = down ? -1 : 0;
        if (!down) {
            GETTEXTLENGTHEX gl = {GTL_NUMCHARS, 1200};
            ft.chrg.cpMin = (LONG)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
        }
        pos = (LONG)SendMessageW(h, EM_FINDTEXTEXW, f, (LPARAM)&ft);
    }
    if (pos < 0) {
        if (!quiet) {
            WCHAR msg[320];
            swprintf(msg, 320, L"Cannot find \"%s\".", what);
            MessageBoxW(g_find ? g_find : g_main, msg, L"Domdly", MB_ICONINFORMATION | MB_OK);
        }
        return 0;
    }
    set_sel(h, ft.chrgText.cpMin, ft.chrgText.cpMax);
    SendMessageW(h, EM_SCROLLCARET, 0, 0);
    return 1;
}

static void open_find(int replace)
{
    if (g_find) {
        DestroyWindow(g_find);
        g_find = NULL;
    }
    if (replace) ensure_edit();
    memset(&g_fr, 0, sizeof g_fr);
    g_fr.lStructSize = sizeof g_fr;
    g_fr.hwndOwner = g_main;
    g_fr.Flags = FR_DOWN;
    g_fr.lpstrFindWhat = g_find_text;
    g_fr.wFindWhatLen = (WORD)(sizeof g_find_text / sizeof g_find_text[0]);
    g_fr.lpstrReplaceWith = g_replace_text;
    g_fr.wReplaceWithLen = (WORD)(sizeof g_replace_text / sizeof g_replace_text[0]);
    HWND h = find_target();
    CHARRANGE cr = get_sel(h);
    if (cr.cpMax > cr.cpMin && cr.cpMax - cr.cpMin < 200) {
        WCHAR *sel = get_range(h, cr.cpMin, cr.cpMax);
        if (sel && !wcschr(sel, L'\r')) wcsncpy(g_find_text, sel, 255);
        free(sel);
    }
    g_find = replace ? ReplaceTextW(&g_fr) : FindTextW(&g_fr);
}

static void on_find_message(FINDREPLACEW *fr)
{
    if (fr->Flags & FR_DIALOGTERM) {
        g_find = NULL;
        return;
    }
    if (fr->Flags & FR_FINDNEXT) {
        find_next(fr->lpstrFindWhat, fr->Flags, 0);
    } else if (fr->Flags & FR_REPLACE) {
        ensure_edit();
        CHARRANGE cr = get_sel(g_edit);
        WCHAR *sel = get_range(g_edit, cr.cpMin, cr.cpMax);
        if (sel) {
            int same = (fr->Flags & FR_MATCHCASE) ? wcscmp(sel, fr->lpstrFindWhat) == 0
                                                  : _wcsicmp(sel, fr->lpstrFindWhat) == 0;
            if (same) replace_sel(fr->lpstrReplaceWith);
            free(sel);
        }
        find_next(fr->lpstrFindWhat, fr->Flags | FR_DOWN, 0);
    } else if (fr->Flags & FR_REPLACEALL) {
        ensure_edit();
        int count = 0;
        set_sel(g_edit, 0, 0);
        int frozen = freeze(g_edit);
        for (;;) {
            FINDTEXTEXW ft;
            CHARRANGE cr = get_sel(g_edit);
            ft.lpstrText = fr->lpstrFindWhat;
            ft.chrg.cpMin = cr.cpMax;
            ft.chrg.cpMax = -1;
            LONG pos = (LONG)SendMessageW(g_edit, EM_FINDTEXTEXW, FR_DOWN | (fr->Flags & (FR_MATCHCASE | FR_WHOLEWORD)),
                                          (LPARAM)&ft);
            if (pos < 0 || !*fr->lpstrFindWhat) break;
            set_sel(g_edit, ft.chrgText.cpMin, ft.chrgText.cpMax);
            replace_sel(fr->lpstrReplaceWith);
            if (++count > 1000000) break;
        }
        thaw(g_edit, frozen);
        WCHAR msg[96];
        swprintf(msg, 96, L"Replaced %d occurrence%s.", count, count == 1 ? L"" : L"s");
        MessageBoxW(g_find ? g_find : g_main, msg, L"Domdly", MB_ICONINFORMATION | MB_OK);
    }
}

/* ------------------------------------------------------------------------ */
/* Text window (license viewer)                                             */
/* ------------------------------------------------------------------------ */

static LRESULT CALLBACK text_child_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        PostMessageW(GetParent(h), WM_CLOSE, 0, 0);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static LRESULT CALLBACK text_wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE: {
        HWND child = GetWindow(h, GW_CHILD);
        if (child) {
            MoveWindow(child, 0, 0, LOWORD(l), HIWORD(l), TRUE);
            set_inner_rect(child, S_(22), S_(16));
        }
        return 0;
    }
    case WM_SETFOCUS: {
        HWND child = GetWindow(h, GW_CHILD);
        if (child) SetFocus(child);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static void show_text_window(const WCHAR *title, const char *utf8, size_t len)
{
    HWND w = CreateWindowExW(0, TEXT_CLASS, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S_(760), S_(640),
                             g_main, NULL, g_inst, NULL);
    if (!w) return;
    BOOL dark = g_dark;
    DwmSetWindowAttribute(w, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof dark);
    COLORREF caption = P.bar_bg;
    DwmSetWindowAttribute(w, DWMWA_CAPTION_COLOR_, &caption, sizeof caption);
    HWND re = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                              WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0,
                              10, 10, w, NULL, g_inst, NULL);
    SendMessageW(re, EM_SETTEXTMODE, TM_PLAINTEXT, 0);
    SendMessageW(re, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
    SetWindowTheme(re, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    SendMessageW(re, EM_SETBKGNDCOLOR, 0, (LPARAM)P.doc_bg);
    WCHAR *text = util_utf8_to_wide(utf8, (int)len);
    if (text) {
        stream_in(re, text, wcslen(text) * sizeof(WCHAR), SF_TEXT | SF_UNICODE);
        free(text);
    }
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR;
    cf.yHeight = 10 * 20;
    cf.crTextColor = P.doc_fg;
    wcsncpy(cf.szFaceName, g_ui_family, LF_FACESIZE - 1);
    SendMessageW(re, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SetWindowSubclass(re, text_child_proc, 1, 0);
    RECT rc;
    GetClientRect(w, &rc);
    SendMessageW(w, WM_SIZE, 0, MAKELPARAM(rc.right, rc.bottom));
    ShowWindow(w, SW_SHOW);
    SetFocus(re);
}

/* ------------------------------------------------------------------------ */
/* About                                                                    */
/* ------------------------------------------------------------------------ */

static HRESULT CALLBACK about_cb(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR data)
{
    (void)hwnd;
    (void)wp;
    (void)data;
    if (msg == TDN_HYPERLINK_CLICKED) ShellExecuteW(g_main, L"open", (LPCWSTR)lp, NULL, NULL, SW_SHOWNORMAL);
    return S_OK;
}

static void cmd_about(void)
{
    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof tc);
    tc.cbSize = sizeof tc;
    tc.hwndParent = g_main;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    tc.pszWindowTitle = L"About Domdly";
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.pszMainInstruction = L"Domdly " DOMDLY_VERSION_WSTR;
    tc.pszContent = L"A lightweight Markdown reader and editor.\n\n"
                    L"Copyright \x00A9 2026 Dogukan Sahil and Domdly contributors.\n\n"
                    L"This program is free software: you can redistribute it and/or modify it under the terms of the "
                    L"GNU General Public License, version 3. It comes with ABSOLUTELY NO WARRANTY. "
                    L"See Help \x2192 License for details.\n\n"
                    L"<a href=\"" DOMDLY_URL_WSTR L"\">github.com/dogukansahil/Domdly</a>";
    tc.dwCommonButtons = TDCBF_OK_BUTTON;
    tc.pfCallback = about_cb;
    TaskDialogIndirect(&tc, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------------ */
/* Menu                                                                     */
/* ------------------------------------------------------------------------ */

static void rebuild_recent_menu(void)
{
    if (!g_recent_menu) return;
    while (GetMenuItemCount(g_recent_menu) > 0) DeleteMenu(g_recent_menu, 0, MF_BYPOSITION);
    if (S.recent_count == 0) {
        AppendMenuW(g_recent_menu, MF_STRING | MF_GRAYED, 0, L"(empty)");
        return;
    }
    for (int i = 0; i < S.recent_count; i++) {
        WCHAR item[DOMDLY_PATH_MAX + 16];
        swprintf(item, DOMDLY_PATH_MAX + 16, L"&%d  %s", (i + 1) % 10, S.recent[i]);
        AppendMenuW(g_recent_menu, MF_STRING, IDM_RECENT_FIRST + i, item);
    }
    AppendMenuW(g_recent_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_recent_menu, MF_STRING, IDM_RECENT_CLEAR, L"&Clear Recent Files");
}

static void update_menu(void)
{
    HMENU m = g_menubar;
    CheckMenuRadioItem(m, IDM_VIEW_EDITOR, IDM_VIEW_PREVIEW, is_read() ? IDM_VIEW_PREVIEW : IDM_VIEW_EDITOR,
                       MF_BYCOMMAND);
    for (int t = THEME_SYSTEM; t <= THEME_NIGHT; t++) {
        UINT item = t == THEME_NIGHT ? IDM_THEME_NIGHT : (UINT)(IDM_THEME_SYSTEM + t);
        MENUITEMINFOW mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        mi.fMask = MIIM_FTYPE | MIIM_STATE;
        mi.fType = MFT_RADIOCHECK;
        mi.fState = S.theme == t ? MFS_CHECKED : MFS_UNCHECKED;
        SetMenuItemInfoW(m, item, FALSE, &mi);
    }
    CheckMenuItem(m, IDM_TOGGLE_TREE, MF_BYCOMMAND | (S.tree_open ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_TOGGLE_FMTBAR, MF_BYCOMMAND | (S.fmt_bar ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, IDM_WORDWRAP, MF_BYCOMMAND | (S.word_wrap ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(m, IDM_UNDO, MF_BYCOMMAND | (SendMessageW(g_edit, EM_CANUNDO, 0, 0) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, IDM_REDO, MF_BYCOMMAND | (SendMessageW(g_edit, EM_CANREDO, 0, 0) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, IDM_INSTALL, MF_BYCOMMAND | (inst_running_installed() ? MF_GRAYED : MF_ENABLED));
}

static void on_command(int id);
static int raw_source_cmd(int id);
static int wys_command(int id);
#define IDM_RAW_ENTER (-1) /* raw_source_cmd: list-aware Enter */

static void show_main_menu(void)
{
    HMENU pop = CreatePopupMenu();
    if (!pop) return;
    int n = GetMenuItemCount(g_menubar);
    for (int i = 0; i < n; i++) {
        WCHAR name[64];
        GetMenuStringW(g_menubar, (UINT)i, name, 64, MF_BYPOSITION);
        AppendMenuW(pop, MF_POPUP | MF_STRING, (UINT_PTR)GetSubMenu(g_menubar, i), name);
    }
    set_menu_theme(g_dark);
    update_menu();
    POINT pt = {g_rc_menu.left, g_rc_menu.bottom + S_(4)};
    ClientToScreen(g_main, &pt);
    g_pressed = HOT_MENU;
    invalidate_chrome();
    int cmd = (int)TrackPopupMenuEx(pop, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_main, NULL);
    /* The submenus belong to g_menubar: detach them before destroying. */
    while (GetMenuItemCount(pop) > 0) RemoveMenu(pop, 0, MF_BYPOSITION);
    DestroyMenu(pop);
    g_pressed = HOT_NONE;
    invalidate_chrome();
    if (cmd) on_command(cmd);
}

/* ------------------------------------------------------------------------ */
/* Commands                                                                 */
/* ------------------------------------------------------------------------ */

static void set_zoom(int zoom)
{
    if (zoom < 50) zoom = 50;
    if (zoom > 300) zoom = 300;
    S.zoom = zoom;
    SendMessageW(g_edit, EM_SETZOOM, (WPARAM)zoom, 100);
    g_preview_dirty = 1;
    if (is_read()) render_preview();
    invalidate_chrome();
    settings_save(&S);
}

static void set_word_wrap(int on)
{
    S.word_wrap = on;
    SendMessageW(g_edit, EM_SETTARGETDEVICE, 0, on ? 0 : 1);
    LONG_PTR style = GetWindowLongPtrW(g_edit, GWL_STYLE);
    if (on) style &= ~WS_HSCROLL;
    else style |= WS_HSCROLL;
    SetWindowLongPtrW(g_edit, GWL_STYLE, style);
    SetWindowPos(g_edit, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    layout();
}

static void set_theme(int theme)
{
    S.theme = theme;
    apply_theme();
    settings_save(&S);
}

static void on_command(int id)
{
    if (id >= IDM_RECENT_FIRST && id <= IDM_RECENT_LAST) {
        int i = id - IDM_RECENT_FIRST;
        if (i < S.recent_count) {
            WCHAR path[DOMDLY_PATH_MAX];
            wcscpy(path, S.recent[i]);
            open_file(path);
        }
        return;
    }
    /* In the Read view, editing commands act on the formatted document. */
    if (is_read() && g_raw_line < 0 && !g_raw_cmd && !g_welcome_shown && wys_command(id)) return;
    /* ... or on the block that is open as Markdown source. */
    if (is_read() && g_raw_line >= 0 && !g_raw_cmd) {
        /* typing in the region is undone there first (RichEdit groups it) */
        if (id == IDM_UNDO && SendMessageW(g_prev, EM_CANUNDO, 0, 0)) {
            SendMessageW(g_prev, EM_UNDO, 0, 0);
            return;
        }
        if (id == IDM_REDO && SendMessageW(g_prev, EM_CANREDO, 0, 0)) {
            SendMessageW(g_prev, EM_REDO, 0, 0);
            return;
        }
        if ((id >= IDM_BOLD && id <= IDM_IMAGE) || id == IDM_UNDO || id == IDM_REDO) {
            raw_source_cmd(id);
            return;
        }
        if (id == IDM_CUT) {
            SendMessageW(g_prev, WM_CUT, 0, 0);
            return;
        }
        if (id == IDM_PASTE) {
            SendMessageW(g_prev, WM_PASTE, 0, 0);
            return;
        }
    }
    switch (id) {
    case IDM_NEW:
        new_document(NULL);
        ensure_edit();
        break;
    case IDM_CLOSE_TAB: tab_close(g_cur); break;
    case IDM_REOPEN_TAB: reopen_closed(); break;
    case IDM_NEXT_TAB: tab_activate((g_cur + 1) % g_ntabs); break;
    case IDM_PREV_TAB: tab_activate((g_cur + g_ntabs - 1) % g_ntabs); break;
    case IDM_OPEN: cmd_open(); break;
    case IDM_SAVE: save_file(0); break;
    case IDM_SAVEAS: save_file(1); break;
    case IDM_EXPORT_HTML: cmd_export_html(); break;
    case IDM_EXPORT_PDF: cmd_export_pdf(); break;
    case IDM_RECENT_CLEAR:
        S.recent_count = 0;
        settings_save(&S);
        break;
    case IDM_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); break;

    case IDM_UNDO: ensure_edit(); SendMessageW(g_edit, EM_UNDO, 0, 0); break;
    case IDM_REDO: ensure_edit(); SendMessageW(g_edit, EM_REDO, 0, 0); break;
    case IDM_CUT: ensure_edit(); SendMessageW(g_edit, WM_CUT, 0, 0); break;
    case IDM_COPY: SendMessageW(find_target(), WM_COPY, 0, 0); break;
    case IDM_PASTE: ensure_edit(); SendMessageW(g_edit, WM_PASTE, 0, 0); break;
    case IDM_SELALL: set_sel(find_target(), 0, -1); break;
    case IDM_COPY_HTML: cmd_copy_html(); break;
    case IDM_FIND: open_find(0); break;
    case IDM_REPLACE: open_find(1); break;
    case IDM_FINDNEXT:
        if (g_find_text[0]) find_next(g_find_text, FR_DOWN | (g_fr.Flags & (FR_MATCHCASE | FR_WHOLEWORD)), 0);
        else open_find(0);
        break;

    case IDM_BOLD: cmd_wrap(L"**", L"**", L"bold text"); break;
    case IDM_ITALIC: cmd_wrap(L"*", L"*", L"italic text"); break;
    case IDM_STRIKE: cmd_wrap(L"~~", L"~~", L"struck text"); break;
    case IDM_CODE: cmd_wrap(L"`", L"`", L"code"); break;
    case IDM_LINK: cmd_link(0); break;
    case IDM_IMAGE: cmd_link(1); break;
    case IDM_H1: cmd_line_prefix(PFX_HEADING, 1); break;
    case IDM_H2: cmd_line_prefix(PFX_HEADING, 2); break;
    case IDM_H3: cmd_line_prefix(PFX_HEADING, 3); break;
    case IDM_QUOTE: cmd_line_prefix(PFX_QUOTE, 0); break;
    case IDM_BULLET: cmd_line_prefix(PFX_BULLET, 0); break;
    case IDM_NUMBERED: cmd_line_prefix(PFX_NUMBER, 0); break;
    case IDM_TASK: cmd_line_prefix(PFX_TASK, 0); break;
    case IDM_CODEBLOCK: insert_block(L"```\r", L"\r```\r", L"code"); break;
    case IDM_TABLE: insert_block(L"| Column 1 | Column 2 |\r| --- | --- |\r| ", L" | Cell |\r", L"Cell"); break;
    case IDM_HR: insert_block(L"\r---\r", L"", L""); break;

    case IDM_VIEW_EDITOR: ensure_edit(); break;
    case IDM_VIEW_PREVIEW: switch_to_read(); break;
    case IDM_TOGGLE_TREE: tree_toggle(); break;
    case IDM_TOGGLE_FMTBAR:
        S.fmt_bar = !S.fmt_bar;
        layout();
        settings_save(&S);
        break;
    case IDM_TREE_FOLDER: tree_pick_folder(); break;
    case IDM_TREE_FOLLOW:
        S.tree_root[0] = 0;
        settings_save(&S);
        tree_sync(1);
        break;
    case IDM_TREE_REFRESH: tree_sync(1); break;
    case IDM_TREE_REVEAL:
        if (g_tree_root[0]) ShellExecuteW(g_main, L"open", L"explorer.exe", g_tree_root, NULL, SW_SHOWNORMAL);
        break;
    case IDM_TOGGLE_MODE:
        if (is_read()) switch_to_edit(0, 0, 0);
        else switch_to_read();
        break;
    case IDM_THEME_SYSTEM:
    case IDM_THEME_LIGHT:
    case IDM_THEME_DARK: set_theme(id - IDM_THEME_SYSTEM); break;
    case IDM_THEME_NIGHT: set_theme(THEME_NIGHT); break;
    case IDM_THEME_TOGGLE: /* light -> dark navy -> night -> light */
        set_theme(g_night ? THEME_LIGHT : g_dark ? THEME_NIGHT : THEME_DARK);
        break;
    case IDM_WORDWRAP:
        set_word_wrap(!S.word_wrap);
        settings_save(&S);
        break;
    case IDM_ZOOM_IN: set_zoom(S.zoom + 10); break;
    case IDM_ZOOM_OUT: set_zoom(S.zoom - 10); break;
    case IDM_ZOOM_RESET: set_zoom(100); break;

    case IDM_SAMPLE:
        {
            size_t len = 0;
            const char *sample = util_resource(IDR_SAMPLE, &len);
            if (sample) {
                char *copy = (char *)malloc(len + 1);
                if (copy) {
                    memcpy(copy, sample, len);
                    copy[len] = 0;
                    new_document(copy);
                    free(copy);
                    switch_to_read();
                }
            }
        }
        break;
    case IDM_INSTALL:
        if (inst_interactive_install(g_main)) PostMessageW(g_main, WM_CLOSE, 0, 0);
        break;
    case IDM_UNINSTALL: inst_interactive_uninstall(g_main); break;
    case IDM_LICENSE: {
        size_t len = 0;
        const char *lic = util_resource(IDR_LICENSE, &len);
        if (lic) show_text_window(L"Domdly License (GNU GPL v3)", lic, len);
        break;
    }
    case IDM_WEBSITE: ShellExecuteW(g_main, L"open", DOMDLY_URL_WSTR, NULL, NULL, SW_SHOWNORMAL); break;
    case IDM_ABOUT: cmd_about(); break;
    case IDM_CHECK_UPDATES: cmd_check_updates(); break;
    }
}

/* ------------------------------------------------------------------------ */
/* Drag and drop: an OLE drop target on the main window shows a "Drop to    */
/* open" overlay while a file is dragged over Domdly.                         */
/* ------------------------------------------------------------------------ */

typedef struct drop_target {
    IDropTarget iface;
    LONG refs;
    int accept;
} drop_target;

/* Pictures dropped on the document are linked from it as Markdown images,
 * ![name](path), instead of being opened. */
typedef struct dropped_images {
    POINT pt;  /* screen point of the drop */
    char *md;  /* one image per line */
} dropped_images;

static int is_image_path(const WCHAR *p)
{
    static const WCHAR *const ext[] = {L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".svg"};
    const WCHAR *dot = wcsrchr(p, L'.');
    if (!dot || wcschr(dot, L'\\')) return 0;
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++)
        if (_wcsicmp(dot, ext[i]) == 0) return 1;
    return 0;
}

/* file relative to folder dir ("img\a.png", "..\pics\a.png"); the full path
 * when they share no more than the drive. */
static void relative_path(const WCHAR *dir, const WCHAR *file, WCHAR *out, size_t cap)
{
    size_t i = 0, last = 0;
    while (dir[i] && file[i] && towlower(dir[i]) == towlower(file[i])) {
        if (dir[i] == '\\') last = i + 1;
        i++;
    }
    if (!dir[i] && file[i] == '\\') last = i + 1;
    if (last <= 3) {
        wcsncpy(out, file, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    out[0] = 0;
    const WCHAR *r = last > wcslen(dir) ? L"" : dir + last; /* "" when dir holds the file */
    if (*r) {
        int ups = 1;
        for (const WCHAR *q = r; *q; q++)
            if (*q == '\\' && q[1]) ups++;
        for (int k = 0; k < ups && wcslen(out) + 4 < cap; k++) wcscat(out, L"..\\");
    }
    if (wcslen(out) + wcslen(file + last) < cap) wcscat(out, file + last);
}

/* "![name](path)" for an image file. The path is relative to the document
 * when it has been saved, uses forward slashes, and spaces and brackets are
 * percent-encoded so that it stays a valid Markdown link. */
static void append_image_md(mdbuf *o, const WCHAR *file)
{
    WCHAR rel[DOMDLY_PATH_MAX];
    if (g_has_path) {
        WCHAR dir[DOMDLY_PATH_MAX];
        util_path_dir(g_path, dir, DOMDLY_PATH_MAX);
        relative_path(dir, file, rel, DOMDLY_PATH_MAX);
    } else {
        wcsncpy(rel, file, DOMDLY_PATH_MAX - 1);
        rel[DOMDLY_PATH_MAX - 1] = 0;
    }
    for (WCHAR *q = rel; *q; q++)
        if (*q == '\\') *q = '/';
    WCHAR name[DOMDLY_PATH_MAX];
    wcsncpy(name, util_path_name(file), DOMDLY_PATH_MAX - 1);
    name[DOMDLY_PATH_MAX - 1] = 0;
    WCHAR *dot = wcsrchr(name, L'.');
    if (dot && dot != name) *dot = 0;
    char *alt = util_wide_to_utf8(name, -1, NULL), *url = util_wide_to_utf8(rel, -1, NULL);
    mdbuf_puts(o, "![");
    for (const char *c = alt ? alt : ""; *c; c++) {
        if (*c == '[' || *c == ']' || *c == '\\') mdbuf_putc(o, '\\');
        mdbuf_putc(o, *c);
    }
    mdbuf_puts(o, "](");
    for (const unsigned char *c = (const unsigned char *)(url ? url : ""); *c; c++) {
        if (*c == ' ' || *c == '(' || *c == ')' || *c == '<' || *c == '>' || *c == '%') mdbuf_printf(o, "%%%02X", *c);
        else mdbuf_putc(o, (char)*c);
    }
    mdbuf_putc(o, ')');
    free(alt);
    free(url);
}

static void raw_open(long line, int caret, POINT pt);
static void wys_render_at(int line, long off);
static void splice_source(int l1, int l2, const char *utf8);
static int editor_line_count(void);

/* Inserts the image links where they were dropped: at that spot of the
 * source in the Edit view, or as a paragraph after the block under the
 * pointer in the Read view. */
static void insert_dropped_images(const dropped_images *di)
{
    if (!di->md || !*di->md) return;
    if (!is_read()) {
        POINT c = di->pt;
        ScreenToClient(g_edit, &c);
        POINTL pl = {c.x, c.y};
        LONG cp = (LONG)SendMessageW(g_edit, EM_CHARFROMPOS, 0, (LPARAM)&pl);
        WCHAR *w = util_utf8_to_wide(di->md, -1);
        if (!w) return;
        for (WCHAR *q = w; *q; q++)
            if (*q == '\n') *q = '\r';
        set_sel(g_edit, cp, cp);
        replace_sel(w);
        free(w);
        SetFocus(g_edit);
        return;
    }
    if (g_raw_line >= 0) {
        POINT none = {0, 0};
        raw_open(-1, 0, none); /* finish the block that is open as source */
    }
    wys_flush();
    POINT c = di->pt;
    ScreenToClient(g_prev, &c);
    POINTL pl = {c.x, c.y};
    LONG cp = (LONG)SendMessageW(g_prev, EM_CHARFROMPOS, 0, (LPARAM)&pl);
    int k = g_welcome_shown ? -1 : marker_for_cp(cp);
    int at = k >= 0 ? g_blk[k].end + 1 : editor_line_count();
    mdbuf o;
    mdbuf_init(&o);
    mdbuf_putc(&o, '\n');
    for (const char *q = di->md; *q; q++) {
        mdbuf_putc(&o, *q);
        if (*q == '\n') mdbuf_putc(&o, '\n'); /* each picture in a paragraph of its own */
    }
    splice_source(at, at - 1, o.data ? o.data : "");
    mdbuf_free(&o);
    wys_render_at(at + 1, 0);
}

static int first_dropped_file(IDataObject *data, WCHAR *out, UINT cap)
{
    FORMATETC fe = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm;
    if (FAILED(data->lpVtbl->GetData(data, &fe, &sm))) return 0;
    int ok = DragQueryFileW((HDROP)sm.hGlobal, 0, out, cap) > 0;
    ReleaseStgMedium(&sm);
    return ok;
}

static void show_drop(int on)
{
    if (!g_drop) return;
    if (on) {
        SetWindowPos(g_drop, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        InvalidateRect(g_drop, NULL, FALSE);
    } else {
        ShowWindow(g_drop, SW_HIDE);
    }
}

static HRESULT STDMETHODCALLTYPE dt_query_interface(IDropTarget *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dt_add_ref(IDropTarget *self)
{
    return (ULONG)InterlockedIncrement(&((drop_target *)self)->refs);
}

static ULONG STDMETHODCALLTYPE dt_release(IDropTarget *self)
{
    drop_target *t = (drop_target *)self;
    LONG r = InterlockedDecrement(&t->refs);
    if (r == 0) free(t);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE dt_drag_enter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    (void)keys;
    (void)pt;
    drop_target *t = (drop_target *)self;
    WCHAR path[DOMDLY_PATH_MAX];
    t->accept = first_dropped_file(data, path, DOMDLY_PATH_MAX);
    if (t->accept && !is_image_path(path)) { /* pictures are linked where they land: no curtain */
        wcsncpy(g_drop_name, util_path_name(path), DOMDLY_PATH_MAX - 1);
        show_drop(1);
    }
    *effect = t->accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_drag_over(IDropTarget *self, DWORD keys, POINTL pt, DWORD *effect)
{
    (void)keys;
    (void)pt;
    *effect = ((drop_target *)self)->accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_drag_leave(IDropTarget *self)
{
    (void)self;
    show_drop(0);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_drop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    (void)keys;
    (void)pt;
    drop_target *t = (drop_target *)self;
    show_drop(0);
    FORMATETC fe = {CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm;
    if (t->accept && SUCCEEDED(data->lpVtbl->GetData(data, &fe, &sm))) {
        /* Every dropped file opens in its own tab, after the drag source is
         * released so that dialogs cannot block it. */
        UINT n = DragQueryFileW((HDROP)sm.hGlobal, 0xFFFFFFFF, NULL, 0);
        mdbuf imgs;
        mdbuf_init(&imgs);
        for (UINT i = 0; i < n && i < MAX_TABS; i++) {
            WCHAR path[DOMDLY_PATH_MAX];
            if (!DragQueryFileW((HDROP)sm.hGlobal, i, path, DOMDLY_PATH_MAX)) continue;
            if (is_image_path(path)) {
                if (imgs.len) mdbuf_putc(&imgs, '\n');
                append_image_md(&imgs, path);
                continue;
            }
            WCHAR *copy = _wcsdup(path);
            if (copy) PostMessageW(g_main, WM_APP_OPEN, 0, (LPARAM)copy);
        }
        ReleaseStgMedium(&sm);
        dropped_images *di = imgs.len ? (dropped_images *)calloc(1, sizeof *di) : NULL;
        if (di) {
            di->pt.x = pt.x;
            di->pt.y = pt.y;
            di->md = mdbuf_detach(&imgs);
            PostMessageW(g_main, WM_APP_IMAGES, 0, (LPARAM)di);
        } else {
            mdbuf_free(&imgs);
        }
        *effect = DROPEFFECT_COPY;
    } else {
        *effect = DROPEFFECT_NONE;
    }
    return S_OK;
}

static IDropTargetVtbl dt_vtbl = {dt_query_interface, dt_add_ref,   dt_release, dt_drag_enter,
                                  dt_drag_over,       dt_drag_leave, dt_drop};

static drop_target *g_drop_target;

/* RichEdit registers its own drop target, which would take a dropped file
 * as text or as an embedded object. Domdly's target replaces it, so a file
 * dropped on either view opens in a tab. */
static void take_over_drop(HWND h)
{
    if (!h || !g_drop_target) return;
    RevokeDragDrop(h);
    RegisterDragDrop(h, &g_drop_target->iface);
}

static void register_drop_target(void)
{
    drop_target *t = (drop_target *)calloc(1, sizeof *t);
    if (!t) return;
    t->iface.lpVtbl = &dt_vtbl;
    t->refs = 1;
    g_drop_target = t; /* keeps the creation reference until exit */
    if (FAILED(RegisterDragDrop(g_main, &t->iface))) DragAcceptFiles(g_main, TRUE); /* fallback */
    take_over_drop(g_prev);
    for (int i = 0; i < g_ntabs; i++) take_over_drop(i == g_cur ? g_edit : g_tabs[i].edit);
}

static LRESULT CALLBACK drop_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mdc, bmp);
        fill_rect(mdc, &rc, P.doc_bg);
        RECT box = rc;
        InflateRect(&box, -S_(24), -S_(24));
        fill_round(mdc, &box, SF(16), P.drop_bg);
        stroke_round(mdc, &box, SF(16), SF(2), P.accent);
        RECT t1 = box, t2 = box;
        int mid = (box.top + box.bottom) / 2;
        t1.bottom = mid;
        t2.top = mid + S_(6);
        draw_text(mdc, L"Drop to open", &t1, g_font_big, P.bar_fg, DT_SINGLELINE | DT_CENTER | DT_BOTTOM);
        draw_text(mdc, g_drop_name, &t2, g_font_ui, P.bar_muted, DT_SINGLELINE | DT_CENTER | DT_TOP | DT_PATH_ELLIPSIS);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
        SelectObject(mdc, old);
        DeleteObject(bmp);
        DeleteDC(mdc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------------ */
/* Document controls                                                        */
/* ------------------------------------------------------------------------ */

static BOOL wheel_scroll(HWND h, WPARAM w);

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (m == WM_MOUSEWHEEL && wheel_scroll(h, w)) return 0;
    if (m == WM_KEYDOWN) {
        int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (w == VK_RETURN && !ctrl && !alt && !shift && handle_enter()) return 0;
        /* RichEdit paragraph formatting shortcuts make no sense in Markdown. */
        if (ctrl && !alt && (w == 'E' || w == 'L' || w == 'R' || w == 'J' || w == '5' || w == VK_OEM_PLUS)) return 0;
    }
    LRESULT r = DefSubclassProc(h, m, w, l);
    if (m == WM_PAINT && GetWindowTextLengthW(h) == 0) {
        /* placeholder for an empty document */
        HDC dc = GetDC(h);
        RECT fr;
        SendMessageW(h, EM_GETRECT, 0, (LPARAM)&fr);
        draw_text(dc, L"Start writing Markdown, or drop a file here.", &fr, g_font_ui, P.bar_muted,
                  DT_SINGLELINE | DT_LEFT | DT_TOP);
        ReleaseDC(h, dc);
    }
    return r;
}

/* The largest vertical scroll position at which text still fills the view:
   the end of the document sits one line above the bottom edge. */
static long scroll_limit(HWND h)
{
    GETTEXTLENGTHEX gl = {GTL_NUMCHARS | GTL_PRECISE, 1200};
    LONG len = (LONG)SendMessageW(h, EM_GETTEXTLENGTHEX, (WPARAM)&gl, 0);
    POINT pos;
    SendMessageW(h, EM_GETSCROLLPOS, 0, (LPARAM)&pos);
    POINTL end = {0, 0};
    SendMessageW(h, EM_POSFROMCHAR, (WPARAM)&end, len);
    RECT fr;
    SendMessageW(h, EM_GETRECT, 0, (LPARAM)&fr);
    long line = (long)MulDiv(11 * 14, g_dpi, 720) * (S.zoom > 0 ? S.zoom : 100) / 100;
    long max = pos.y + end.y + line - fr.bottom;
    return max < 0 ? 0 : max;
}

/* RichEdit animates every wheel notch over roughly 300 ms, which feels
   sluggish. Domdly scrolls by the same distance at once instead. Ctrl+wheel
   goes through set_zoom so the status bar stays in step. */
static BOOL wheel_scroll(HWND h, WPARAM w)
{
    static HWND last;
    static int rest; /* wheel delta not yet turned into pixels (touchpads) */
    int delta = GET_WHEEL_DELTA_WPARAM(w);
    if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) {
        rest = 0;
        if (delta != 0) set_zoom(S.zoom + (delta > 0 ? 10 : -10));
        return TRUE;
    }
    if (GET_KEYSTATE_WPARAM(w) & MK_SHIFT) return FALSE;
    if (h != last) {
        last = h;
        rest = 0;
    }
    if ((delta > 0) != (rest > 0)) rest = 0; /* direction changed */
    rest += delta;

    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == 0) return TRUE;
    RECT rc;
    GetClientRect(h, &rc);
    long step; /* pixels per full notch */
    if (lines == WHEEL_PAGESCROLL) {
        step = rc.bottom - rc.top - S_(40);
    } else {
        /* one line of 11 pt body text at about 1.4 line spacing, zoomed */
        long line = (long)MulDiv(11 * 14, g_dpi, 720) * (S.zoom > 0 ? S.zoom : 100) / 100;
        step = line * (long)lines;
    }
    if (step < 1) step = 1;
    long px = (long)rest * step / WHEEL_DELTA;
    if (px == 0) return TRUE;
    rest -= (int)(px * WHEEL_DELTA / step);

    POINT pos;
    SendMessageW(h, EM_GETSCROLLPOS, 0, (LPARAM)&pos);
    if (px < 0) {
        /* EM_SETSCROLLPOS happily scrolls past the end of the text into an
           empty page, so stop once the last line reaches the bottom. */
        long max = scroll_limit(h);
        if (pos.y >= max) return TRUE;
        pos.y -= px;
        if (pos.y > max) pos.y = max;
    } else {
        pos.y -= px;
        if (pos.y < 0) pos.y = 0;
    }
    SendMessageW(h, EM_SETSCROLLPOS, 0, (LPARAM)&pos);
    return TRUE;
}

static int editor_line_count(void)
{
    int n = 0, lines = 1;
    WCHAR *t = control_text(g_edit, &n);
    for (int i = 0; t && i < n; i++)
        if (t[i] == '\r') lines++;
    free(t);
    return lines;
}

/* Runs an editor command (or the list-aware Enter) on the region's source
 * lines: the Read view selection is mapped into the editor, the command runs
 * there, and the region is rebuilt with the selection mapped back. Returns 0
 * when the command did nothing. */
static int raw_source_cmd(int id)
{
    sync_raw(0);
    LONG a, b;
    if (!raw_caret_to_source(&a, &b)) return 0;
    set_sel(g_edit, a, b);
    int before = editor_line_count();
    g_raw_cmd = 1;
    g_syncing = 1;
    int done = 1;
    if (id == IDM_RAW_ENTER) done = handle_enter();
    else on_command(id);
    g_syncing = 0;
    g_raw_cmd = 0;
    if (!done) return 0;
    g_raw_count += editor_line_count() - before;
    if (g_raw_count < 1) g_raw_count = 1;
    CHARRANGE sel = get_sel(g_edit);

    int frozen = freeze(g_prev);
    g_hold_redraw = 1;
    render_preview();
    g_hold_redraw = 0;
    raw_caret_from_source(sel.cpMin, sel.cpMax);
    SendMessageW(g_prev, EM_SCROLLCARET, 0, 0);
    thaw(g_prev, frozen);
    SetFocus(g_prev);
    update_title();
    return 1;
}

/* Character formatting in the Read view: toggles bold, italic,
 * strikethrough or inline code on the selection (or for the next typing). */
static void wys_toggle(int id, int k)
{
    if (g_wys_dirty >= 0 && g_wys_dirty != k) wys_flush();
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_STRIKEOUT | CFM_FACE;
    SendMessageW(g_prev, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    CHARFORMAT2W nf;
    memset(&nf, 0, sizeof nf);
    nf.cbSize = sizeof nf;
    if (id == IDM_BOLD || id == IDM_ITALIC || id == IDM_STRIKE) {
        DWORD mask = id == IDM_BOLD ? CFM_BOLD : id == IDM_ITALIC ? CFM_ITALIC : CFM_STRIKEOUT;
        DWORD eff = id == IDM_BOLD ? CFE_BOLD : id == IDM_ITALIC ? CFE_ITALIC : CFE_STRIKEOUT;
        int on = (cf.dwMask & mask) && (cf.dwEffects & eff);
        nf.dwMask = mask;
        nf.dwEffects = on ? 0 : eff;
    } else { /* inline code */
        int code = (cf.dwMask & CFM_FACE) && _wcsicmp(cf.szFaceName, g_mono_family) == 0;
        md_rtf_options opt;
        md_rtf_default_options(&opt, g_dark);
        if (g_night) night_colors(&opt.colors);
        unsigned long bg = opt.colors.code_bg;
        nf.dwMask = CFM_FACE | CFM_BACKCOLOR;
        wcsncpy(nf.szFaceName, code ? g_ui_family : g_mono_family, LF_FACESIZE - 1);
        if (code) nf.dwEffects = CFE_AUTOBACKCOLOR;
        else nf.crBackColor = RGB((bg >> 16) & 255, (bg >> 8) & 255, bg & 255);
    }
    SendMessageW(g_prev, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&nf);
    g_wys_dirty = k;
    SetTimer(g_main, TIMER_SYNC, 500, NULL);
}

static int is_line_command(int id)
{
    return (id >= IDM_H1 && id <= IDM_H3) || id == IDM_QUOTE || id == IDM_BULLET || id == IDM_NUMBERED ||
           id == IDM_TASK;
}

/* Editing commands while the Read view is edited like a document. Returns
 * 1 when the command was handled. */
static int wys_command(int id)
{
    LONG sa, sb;
    int k2, k = edit_target(&sa, &sb, &k2);
    if (id == IDM_BOLD || id == IDM_ITALIC || id == IDM_STRIKE || id == IDM_CODE) {
        if (k < 0 || k2 != k || !(is_textual(k) || g_blk[k].kind == BK_ROW)) return 1;
        wys_toggle(id, k);
        return 1;
    }
    if (is_line_command(id)) {
        if (g_pend != -1 && sa == sb && sa == g_pend_cp) {
            /* a new empty paragraph becomes an empty heading or list item */
            const char *pre = id == IDM_H1 ? "# " : id == IDM_H2 ? "## " : id == IDM_H3 ? "### " :
                              id == IDM_BULLET ? "- " : id == IDM_NUMBERED ? "1. " : id == IDM_TASK ? "- [ ] " : NULL;
            if (pre) op_pending_type(pre, 1);
            return 1;
        }
        /* headings, quotes and lists are line prefixes: change the source */
        if (!is_textual(k)) return 1;
        wys_flush();
        LONG a, b;
        if (!blk_range(k, &a, &b)) return 1;
        long off = vis_count(a, sa);
        int line = g_blk[k].line;
        LONG es = para_start_of_line(line), ee;
        para_at(para_start_of_line(g_blk[k].end), NULL, &ee, NULL);
        set_sel(g_edit, es, ee);
        g_raw_cmd = 1;
        g_syncing = 1;
        on_command(id);
        g_syncing = 0;
        g_raw_cmd = 0;
        update_title();
        wys_render_at(line, off);
        return 1;
    }
    if (id == IDM_CODEBLOCK || id == IDM_TABLE || id == IDM_HR) {
        wys_flush();
        int kk = k >= 0 ? k : marker_for_cp(sa);
        int at = kk >= 0 ? g_blk[kk].end + 1 : editor_line_count();
        const char *text = id == IDM_CODEBLOCK ? "\n```\n\n```"
                           : id == IDM_TABLE   ? "\n| Column 1 | Column 2 |\n| --- | --- |\n|  |  |"
                                               : "\n---\n";
        splice_source(at, at - 1, text);
        wys_render_at(id == IDM_HR ? (kk >= 0 ? g_blk[kk].line : 0) : at + 1, id == IDM_HR ? 1L << 30 : 0);
        return 1;
    }
    if (id == IDM_LINK || id == IDM_IMAGE) {
        /* the address has to be typed as text: insert the Markdown and open
         * the block as source with the address selected */
        if (!is_textual(k) || k2 != k) return 1;
        wys_flush();
        LONG a, b;
        if (!blk_range(k, &a, &b)) return 1;
        mdbuf o;
        mdbuf_init(&o);
        blk_lead(k, &o);
        blk_inline(k, a, sa, 1, &o);
        if (id == IDM_IMAGE) {
            WCHAR *alt = get_range(g_prev, sa, sb);
            char *u = alt && *alt ? util_wide_to_utf8(alt, -1, NULL) : NULL;
            mdbuf_puts(&o, "![");
            mdbuf_puts(&o, u ? u : "description");
            mdbuf_puts(&o, "](image.png)");
            free(u);
            free(alt);
        } else {
            mdbuf_putc(&o, '[');
            if (sb > sa) blk_inline(k, sa, sb, 1, &o);
            else mdbuf_puts(&o, "text");
            mdbuf_puts(&o, "](https://)");
        }
        blk_inline(k, sb, b, 1, &o);
        int line = g_blk[k].line;
        splice_source(g_blk[k].line, g_blk[k].end, o.data ? o.data : "");
        mdbuf_free(&o);
        POINT none = {0, 0};
        raw_open(line, 0, none);
        LONG rs, re;
        if (raw_bounds(&rs, &re)) {
            FINDTEXTEXW ft;
            ft.chrg.cpMin = rs;
            ft.chrg.cpMax = re;
            ft.lpstrText = id == IDM_IMAGE ? L"image.png" : L"https://";
            LONG cp = (LONG)SendMessageW(g_prev, EM_FINDTEXTEXW, FR_DOWN | FR_MATCHCASE, (LPARAM)&ft);
            if (cp >= 0) set_sel(g_prev, cp, cp + (LONG)wcslen(ft.lpstrText));
        }
        return 1;
    }
    if (id == IDM_UNDO || id == IDM_REDO) {
        int undo = id == IDM_UNDO;
        if (SendMessageW(g_prev, undo ? EM_CANUNDO : EM_CANREDO, 0, 0)) {
            /* typing since the last render: RichEdit undoes it itself */
            SendMessageW(g_prev, undo ? EM_UNDO : EM_REDO, 0, 0);
            locate_markers();
            CHARRANGE cr = get_sel(g_prev);
            int kk = marker_for_cp(cr.cpMin);
            if (kk >= 0 && g_blk[kk].editable) {
                g_wys_dirty = kk;
                SetTimer(g_main, TIMER_SYNC, 500, NULL);
            }
            return 1;
        }
        wys_flush();
        g_syncing = 1;
        SendMessageW(g_edit, undo ? EM_UNDO : EM_REDO, 0, 0);
        g_syncing = 0;
        CHARRANGE cr = get_sel(g_edit);
        long line;
        para_at(cr.cpMin, NULL, NULL, &line);
        update_title();
        int kk = marker_for_line(line);
        wys_render_at(kk >= 0 ? g_mline[kk] : (int)line, 0);
        return 1;
    }
    if (id == IDM_CUT) {
        SendMessageW(g_prev, WM_CUT, 0, 0);
        return 1;
    }
    if (id == IDM_PASTE) {
        wys_paste();
        return 1;
    }
    return 0;
}

/* Flips a task list check box in the source. */
static void toggle_task(int k)
{
    wys_flush();
    blk *B = &g_blk[k];
    mdbuf o;
    mdbuf_init(&o);
    for (const char *p = B->pfx; *p; p++) {
        if (p[0] == '[' && p[1] && p[2] == ']') {
            mdbuf_puts(&o, p[1] == ' ' ? "[x]" : "[ ]");
            p += 2;
        } else {
            mdbuf_putc(&o, *p);
        }
    }
    LONG a, b;
    if (blk_range(k, &a, &b)) blk_inline(k, a, b, 1, &o);
    int line = B->line;
    splice_source(B->line, B->end, o.data ? o.data : "");
    mdbuf_free(&o);
    wys_render_at(line, 0);
}

static LRESULT CALLBACK preview_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    int active = g_raw_line >= 0; /* a block is open as Markdown source */
    if (m == WM_MOUSEWHEEL && wheel_scroll(h, w)) return 0;

    if (m == WM_LBUTTONUP) {
        LRESULT r = DefSubclassProc(h, m, w, l);
        if (g_link_clicked) {
            g_link_clicked = 0;
            return r;
        }
        CHARRANGE cr = get_sel(h);
        if (cr.cpMin != cr.cpMax) return r; /* a selection was made */
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (active) {
            LONG rs, re;
            if (!raw_bounds(&rs, &re) || cr.cpMin < rs || cr.cpMin > re) raw_leave_at(pt);
            return r;
        }
        if (g_welcome_shown || g_mcount == 0) {
            /* nothing to edit yet: an empty page where typing starts the document */
            g_welcome_off = 1;
            render_preview();
            CHARRANGE z = get_sel(h);
            g_pend = -2;
            g_pend_cp = z.cpMin;
            return r;
        }
        int k = marker_for_cp(cr.cpMin);
        if (k < 0) return r;
        LONG a, b;
        if (g_blk[k].kind == BK_ITEM && g_blk[k].task && g_blk[k].editable && blk_range(k, &a, &b) &&
            cr.cpMin < a && cr.cpMin >= g_mcp[k]) {
            toggle_task(k); /* a click on the check box */
            return r;
        }
        if (!g_blk[k].editable && g_blk[k].kind != BK_NONE) raw_open(g_mline[k], 2, pt);
        return r;
    }

    if (active) {
        /* a block shown as Markdown source: edits stay inside it */
        switch (m) {
        case WM_KEYDOWN: {
            int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
            switch (w) {
            case VK_ESCAPE: {
                POINT none = {0, 0};
                raw_open(-1, 0, none);
                return 0;
            }
            case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
            case VK_PRIOR: case VK_NEXT: case VK_HOME: case VK_END: {
                LONG before = get_sel(h).cpMax;
                LRESULT r = DefSubclassProc(h, m, w, l);
                if (!shift) raw_follow_caret(w, before);
                return r;
            }
            case VK_RETURN:
                if (ctrl || alt) return 0;
                if (!raw_edit_allowed(0)) return 0;
                if (shift || !raw_source_cmd(IDM_RAW_ENTER)) SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"\r");
                raw_refocus();
                return 0;
            case VK_BACK:
                if (!raw_edit_allowed(VK_BACK)) return 0;
                break;
            case VK_DELETE:
                if (!raw_edit_allowed(shift ? 0 : VK_DELETE)) return 0;
                break;
            case VK_INSERT:
                if (shift) {
                    SendMessageW(h, WM_PASTE, 0, 0);
                    return 0;
                }
                break;
            default:
                if (!ctrl || alt) break;
                if (w == 'A') {
                    LONG rs, re;
                    if (raw_bounds(&rs, &re)) set_sel(h, rs, re);
                    return 0;
                }
                if (w == 'V') {
                    SendMessageW(h, WM_PASTE, 0, 0);
                    return 0;
                }
                if (w == 'X' && !raw_edit_allowed(0)) return 0;
                if (w == 'Z' && !SendMessageW(h, EM_CANUNDO, 0, 0)) {
                    raw_source_cmd(IDM_UNDO);
                    return 0;
                }
                if (w == 'Y' && !SendMessageW(h, EM_CANREDO, 0, 0)) {
                    raw_source_cmd(IDM_REDO);
                    return 0;
                }
                if (w == 'L' || w == 'R' || w == 'J' || w == '5' || w == VK_OEM_PERIOD || w == VK_OEM_COMMA) return 0;
                break;
            }
            break;
        }
        case WM_CHAR:
            if (w == '\r' || w == '\n') return 0; /* handled on WM_KEYDOWN */
            if ((w >= 0x20 || w == '\t') && !raw_edit_allowed(0)) return 0;
            break;
        case WM_PASTE:
            if (raw_edit_allowed(0)) SendMessageW(h, EM_PASTESPECIAL, CF_UNICODETEXT, 0);
            return 0;
        case WM_CUT:
        case WM_CLEAR:
            if (!raw_edit_allowed(0)) {
                if (m == WM_CUT) SendMessageW(h, WM_COPY, 0, 0);
                return 0;
            }
            break;
        case WM_IME_STARTCOMPOSITION:
        case WM_IME_COMPOSITION:
        case WM_IME_CHAR:
            if (!raw_edit_allowed(0)) return 0;
            break;
        }
        return DefSubclassProc(h, m, w, l);
    }

    /* the formatted document itself is edited */
    switch (m) {
    case WM_KEYDOWN: {
        int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        g_eat_char = 0;
        switch (w) {
        case VK_RETURN:
            g_eat_char = 1;
            if (!ctrl && !alt) wys_enter();
            return 0;
        case VK_BACK:
        case VK_DELETE: {
            if (shift && w == VK_DELETE) { /* Shift+Delete cuts */
                SendMessageW(h, WM_CUT, 0, 0);
                g_eat_char = 1;
                return 0;
            }
            if (wys_delete(w)) {
                g_eat_char = 1;
                return 0;
            }
            LONG sa, sb;
            int k2, k = edit_target(&sa, &sb, &k2);
            return native_edit(h, m, w, l, k);
        }
        case VK_TAB:
            g_eat_char = 1;
            if (!ctrl && !alt) wys_tab(shift);
            return 0;
        case VK_INSERT:
            if (shift) {
                wys_paste();
                return 0;
            }
            break;
        default:
            if (!ctrl || alt) break;
            if (w == 'V') {
                wys_paste();
                return 0;
            }
            if (w == 'X') {
                SendMessageW(h, WM_CUT, 0, 0);
                return 0;
            }
            if (w == 'Z' || w == 'Y') {
                wys_command(w == 'Z' ? IDM_UNDO : IDM_REDO);
                return 0;
            }
            /* RichEdit's own alignment, spacing and font size shortcuts */
            if (w == 'L' || w == 'R' || w == 'J' || w == '5' || w == VK_OEM_PERIOD || w == VK_OEM_COMMA) return 0;
            break;
        }
        break;
    }
    case WM_CHAR: {
        if (g_eat_char) {
            g_eat_char = 0;
            if (w == '\r' || w == '\n' || w == '\t' || w == 8 || w == 0x7F) return 0;
        }
        if (w == 8 || w == 0x7F) {
            LONG sa, sb;
            int k2, k = edit_target(&sa, &sb, &k2);
            if (k < 0 || k2 != k) return 0;
            return native_edit(h, m, w, l, k);
        }
        if (w < 0x20) return 0;
        wys_char(h, m, w, l);
        return 0;
    }
    case WM_PASTE: wys_paste(); return 0;
    case WM_CUT:
    case WM_CLEAR: {
        CHARRANGE cr = get_sel(h);
        if (cr.cpMin == cr.cpMax) return 0;
        if (m == WM_CUT) SendMessageW(h, WM_COPY, 0, 0);
        if (!wys_delete(VK_DELETE)) {
            LONG sa, sb;
            int k2, k = edit_target(&sa, &sb, &k2);
            native_edit(h, WM_CLEAR, 0, 0, k);
        }
        return 0;
    }
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
    case WM_IME_CHAR: {
        LONG sa, sb;
        int k2, k = edit_target(&sa, &sb, &k2);
        if (k < 0 || k2 != k) return 0;
        if (m == WM_IME_STARTCOMPOSITION) break;
        return native_edit(h, m, w, l, k);
    }
    }
    return DefSubclassProc(h, m, w, l);
}

/* ------------------------------------------------------------------------ */
/* Check for updates: asks GitHub for the latest release when the user     */
/* picks the menu item (never on its own). Nothing is downloaded or run;   */
/* a newer version only offers to open its release page in the browser.   */
/* ------------------------------------------------------------------------ */

typedef struct update_result {
    int status;      /* HTTP status, 0 when GitHub could not be reached */
    char tag[64];    /* "v1.2.3" */
    char url[512];   /* release page */
} update_result;

static volatile LONG g_update_busy;

/* Finds "key": "value" in a JSON text and copies the value (no escapes are
 * expected in tags or GitHub URLs; a value with a backslash is rejected). */
static int json_string(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return 0;
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\' || n + 1 >= cap) return 0;
        out[n++] = *p++;
    }
    out[n] = 0;
    return *p == '"';
}

static DWORD WINAPI update_thread(LPVOID arg)
{
    (void)arg;
    update_result *r = (update_result *)calloc(1, sizeof *r);
    if (!r) {
        InterlockedExchange(&g_update_busy, 0);
        return 0;
    }
    HINTERNET s = WinHttpOpen(L"Domdly/" DOMDLY_VERSION_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET c = s ? WinHttpConnect(s, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0) : NULL;
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", L"/repos/dogukansahil/Domdly/releases/latest", NULL,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                    : NULL;
    if (s) WinHttpSetTimeouts(s, 10000, 10000, 10000, 15000);
    if (q && WinHttpSendRequest(q, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0,
                                0, 0) &&
        WinHttpReceiveResponse(q, NULL)) {
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        r->status = (int)status;
        if (status == 200) {
            size_t cap = 1024 * 1024, len = 0;
            char *body = (char *)malloc(cap + 1);
            DWORD got = 0;
            while (body && len < cap && WinHttpReadData(q, body + len, (DWORD)(cap - len), &got) && got > 0)
                len += got;
            if (body) {
                body[len] = 0;
                if (!json_string(body, "tag_name", r->tag, sizeof r->tag)) r->status = 0;
                json_string(body, "html_url", r->url, sizeof r->url);
                free(body);
            }
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    if (!PostMessageW(g_main, WM_APP_UPDATE, 0, (LPARAM)r)) {
        free(r);
        InterlockedExchange(&g_update_busy, 0);
    }
    return 0;
}

static void cmd_check_updates(void)
{
    if (InterlockedCompareExchange(&g_update_busy, 1, 0) != 0) return; /* already asking */
    swprintf(g_status, 256, L"Checking GitHub for a newer Domdly...");
    InvalidateRect(g_main, &g_rc_bottom, FALSE);
    HANDLE t = CreateThread(NULL, 0, update_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else InterlockedExchange(&g_update_busy, 0);
}

static void parse_version(const char *s, int v[3])
{
    v[0] = v[1] = v[2] = 0;
    if (*s == 'v' || *s == 'V') s++;
    sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]);
}

static void open_releases_page(const char *url)
{
    static const char prefix[] = "https://github.com/dogukansahil/Domdly/";
    if (url && strncmp(url, prefix, sizeof prefix - 1) == 0) {
        WCHAR *w = util_utf8_to_wide(url, -1);
        if (w) {
            ShellExecuteW(g_main, L"open", w, NULL, NULL, SW_SHOWNORMAL);
            free(w);
            return;
        }
    }
    ShellExecuteW(g_main, L"open", DOMDLY_URL_WSTR L"/releases", NULL, NULL, SW_SHOWNORMAL);
}

static void on_update_result(update_result *r)
{
    InterlockedExchange(&g_update_busy, 0);
    update_status();
    if (!r) return;
    if (r->status == 200) {
        int have[3] = {DOMDLY_VERSION_MAJOR, DOMDLY_VERSION_MINOR, DOMDLY_VERSION_PATCH}, got[3];
        parse_version(r->tag, got);
        int newer = got[0] != have[0] ? got[0] > have[0] : got[1] != have[1] ? got[1] > have[1] : got[2] > have[2];
        if (newer) {
            WCHAR msg[256];
            swprintf(msg, 256, L"Domdly %d.%d.%d is available. You have %s.\n\nOpen the download page?", got[0], got[1],
                     got[2], DOMDLY_VERSION_WSTR);
            if (MessageBoxW(g_main, msg, L"Domdly update", MB_ICONINFORMATION | MB_YESNO) == IDYES)
                open_releases_page(r->url);
        } else {
            MessageBoxW(g_main, L"You have the latest version (" DOMDLY_VERSION_WSTR L").", L"Domdly update",
                        MB_ICONINFORMATION | MB_OK);
        }
    } else if (r->status == 404) {
        MessageBoxW(g_main, L"No release has been published on GitHub yet.", L"Domdly update",
                    MB_ICONINFORMATION | MB_OK);
    } else if (MessageBoxW(g_main, L"Could not reach GitHub.\n\nOpen the releases page in your browser?",
                           L"Domdly update", MB_ICONWARNING | MB_YESNO) == IDYES) {
        open_releases_page(NULL);
    }
    free(r);
}

/* ------------------------------------------------------------------------ */
/* Files sidebar: a tree of the folder that holds the open document (or a   */
/* folder the user pinned). Folders load when they are first expanded.      */
/* Markdown files open in a tab with one click; other files open with their */
/* usual program on a double click or Enter.                                */
/* ------------------------------------------------------------------------ */

#define TREE_MAX_ENTRIES 5000

typedef struct tree_node {
    int dir, md, loaded;
    WCHAR path[1]; /* full path, allocated to fit */
} tree_node;

typedef struct tree_entry {
    int dir;
    WCHAR *name;
} tree_entry;

static WCHAR g_tree_shown[DOMDLY_PATH_MAX]; /* document last revealed in the tree */

static void path_join(WCHAR *out, const WCHAR *dir, const WCHAR *name)
{
    size_t n = wcslen(dir);
    int slash = n > 0 && dir[n - 1] == L'\\';
    swprintf(out, DOMDLY_PATH_MAX, slash ? L"%s%s" : L"%s\\%s", dir, name);
}

static void path_parent(WCHAR *out, const WCHAR *path)
{
    wcsncpy(out, path, DOMDLY_PATH_MAX - 1);
    out[DOMDLY_PATH_MAX - 1] = 0;
    WCHAR *slash = wcsrchr(out, L'\\');
    if (!slash) {
        out[0] = 0;
        return;
    }
    *slash = 0;
    if (wcslen(out) == 2 && out[1] == L':') wcscat(out, L"\\"); /* "C:" -> "C:\" */
}

static int is_folder(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int tree_entry_cmp(const void *pa, const void *pb)
{
    const tree_entry *a = (const tree_entry *)pa, *b = (const tree_entry *)pb;
    if (a->dir != b->dir) return b->dir - a->dir; /* folders first */
    return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a->name, -1, b->name,
                           -1, NULL, NULL, 0) -
           2;
}

static tree_node *tree_node_of(HTREEITEM h)
{
    TVITEMW it;
    memset(&it, 0, sizeof it);
    it.mask = TVIF_PARAM;
    it.hItem = h;
    if (!h || !SendMessageW(g_tree, TVM_GETITEMW, 0, (LPARAM)&it)) return NULL;
    return (tree_node *)it.lParam;
}

/* Lists one folder under parent: everything except hidden and dot names. */
static int tree_fill(HTREEITEM parent, const WCHAR *dir)
{
    WCHAR pattern[DOMDLY_PATH_MAX];
    path_join(pattern, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return 0;
    tree_entry *list = NULL;
    int n = 0, cap = 0;
    do {
        if (fd.cFileName[0] == L'.') continue; /* ".", "..", ".git" and friends */
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        if (n == cap) {
            int nc = cap ? cap * 2 : 64;
            tree_entry *g = (tree_entry *)realloc(list, (size_t)nc * sizeof *g);
            if (!g) break;
            list = g;
            cap = nc;
        }
        list[n].dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        list[n].name = _wcsdup(fd.cFileName);
        if (list[n].name) n++;
    } while (n < TREE_MAX_ENTRIES && FindNextFileW(f, &fd));
    FindClose(f);
    if (n > 1) qsort(list, (size_t)n, sizeof *list, tree_entry_cmp);
    for (int i = 0; i < n; i++) {
        WCHAR full[DOMDLY_PATH_MAX];
        path_join(full, dir, list[i].name);
        size_t len = wcslen(full);
        tree_node *node = (tree_node *)malloc(sizeof *node + len * sizeof(WCHAR));
        if (node) {
            node->dir = list[i].dir;
            node->md = !node->dir && is_markdown_path(full);
            node->loaded = 0;
            wcscpy(node->path, full);
            TVINSERTSTRUCTW ins;
            memset(&ins, 0, sizeof ins);
            ins.hParent = parent;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
            ins.item.pszText = list[i].name;
            ins.item.cChildren = node->dir ? 1 : 0; /* filled when expanded */
            ins.item.lParam = (LPARAM)node;
            if (!SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&ins)) free(node);
        }
        free(list[i].name);
    }
    free(list);
    return n;
}

static HTREEITEM tree_first_child(HTREEITEM parent)
{
    return (HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, parent == TVI_ROOT ? TVGN_ROOT : TVGN_CHILD,
                                   parent == TVI_ROOT ? 0 : (LPARAM)parent);
}

static HTREEITEM tree_next(HTREEITEM h)
{
    return (HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_NEXT, (LPARAM)h);
}

static int tree_expanded(HTREEITEM h)
{
    return (SendMessageW(g_tree, TVM_GETITEMSTATE, (WPARAM)h, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
}

/* The item for a path below the root. With expand, folders on the way are
 * opened; without it, NULL is returned when one of them is closed. */
static HTREEITEM tree_find(const WCHAR *path, int expand)
{
    size_t rl = wcslen(g_tree_root);
    if (!rl || wcslen(path) <= rl || _wcsnicmp(path, g_tree_root, rl) != 0) return NULL;
    const WCHAR *rest = path + rl;
    if (g_tree_root[rl - 1] != L'\\') {
        if (*rest != L'\\') return NULL;
        rest++;
    }
    HTREEITEM parent = TVI_ROOT, found = NULL;
    while (*rest) {
        const WCHAR *end = wcschr(rest, L'\\');
        size_t seg = end ? (size_t)(end - rest) : wcslen(rest);
        size_t upto = (size_t)(rest - path) + seg;
        found = NULL;
        for (HTREEITEM c = tree_first_child(parent); c; c = tree_next(c)) {
            tree_node *node = tree_node_of(c);
            if (node && wcslen(node->path) == upto && _wcsnicmp(node->path, path, upto) == 0) {
                found = c;
                break;
            }
        }
        if (!found || !end) break;
        if (!tree_expanded(found)) {
            if (!expand) return NULL;
            SendMessageW(g_tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)found);
        }
        parent = found;
        rest = end + 1;
    }
    return found;
}

static void tree_select(HTREEITEM h)
{
    g_tree_quiet = 1;
    SendMessageW(g_tree, TVM_SELECTITEM, TVGN_CARET, (LPARAM)h);
    if (h) SendMessageW(g_tree, TVM_ENSUREVISIBLE, 0, (LPARAM)h);
    g_tree_quiet = 0;
}

typedef struct strlist {
    WCHAR **v;
    int n, cap;
} strlist;

static void strlist_add(strlist *l, const WCHAR *s)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 16;
        WCHAR **g = (WCHAR **)realloc(l->v, (size_t)nc * sizeof *g);
        if (!g) return;
        l->v = g;
        l->cap = nc;
    }
    if ((l->v[l->n] = _wcsdup(s)) != NULL) l->n++;
}

static void collect_expanded(HTREEITEM parent, strlist *out)
{
    for (HTREEITEM c = tree_first_child(parent); c; c = tree_next(c)) {
        if (!tree_expanded(c)) continue;
        tree_node *node = tree_node_of(c);
        if (node) strlist_add(out, node->path);
        collect_expanded(c, out);
    }
}

/* Lists the root again. keep: folders that were open stay open and the
 * scroll position is kept (used for refreshes of the same folder). */
static void tree_load(int keep)
{
    strlist open = {NULL, 0, 0};
    WCHAR top[DOMDLY_PATH_MAX] = L"";
    if (keep) {
        collect_expanded(TVI_ROOT, &open);
        tree_node *t = tree_node_of((HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_FIRSTVISIBLE, 0));
        if (t) wcscpy(top, t->path);
    }
    int visible = IsWindowVisible(g_tree);
    if (visible) SendMessageW(g_tree, WM_SETREDRAW, FALSE, 0);
    g_tree_quiet = 1;
    SendMessageW(g_tree, TVM_DELETEITEM, 0, (LPARAM)TVI_ROOT);
    g_tree_quiet = 0;
    if (g_tree_root[0]) tree_fill(TVI_ROOT, g_tree_root);
    for (int i = 0; i < open.n; i++) {
        HTREEITEM h = tree_find(open.v[i], 0);
        if (h) SendMessageW(g_tree, TVM_EXPAND, TVE_EXPAND, (LPARAM)h);
        free(open.v[i]);
    }
    free(open.v);
    if (top[0]) {
        HTREEITEM h = tree_find(top, 0);
        if (h) SendMessageW(g_tree, TVM_SELECTITEM, TVGN_FIRSTVISIBLE, (LPARAM)h);
    }
    if (visible) {
        SendMessageW(g_tree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_tree, NULL, TRUE);
    }
}

/* Brings the sidebar in line with the active document: picks the root,
 * lists it when it changed (or always, with refresh) and selects the
 * document. A newly opened document is revealed by expanding its folders. */
static void tree_sync(int refresh)
{
    if (!g_tree || !S.tree_open) return;
    WCHAR want[DOMDLY_PATH_MAX];
    if (S.tree_root[0] && is_folder(S.tree_root)) wcscpy(want, S.tree_root);
    else if (g_has_path) path_parent(want, g_path);
    else wcscpy(want, g_tree_root); /* an untitled tab keeps the folder */
    int changed = _wcsicmp(want, g_tree_root) != 0;
    if (changed) {
        wcscpy(g_tree_root, want);
        g_tree_shown[0] = 0;
        tree_load(0);
        layout(); /* shows or hides the tree and the hint */
    } else if (refresh) {
        tree_load(1);
    }
    const WCHAR *doc = g_has_path ? g_path : L"";
    int fresh = _wcsicmp(doc, g_tree_shown) != 0;
    HTREEITEM h = *doc ? tree_find(doc, fresh) : NULL;
    if (fresh || h) tree_select(h);
    wcscpy(g_tree_shown, doc);
    InvalidateRect(g_main, &g_rc_side, FALSE);
}

static void tree_toggle(void)
{
    S.tree_open = !S.tree_open;
    settings_save(&S);
    HWND focus = GetFocus();
    layout();
    if (S.tree_open) {
        tree_sync(1);
    } else if (focus == g_tree) {
        SetFocus(is_read() ? g_prev : g_edit);
    }
    invalidate_chrome();
}

static void tree_pick_folder(void)
{
    IFileOpenDialog *d = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return;
    DWORD opt = 0;
    d->lpVtbl->GetOptions(d, &opt);
    d->lpVtbl->SetOptions(d, opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->lpVtbl->SetTitle(d, L"Folder for the files sidebar");
    if (g_tree_root[0]) {
        IShellItem *start = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_tree_root, NULL, &IID_IShellItem, (void **)&start))) {
            d->lpVtbl->SetFolder(d, start);
            start->lpVtbl->Release(start);
        }
    }
    IShellItem *item = NULL;
    if (SUCCEEDED(d->lpVtbl->Show(d, g_main)) && SUCCEEDED(d->lpVtbl->GetResult(d, &item))) {
        WCHAR *p = NULL;
        if (SUCCEEDED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &p)) && p) {
            if (wcslen(p) < DOMDLY_PATH_MAX) {
                wcscpy(S.tree_root, p);
                S.tree_open = 1;
                settings_save(&S);
                layout();
                tree_sync(1);
                invalidate_chrome();
            }
            CoTaskMemFree(p);
        }
        item->lpVtbl->Release(item);
    }
    d->lpVtbl->Release(d);
}

static void tree_head_menu(void)
{
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, IDM_TREE_FOLDER, L"&Open Folder...");
    AppendMenuW(m, MF_STRING | (S.tree_root[0] ? 0 : MF_CHECKED), IDM_TREE_FOLLOW, L"&Follow Open File");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_tree_root[0] ? 0 : MF_GRAYED), IDM_TREE_REFRESH, L"&Refresh\tF5");
    AppendMenuW(m, MF_STRING | (g_tree_root[0] ? 0 : MF_GRAYED), IDM_TREE_REVEAL, L"Show in File &Explorer");
    set_menu_theme(g_dark);
    POINT pt = {g_rc_sidehead.left + S_(6), g_rc_sidehead.bottom - S_(2)};
    ClientToScreen(g_main, &pt);
    int cmd = (int)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, g_main, NULL);
    DestroyMenu(m);
    if (cmd) on_command(cmd);
}

/* Opens what an item stands for. Markdown opens in Domdly (posted, so the
 * tree is not changed while it is still handling the click). */
static void tree_open_item(HTREEITEM h, int allow_other)
{
    tree_node *node = tree_node_of(h);
    if (!node) return;
    if (node->dir) {
        SendMessageW(g_tree, TVM_EXPAND, TVE_TOGGLE, (LPARAM)h);
    } else if (node->md) {
        WCHAR *p = _wcsdup(node->path);
        if (p) PostMessageW(g_main, WM_APP_TREE, 0, (LPARAM)p);
    } else if (allow_other) {
        ShellExecuteW(g_main, L"open", node->path, NULL, NULL, SW_SHOWNORMAL);
    }
}

static HTREEITEM tree_item_at_cursor(UINT *flags)
{
    TVHITTESTINFO hi;
    memset(&hi, 0, sizeof hi);
    GetCursorPos(&hi.pt);
    ScreenToClient(g_tree, &hi.pt);
    HTREEITEM h = (HTREEITEM)SendMessageW(g_tree, TVM_HITTEST, 0, (LPARAM)&hi);
    if (flags) *flags = hi.flags;
    return h;
}

static LRESULT tree_notify(NMHDR *nh)
{
    switch (nh->code) {
    case TVN_DELETEITEMW: {
        NMTREEVIEWW *nm = (NMTREEVIEWW *)nh;
        free((void *)nm->itemOld.lParam);
        return 0;
    }
    case TVN_ITEMEXPANDINGW: {
        NMTREEVIEWW *nm = (NMTREEVIEWW *)nh;
        tree_node *node = (tree_node *)nm->itemNew.lParam;
        if ((nm->action & TVE_EXPAND) && node && node->dir && !node->loaded) {
            node->loaded = 1;
            if (!tree_fill(nm->itemNew.hItem, node->path)) {
                TVITEMW it;
                memset(&it, 0, sizeof it);
                it.mask = TVIF_CHILDREN;
                it.hItem = nm->itemNew.hItem;
                it.cChildren = 0; /* empty: drop the expander */
                SendMessageW(g_tree, TVM_SETITEMW, 0, (LPARAM)&it);
            }
        }
        return 0;
    }
    case NM_CLICK: {
        UINT flags = 0;
        HTREEITEM h = tree_item_at_cursor(&flags);
        if (!h || (flags & TVHT_ONITEMBUTTON)) return 0;
        if (!(flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT))) return 0;
        tree_select(h);
        tree_open_item(h, 0); /* one click: folders toggle, Markdown opens */
        return 1;
    }
    case NM_DBLCLK: {
        UINT flags = 0;
        HTREEITEM h = tree_item_at_cursor(&flags);
        tree_node *node = tree_node_of(h);
        if (!node || (flags & TVHT_ONITEMBUTTON)) return 0;
        if (!node->dir && !node->md) tree_open_item(h, 1);
        return 1; /* the first click already toggled a folder */
    }
    case TVN_KEYDOWN: {
        NMTVKEYDOWN *kd = (NMTVKEYDOWN *)nh;
        if (kd->wVKey == VK_RETURN) {
            tree_open_item((HTREEITEM)SendMessageW(g_tree, TVM_GETNEXTITEM, TVGN_CARET, 0), 1);
            return 1;
        }
        if (kd->wVKey == VK_F5) {
            tree_sync(1);
            return 1;
        }
        return 0;
    }
    case NM_CUSTOMDRAW: {
        NMTVCUSTOMDRAW *cd = (NMTVCUSTOMDRAW *)nh;
        if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
        if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            tree_node *node = (tree_node *)cd->nmcd.lItemlParam;
            if (!node) return CDRF_DODEFAULT;
            cd->clrText = node->dir || node->md ? P.bar_fg : P.bar_muted;
            if (g_has_path && _wcsicmp(node->path, g_path) == 0) {
                SelectObject(cd->nmcd.hdc, g_font_bold); /* the active document */
                return CDRF_NEWFONT;
            }
        }
        return CDRF_DODEFAULT;
    }
    }
    return 0;
}

/* Swallows the Enter character so the tree does not beep after opening. */
static LRESULT CALLBACK tree_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
    (void)id;
    (void)ref;
    if (m == WM_CHAR && (w == L'\r' || w == L'\n')) return 0;
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, m, w, l);
}

static void tree_apply_theme(void)
{
    if (!g_tree) return;
    SetWindowTheme(g_tree, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);
    SendMessageW(g_tree, TVM_SETBKCOLOR, 0, (LPARAM)P.bar_bg);
    SendMessageW(g_tree, TVM_SETTEXTCOLOR, 0, (LPARAM)P.bar_fg);
    SendMessageW(g_tree, TVM_SETLINECOLOR, 0, (LPARAM)P.bar_border);
    InvalidateRect(g_tree, NULL, TRUE);
}

static void tree_create(HWND parent)
{
    g_tree = CreateWindowExW(0, WC_TREEVIEWW, L"",
                             WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | TVS_HASBUTTONS | TVS_LINESATROOT |
                                 TVS_FULLROWSELECT | TVS_TRACKSELECT | TVS_NOHSCROLL,
                             0, 0, 10, 10, parent, NULL, g_inst, NULL);
    if (!g_tree) return;
    SendMessageW(g_tree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
    SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_font_ui, FALSE);
    SendMessageW(g_tree, TVM_SETITEMHEIGHT, (WPARAM)S_(26), 0);
    SendMessageW(g_tree, TVM_SETINDENT, (WPARAM)S_(14), 0);
    SetWindowSubclass(g_tree, tree_proc, 1, 0);
    SendMessageW(g_tree, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0); /* no dotted focus box */
}

static HWND create_richedit(HWND parent, DWORD style)
{
    HWND h = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
                             WS_CHILD | WS_VSCROLL | WS_CLIPSIBLINGS | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOOLEDRAGDROP |
                                 style,
                             0, 0, 10, 10, parent, NULL, g_inst, NULL);
    if (!h) return NULL;
    SendMessageW(h, EM_SETTEXTMODE, style & ES_READONLY ? TM_RICHTEXT : (TM_PLAINTEXT | TM_MULTILEVELUNDO), 0);
    SendMessageW(h, EM_EXLIMITTEXT, 0, 0x7FFFFFFE);
    LRESULT lang = SendMessageW(h, EM_GETLANGOPTIONS, 0, 0);
    lang &= ~(IMF_AUTOFONT | IMF_DUALFONT | IMF_AUTOFONTSIZEADJUST);
    SendMessageW(h, EM_SETLANGOPTIONS, 0, lang);
    return h;
}

static void create_children(HWND hwnd)
{
    g_prev = create_richedit(hwnd, ES_READONLY);
    g_edit = create_editor(&g_tom);
    g_ntabs = 1;
    g_cur = 0;
    g_tabs[0].edit = g_edit;
    g_tabs[0].tom = g_tom;
    g_tabs[0].enc = ENC_UTF8;
    SetWindowLongPtrW(g_edit, GWLP_ID, 1);
    SetWindowLongPtrW(g_prev, GWLP_ID, 2);

    SendMessageW(g_prev, EM_SETEVENTMASK, 0, ENM_LINK | ENM_CHANGE | ENM_SELCHANGE);
    SendMessageW(g_prev, EM_AUTOURLDETECT, 0, 0);
    SendMessageW(g_prev, EM_SETUNDOLIMIT, 200, 0);
    olecb_attach(g_prev); /* needed for pictures */
    SetWindowSubclass(g_prev, preview_proc, 1, 0);

    tree_create(hwnd);
    g_drop = CreateWindowExW(0, DROP_CLASS, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, hwnd, NULL, g_inst, NULL);

    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                            hwnd, NULL, g_inst, NULL);
}

/* ------------------------------------------------------------------------ */
/* Main window                                                              */
/* ------------------------------------------------------------------------ */

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        g_dpi = (int)GetDpiForWindow(hwnd);
        if (g_dpi <= 0) g_dpi = 96;
        create_fonts();
        create_children(hwnd);
        g_menubar = LoadMenuW(g_inst, MAKEINTRESOURCEW(IDR_MENU));
        {
            HMENU file = GetSubMenu(g_menubar, 0);
            g_recent_menu = file ? GetSubMenu(file, 2) : NULL;
        }
        {
            /* The header replaces the caption text and icon (Windows 11 tints
             * the caption to match it). */
            WTA_OPTIONS o = {WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON, WTNCA_NODRAWCAPTION | WTNCA_NODRAWICON};
            SetWindowThemeAttribute(hwnd, WTA_NONCLIENT, &o, sizeof o);
        }
        set_word_wrap(S.word_wrap);
        SendMessageW(g_edit, EM_SETZOOM, (WPARAM)S.zoom, 100);
        apply_theme();
        layout();
        update_title();
        register_drop_target();
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S_(480);
        mm->ptMinTrackSize.y = S_(320);
        return 0;
    }

    case WM_SETFOCUS:
        SetFocus(is_read() ? g_prev : g_edit);
        return 0;

    case WM_COMMAND:
        if ((HWND)lp == g_edit && HIWORD(wp) == EN_CHANGE) {
            update_title();
            if (g_syncing) return 0; /* the Read view already shows it */
            g_preview_dirty = 1;
            schedule_render();
            return 0;
        }
        if ((HWND)lp == g_prev && HIWORD(wp) == EN_CHANGE) {
            sync_raw(1);
            return 0;
        }
        if (lp == 0 || HIWORD(wp) == 0 || HIWORD(wp) == 1) on_command(LOWORD(wp));
        return 0;

    case WM_NOTIFY: {
        NMHDR *nh = (NMHDR *)lp;
        if (g_tree && nh->hwndFrom == g_tree) return tree_notify(nh);
        if (nh->hwndFrom == g_edit && nh->code == EN_SELCHANGE) {
            update_status();
        } else if (nh->hwndFrom == g_prev && nh->code == EN_SELCHANGE) {
            wys_selchange();
        } else if (nh->hwndFrom == g_prev && nh->code == EN_LINK) {
            /* the document is editable, so links open with Ctrl+click */
            ENLINK *el = (ENLINK *)lp;
            int ctrl = GetKeyState(VK_CONTROL) < 0;
            if (el->msg == WM_LBUTTONUP && ctrl) {
                on_link_click(el);
                return 1;
            }
            if (el->msg == WM_SETCURSOR && ctrl) {
                SetCursor(LoadCursorW(NULL, IDC_HAND));
                return 1;
            }
        }
        return 0;
    }

    case WM_TIMER:
        KillTimer(hwnd, wp);
        if (wp == TIMER_SYNC) {
            wys_flush();
        } else if (wp == TIMER_WORDS) {
            update_word_count();
            update_status();
        } else if (is_read()) {
            render_preview();
        } else {
            update_word_count();
            update_status();
        }
        return 0;

    case WM_MOUSEMOVE: {
        if (g_tree_drag) {
            int dip = MulDiv(GET_X_LPARAM(lp), 96, g_dpi);
            if (dip < 160) dip = 160;
            if (dip != S.tree_width) {
                S.tree_width = dip;
                layout();
            }
            return 0;
        }
        int hot = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hot != g_hot) {
            g_hot = hot;
            invalidate_chrome();
        }
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, hwnd, 0};
            g_tracking = TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        g_tracking = 0;
        if (g_hot) {
            g_hot = HOT_NONE;
            invalidate_chrome();
        }
        return 0;

    case WM_SETCURSOR:
        if ((HWND)wp == hwnd && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            int hot = hit_test(pt.x, pt.y);
            SetCursor(LoadCursorW(NULL, g_tree_drag || hot == HOT_SPLIT ? IDC_SIZEWE : hot ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        int hot = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hot == HOT_MENU) {
            show_main_menu();
            return 0;
        }
        if (hot == HOT_SIDEHEAD) {
            tree_head_menu();
            return 0;
        }
        if (hot == HOT_SPLIT) {
            g_tree_drag = 1;
            SetCapture(hwnd);
            InvalidateRect(hwnd, &g_rc_side, FALSE);
            return 0;
        }
        if (hot >= HOT_TAB && hot < HOT_TAB + g_ntabs) {
            tab_activate(hot - HOT_TAB); /* like a browser: switch on press */
            return 0;
        }
        g_pressed = hot;
        if (hot) {
            SetCapture(hwnd);
            invalidate_chrome();
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_tree_drag) {
            g_tree_drag = 0;
            if (GetCapture() == hwnd) ReleaseCapture();
            settings_save(&S);
            InvalidateRect(hwnd, &g_rc_side, FALSE);
            return 0;
        }
        int pressed = g_pressed;
        g_pressed = HOT_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (pressed && pressed == hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
            switch (pressed) {
            case HOT_ZOOM: set_zoom(100); break;
            case HOT_EDIT: ensure_edit(); break;
            case HOT_READ: switch_to_read(); break;
            case HOT_THEME: on_command(IDM_THEME_TOGGLE); break;
            case HOT_SAVE: on_command(IDM_SAVEAS); break;
            case HOT_NEWTAB: on_command(IDM_NEW); break;
            case HOT_SIDEBTN: on_command(IDM_TOGGLE_TREE); break;
            case HOT_VER: on_command(IDM_WEBSITE); break;
            case HOT_PATH: show_in_folder(); break;
            default:
                if (pressed >= HOT_TABX && pressed < HOT_TABX + g_ntabs) tab_close(pressed - HOT_TABX);
                if (pressed >= HOT_FMT && pressed < HOT_FMT + FMT_COUNT) {
                    /* the command works on the selection in the view being
                       edited, which keeps the keyboard focus */
                    on_command(g_fmt[pressed - HOT_FMT].id);
                    SetFocus(is_read() ? g_prev : g_edit);
                }
                break;
            }
        }
        invalidate_chrome();
        return 0;
    }

    case WM_MBUTTONUP: {
        int hot = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (hot >= HOT_TAB && hot < HOT_TAB + g_ntabs) tab_close(hot - HOT_TAB);
        else if (hot >= HOT_TABX && hot < HOT_TABX + g_ntabs) tab_close(hot - HOT_TABX);
        invalidate_chrome();
        return 0;
    }

    case WM_COPYDATA: {
        /* another Domdly process hands over a file to open in a new tab */
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        if (cd && cd->dwData == COPYDATA_OPEN && cd->cbData >= sizeof(WCHAR) && cd->cbData < DOMDLY_PATH_MAX * sizeof(WCHAR)) {
            WCHAR *path = (WCHAR *)calloc(cd->cbData / sizeof(WCHAR) + 1, sizeof(WCHAR));
            if (path) {
                memcpy(path, cd->lpData, cd->cbData);
                PostMessageW(hwnd, WM_APP_OPEN, 0, (LPARAM)path); /* open after the sender is released */
            }
        }
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC mdc = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mdc, bmp);
            fill_rect(mdc, &rc, P.doc_bg);
            paint_chrome(mdc);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mdc, 0, 0, SRCCOPY);
            SelectObject(mdc, old);
            DeleteObject(bmp);
            DeleteDC(mdc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SYSCOMMAND:
        /* Alt or F10 opens the Domdly menu (Alt+Space keeps the system menu). */
        if ((wp & 0xFFF0) == SC_KEYMENU && lp != ' ') {
            show_main_menu();
            return 0;
        }
        break;

    case WM_APP_UPDATE:
        on_update_result((update_result *)lp);
        return 0;

    case WM_APP_TREE: {
        WCHAR *path = (WCHAR *)lp;
        if (path) open_file(path);
        free(path);
        return 0;
    }

    case WM_ACTIVATEAPP:
        if (wp) tree_sync(1); /* files may have changed meanwhile */
        break;

    case WM_APP_IMAGES: {
        dropped_images *di = (dropped_images *)lp;
        if (di) {
            SetForegroundWindow(hwnd);
            insert_dropped_images(di);
            free(di->md);
            free(di);
        }
        return 0;
    }

    case WM_APP_OPEN: {
        WCHAR *path = (WCHAR *)lp;
        SetForegroundWindow(hwnd);
        if (path) open_file(path);
        free(path);
        return 0;
    }

    case WM_DROPFILES: { /* fallback when OLE drag and drop is unavailable */
        HDROP drop = (HDROP)wp;
        WCHAR path[DOMDLY_PATH_MAX];
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < n; i++)
            if (DragQueryFileW(drop, i, path, DOMDLY_PATH_MAX)) open_file(path);
        DragFinish(drop);
        return 0;
    }

    case WM_INITMENUPOPUP:
        if ((HMENU)wp == g_recent_menu) rebuild_recent_menu();
        update_menu();
        return 0;

    case WM_SETTINGCHANGE:
        if (lp && wcscmp((const WCHAR *)lp, L"ImmersiveColorSet") == 0 && S.theme == THEME_SYSTEM) apply_theme();
        break;

    case WM_CLOSE:
        if (!confirm_close_all()) return 0;
        save_session();
        S.placement.length = sizeof S.placement;
        if (GetWindowPlacement(hwnd, &S.placement)) S.has_placement = 1;
        settings_save(&S);
        RevokeDragDrop(hwnd);
        RevokeDragDrop(g_prev);
        tab_store();
        for (int i = 0; i < g_ntabs; i++) RevokeDragDrop(g_tabs[i].edit);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        tab_store();
        for (int i = 0; i < g_ntabs; i++)
            if (g_tabs[i].tom) g_tabs[i].tom->lpVtbl->Release(g_tabs[i].tom);
        g_tom = NULL;
        PostQuitMessage(0);
        return 0;

    default:
        if (msg == g_find_msg && g_find_msg) {
            on_find_message((FINDREPLACEW *)lp);
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

static const WCHAR *first_file_arg(int argc, WCHAR **argv)
{
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--pdf") == 0) {
            i++; /* skip the output file */
            continue;
        }
        if (argv[i][0] != '-' || argv[i][1] != '-') return argv[i];
    }
    return NULL;
}

/* The value after a flag such as --pdf, or NULL. */
static const WCHAR *flag_value(int argc, WCHAR **argv, const WCHAR *flag)
{
    for (int i = 1; i + 1 < argc; i++)
        if (_wcsicmp(argv[i], flag) == 0) return argv[i + 1];
    return NULL;
}

static int has_flag(int argc, WCHAR **argv, const WCHAR *flag)
{
    for (int i = 1; i < argc; i++)
        if (_wcsicmp(argv[i], flag) == 0) return 1;
    return 0;
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmdline, int show)
{
    (void)prev;
    (void)cmdline;
    g_inst = hinst;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);

    int argc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int exit_code = 0;
    if (argv && inst_handle_command_line(argc, argv, &exit_code)) {
        LocalFree(argv);
        return exit_code;
    }

    /* One window: a second start hands its files to the running Domdly, which
     * opens them in new tabs. --new-window keeps a separate window. */
    if (argv && !flag_value(argc, argv, L"--pdf") && !has_flag(argc, argv, L"--new-window")) {
        HWND other = FindWindowW(MAIN_CLASS, NULL);
        if (other) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            int sent = 0;
            for (int i = 1; i < argc; i++) {
                if (argv[i][0] == '-' && argv[i][1] == '-') continue;
                WCHAR full[DOMDLY_PATH_MAX];
                if (!GetFullPathNameW(argv[i], DOMDLY_PATH_MAX, full, NULL)) continue;
                COPYDATASTRUCT cd = {COPYDATA_OPEN, (DWORD)((wcslen(full) + 1) * sizeof(WCHAR)), full};
                SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, NULL);
                sent = 1;
            }
            if (!sent) {
                COPYDATASTRUCT cd = {0, 0, NULL}; /* just bring it to the front */
                SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, NULL);
            }
            LocalFree(argv);
            return 0;
        }
    }

    OleInitialize(NULL);
    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!LoadLibraryExW(L"msftedit.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        MessageBoxW(NULL, L"The RichEdit component (msftedit.dll) is missing.", L"Domdly", MB_ICONERROR | MB_OK);
        return 1;
    }

    settings_load(&S);
    choose_families();
    const WCHAR *file = argv ? first_file_arg(argc, argv) : NULL;
    WCHAR file_full[DOMDLY_PATH_MAX] = L"";
    if (file) GetFullPathNameW(file, DOMDLY_PATH_MAX, file_full, NULL);
    const WCHAR *pdf_out = argv ? flag_value(argc, argv, L"--pdf") : NULL;
    if (!pdf_out && !(argv && has_flag(argc, argv, L"--portable")) && inst_startup_offer(&S, file ? file_full : NULL)) {
        LocalFree(argv);
        OleUninitialize();
        return 0;
    }
    int restore = S.session_count > 0 && !pdf_out && !(argv && has_flag(argc, argv, L"--new-window"));
    if (!file && !restore) S.view = VIEW_EDITOR; /* a new, empty document starts in Edit */

    g_find_msg = RegisterWindowMessageW(FINDMSGSTRINGW);
    HICON icon_big = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                       GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON icon_small = (HICON)LoadImageW(hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = main_proc;
    wc.hInstance = hinst;
    wc.hIcon = icon_big;
    wc.hIconSm = icon_small;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = MAIN_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = text_wnd_proc;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = TEXT_CLASS;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = drop_proc;
    wc.hbrBackground = NULL;
    wc.hIcon = NULL;
    wc.hIconSm = NULL;
    wc.lpszClassName = DROP_CLASS;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, MAIN_CLASS, L"Domdly", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, 1200, 800, NULL, NULL, hinst, NULL);
    if (!hwnd) return 1;
    if (pdf_out) {
        /* Domdly.exe file.md --pdf out.pdf: convert without showing a window */
        WCHAR out_full[DOMDLY_PATH_MAX];
        GetFullPathNameW(pdf_out, DOMDLY_PATH_MAX, out_full, NULL);
        int ok = file && open_file(file_full) && export_pdf(out_full);
        DestroyWindow(hwnd);
        LocalFree(argv);
        OleUninitialize();
        return ok ? 0 : 1;
    }
    g_accel = LoadAcceleratorsW(hinst, MAKEINTRESOURCEW(IDR_ACCEL));

    if (S.has_placement) {
        WINDOWPLACEMENT wp = S.placement;
        if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_MINIMIZE) wp.showCmd = SW_SHOWNORMAL;
        if (show == SW_SHOWMINIMIZED || show == SW_SHOWMINNOACTIVE) wp.showCmd = (UINT)show;
        SetWindowPlacement(hwnd, &wp);
    } else {
        int dpi = (int)GetDpiForWindow(hwnd);
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        int w = util_scale(1100, dpi), h = util_scale(780, dpi);
        if (w > work.right - work.left) w = work.right - work.left;
        if (h > work.bottom - work.top) h = work.bottom - work.top;
        SetWindowPos(hwnd, NULL, work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                     w, h, SWP_NOZORDER);
        ShowWindow(hwnd, show);
    }
    UpdateWindow(hwnd);

    int active = -1;
    if (restore) {
        /* bring back the tabs that were open last time */
        g_bulk_open = 1;
        for (int i = 0; i < S.session_count; i++) {
            if (GetFileAttributesW(S.session[i]) == INVALID_FILE_ATTRIBUTES) continue;
            if (open_file(S.session[i]) && i == S.session_active) active = g_cur;
        }
        g_bulk_open = 0;
    }
    if (file) open_file(file_full);
    else if (active >= 0) tab_activate(active);
    if (restore || !file) {
        g_preview_dirty = 1;
        after_load();
    }
    SetFocus(is_read() ? g_prev : g_edit);
    LocalFree(argv);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (g_find && IsDialogMessageW(g_find, &m)) continue;
        if (GetAncestor(m.hwnd, GA_ROOT) == g_main && TranslateAcceleratorW(g_main, g_accel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    for (int i = 0; i < IMAGE_CACHE; i++) free(g_images[i].data);
    free(g_mline);
    free(g_mcp);
    OleUninitialize();
    return (int)m.wParam;
}
