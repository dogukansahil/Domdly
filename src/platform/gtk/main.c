/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Linux front end, built on GTK 3 (the toolkit of the Debian desktop, linked
 * from the system, nothing copied into this project). Same model as on
 * Windows: every tab holds the Markdown source in an Edit view and shows it
 * formatted in a Read view; Ctrl+E switches between them and keeps the same
 * block on screen. The parser and the HTML renderer are the shared core.
 */
#include <gtk/gtk.h>
#include <cairo-pdf.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/markdown.h"
#include "core/mdbuf.h"
#include "core/render_html.h"
#include "core/md_html.h"
#include "version.h"
#include "../../../tools/icon_art.h"

enum { VIEW_EDIT = 0, VIEW_READ = 1 };
#define READ_MARGIN 56
enum { THEME_SYSTEM = 0, THEME_LIGHT = 1, THEME_DARK = 2, THEME_NIGHT = 3 };

/* ---- one tab ------------------------------------------------------------- */

typedef struct undo_step {
    int insert; /* 1 insert, 0 delete */
    int offset;
    char *text;
} undo_step;

typedef struct doc {
    GtkWidget *page, *fmt, *stack, *edit, *read; /* page = formatting toolbar + stack */
    GtkTextBuffer *ebuf, *rbuf;
    GtkWidget *tab_label, *tab_box;
    char *path;
    int crlf;
    int read_stale;
    GArray *mark_lines; /* source line of each block, in order */
    GPtrArray *marks;   /* GtkTextMark at each block start in the Read view */
    GPtrArray *undo, *redo;
    int in_undo;
} doc;

/* ---- application state --------------------------------------------------- */

static GtkApplication *g_app;
static GtkWidget *g_win, *g_header, *g_notebook, *g_status, *g_zoom_label, *g_btn_edit, *g_btn_read, *g_theme_btn;
static GtkWidget *g_search_bar, *g_search_entry;
static GtkWidget *g_path_btn, *g_path_label;
static int g_fmt_bar = 1; /* the formatting toolbar is shown */ /* full path of the open file, left end of the status bar */
static GtkCssProvider *g_css;
static int g_view = VIEW_EDIT, g_theme = THEME_SYSTEM, g_zoom = 100, g_switching;
static char *g_settings_path;

/* Files sidebar (left): lazy file tree of the workspace folder. */
enum { TC_NAME, TC_PATH, TC_DIR, TC_MD, TC_N };
#define TREE_MIN_WIDTH 160
#define TREE_MAX_ENTRIES 5000
static GtkWidget *g_paned, *g_side, *g_tree, *g_tree_stack, *g_tree_head, *g_tree_icon, *g_tree_menu, *g_tree_follow;
static GtkTreeStore *g_tree_store;
static int g_tree_open, g_tree_width = 240, g_tree_dbl, g_tree_press_exp;
static char *g_tree_root; /* pinned folder, NULL = follow the open file */
static char *g_tree_cur;  /* folder the tree shows now */
static char *g_tree_press;

/* Closed tabs, newest last, for Ctrl+Shift+T: saved documents by path,
 * never saved ones by their text. */
typedef struct closed_tab {
    char *path, *text;
} closed_tab;
static GQueue g_closed = G_QUEUE_INIT;
#define CLOSED_MAX 25

typedef struct palette {
    const char *bg, *fg, *muted, *heading, *link, *code_bg, *quote, *rule, *head_bg, *bar;
} palette;

static const palette light_pal = {"#ffffff", "#1f2328", "#6e7781", "#1f2328", "#0969da",
                                  "#f0f2f5", "#59636e", "#d1d9e0", "#f6f8fa", "#f6f7f9"};
static const palette dark_pal = {"#16181d", "#e6edf3", "#9198a1", "#f0f6fc", "#4493f8",
                                 "#262b33", "#9198a1", "#3d444d", "#22272e", "#1e2127"};
static const palette night_pal = {"#000000", "#c8c8c8", "#8c8c8c", "#e6e6e6", "#6ea0f0",
                                  "#141414", "#8c8c8c", "#2a2a2a", "#111111", "#0a0a0a"};

static int is_dark(void)
{
    if (g_theme == THEME_DARK || g_theme == THEME_NIGHT) return 1;
    if (g_theme == THEME_LIGHT) return 0;
    gboolean dark = FALSE;
    g_object_get(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", &dark, NULL);
    char *name = NULL;
    g_object_get(gtk_settings_get_default(), "gtk-theme-name", &name, NULL);
    int d = dark || (name && strstr(name, "dark"));
    g_free(name);
    return d;
}

static const palette *pal(void) { return g_theme == THEME_NIGHT ? &night_pal : is_dark() ? &dark_pal : &light_pal; }

static doc *current_doc(void)
{
    int n = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
    if (n < 0) return NULL;
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), n);
    return page ? (doc *)g_object_get_data(G_OBJECT(page), "doc") : NULL;
}

static doc *nth_doc(int i)
{
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(g_notebook), i);
    return page ? (doc *)g_object_get_data(G_OBJECT(page), "doc") : NULL;
}

static char *buffer_text(GtkTextBuffer *b)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(b, &s, &e);
    return gtk_text_buffer_get_text(b, &s, &e, FALSE);
}

static void tree_sync(const doc *d);
static void tree_refresh(void);

static const char *doc_name(const doc *d) { return d->path ? strrchr(d->path, '/') ? strrchr(d->path, '/') + 1 : d->path : "Untitled"; }

/* ---- settings (~/.config/domdly/settings.ini) ----------------------------- */

static void settings_save(void)
{
    GKeyFile *k = g_key_file_new();
    g_key_file_set_integer(k, "view", "mode", g_view);
    g_key_file_set_integer(k, "view", "theme", g_theme);
    g_key_file_set_integer(k, "view", "zoom", g_zoom);
    g_key_file_set_boolean(k, "view", "tree_open", g_tree_open);
    g_key_file_set_boolean(k, "view", "format_bar", g_fmt_bar);
    g_key_file_set_integer(k, "view", "tree_width", g_tree_width);
    g_key_file_set_string(k, "view", "tree_root", g_tree_root ? g_tree_root : "");
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    GPtrArray *tabs = g_ptr_array_new();
    int active = 0;
    for (int i = 0; i < n; i++) {
        doc *d = nth_doc(i);
        if (!d || !d->path) continue;
        if (i == gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook))) active = (int)tabs->len;
        g_ptr_array_add(tabs, d->path);
    }
    g_key_file_set_string_list(k, "session", "tabs", (const char *const *)tabs->pdata, tabs->len);
    g_key_file_set_integer(k, "session", "active", active);
    g_ptr_array_free(tabs, TRUE);
    char *dir = g_path_get_dirname(g_settings_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(k, g_settings_path, NULL);
    g_key_file_free(k);
}

/* ---- theme and CSS -------------------------------------------------------- */

static void apply_css(void)
{
    const palette *p = pal();
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", is_dark(), NULL);
    char *css = g_strdup_printf("textview, textview text { background-color: %s; color: %s; font-size: %.1fpt; }\n"
                                "textview.edit, textview.edit text { font-family: monospace; }\n"
                                ".domdly-bar { background-color: %s; padding: 2px 8px; }\n"
                                ".domdly-cell { border: 1px solid %s; padding: 4px 10px; }\n"
                                ".domdly-head { background-color: %s; font-weight: bold; }\n"
                                ".domdly-side { background-color: %s; border-right: 1px solid %s; }\n"
                                "treeview.domdly-tree, treeview.domdly-tree:backdrop { background-color: %s; }\n"
                                "treeview.domdly-tree:selected, treeview.domdly-tree:selected:focus, "
                                "treeview.domdly-tree:selected:backdrop { background-color: %s; color: %s; }\n"
                                ".domdly-sidehead { background-image: none; background-color: transparent; border: none; "
                                "border-radius: 0; box-shadow: none; padding: 5px 10px; font-weight: bold; color: %s; }\n"
                                ".domdly-sidehead:hover { background-color: %s; }\n"
                                "button.domdly-seg:checked { background-image: none; background-color: #2f6feb; "
                                "color: #ffffff; }\n"
                                ".domdly-fmt { background-color: %s; border-bottom: 1px solid %s; padding: 3px 28px; }\n"
                                ".domdly-fmt button { min-height: 0; min-width: 0; padding: 3px 7px; color: %s; }\n"
                                ".domdly-fmt button:hover { color: %s; }\n"
                                ".domdly-fmt separator { margin: 6px 5px; }\n"
                                ".domdly-fmt button:disabled { opacity: 0.35; }\n"
                                "%s",
                                p->bg, p->fg, 11.0 * g_zoom / 100.0, p->bar, p->rule, p->head_bg, p->bar, p->rule, p->bar,
                                p->rule, p->fg, p->fg, p->rule, p->bg, p->rule, p->muted, p->fg,
                                g_theme == THEME_NIGHT ? "window, headerbar, notebook, notebook header, notebook tab "
                                                         "{ background-color: #0a0a0a; background-image: none; color: #d6d6d6; }\n"
                                                       : "");
    gtk_css_provider_load_from_data(g_css, css, -1, NULL);
    g_free(css);
}

/* ---- the Read view --------------------------------------------------------- */

typedef struct render_ctx {
    doc *d;
    GtkTextIter it;
    GtkTextTag *stack[16];
    int depth;
    int list_depth;
    int quote;
    char *base_dir;
} render_ctx;

static GtkTextTag *tag(doc *d, const char *name)
{
    GtkTextTagTable *t = gtk_text_buffer_get_tag_table(d->rbuf);
    return gtk_text_tag_table_lookup(t, name);
}

/* Tags are created once per buffer; their colours follow the theme. */
static void ensure_tags(doc *d)
{
    const palette *p = pal();
    static const double scale[6] = {2.0, 1.6, 1.35, 1.18, 1.05, 1.0};
    GtkTextTagTable *t = gtk_text_buffer_get_tag_table(d->rbuf);
    if (!gtk_text_tag_table_lookup(t, "bold")) {
        gtk_text_buffer_create_tag(d->rbuf, "bold", "weight", PANGO_WEIGHT_BOLD, NULL);
        gtk_text_buffer_create_tag(d->rbuf, "italic", "style", PANGO_STYLE_ITALIC, NULL);
        gtk_text_buffer_create_tag(d->rbuf, "strike", "strikethrough", TRUE, NULL);
        gtk_text_buffer_create_tag(d->rbuf, "code", "family", "monospace", NULL);
        gtk_text_buffer_create_tag(d->rbuf, "link", "underline", PANGO_UNDERLINE_SINGLE, NULL);
        gtk_text_buffer_create_tag(d->rbuf, "para", "pixels-below-lines", 10, NULL);
        gtk_text_buffer_create_tag(d->rbuf, "tight", "pixels-below-lines", 3, NULL);
        /* tag margins replace the view's margin (READ_MARGIN), they do not add to it */
        gtk_text_buffer_create_tag(d->rbuf, "codeblock", "family", "monospace", "left-margin", READ_MARGIN + 16,
                                   "right-margin", READ_MARGIN + 16,
                                   "pixels-above-lines", 4, "pixels-below-lines", 12, NULL);
        gtk_text_buffer_create_tag(d->rbuf, "quote", "left-margin", READ_MARGIN + 24, NULL);
        gtk_text_buffer_create_tag(d->rbuf, "muted", NULL);
        gtk_text_buffer_create_tag(d->rbuf, "center", "justification", GTK_JUSTIFY_CENTER, NULL);
        gtk_text_buffer_create_tag(d->rbuf, "right", "justification", GTK_JUSTIFY_RIGHT, NULL);
        for (int i = 1; i <= 6; i++) {
            char name[8];
            g_snprintf(name, sizeof name, "h%d", i);
            gtk_text_buffer_create_tag(d->rbuf, name, "weight", PANGO_WEIGHT_BOLD, "scale", scale[i - 1],
                                       "pixels-above-lines", i <= 2 ? 18 : 12, "pixels-below-lines", 8, NULL);
        }
        for (int i = 1; i <= 8; i++) {
            char name[8];
            g_snprintf(name, sizeof name, "li%d", i);
            gtk_text_buffer_create_tag(d->rbuf, name, "left-margin", READ_MARGIN + 6 + 24 * i, "indent", -22, NULL);
        }
    }
    g_object_set(tag(d, "link"), "foreground", p->link, NULL);
    g_object_set(tag(d, "code"), "background", p->code_bg, NULL);
    g_object_set(tag(d, "codeblock"), "paragraph-background", p->code_bg, NULL);
    g_object_set(tag(d, "quote"), "foreground", p->quote, NULL);
    g_object_set(tag(d, "muted"), "foreground", p->muted, NULL);
    for (int i = 1; i <= 6; i++) {
        char name[8];
        g_snprintf(name, sizeof name, "h%d", i);
        g_object_set(tag(d, name), "foreground", i == 6 ? p->muted : p->heading, NULL);
    }
}

static void insert(render_ctx *c, const char *s, gssize n)
{
    GtkTextMark *m = gtk_text_buffer_create_mark(c->d->rbuf, NULL, &c->it, TRUE);
    gtk_text_buffer_insert(c->d->rbuf, &c->it, s, n);
    GtkTextIter start;
    gtk_text_buffer_get_iter_at_mark(c->d->rbuf, &start, m);
    for (int i = 0; i < c->depth; i++) gtk_text_buffer_apply_tag(c->d->rbuf, c->stack[i], &start, &c->it);
    gtk_text_buffer_delete_mark(c->d->rbuf, m);
}

static void push(render_ctx *c, GtkTextTag *t)
{
    if (t && c->depth < 16) c->stack[c->depth++] = t;
}

static void pop(render_ctx *c, int to) { c->depth = to; }

static void inlines(render_ctx *c, const md_node *parent);

static void insert_image(render_ctx *c, const md_node *n)
{
    char *path = NULL;
    if (n->url && md_url_is_safe(n->url, 1) && !strstr(n->url, "://")) {
        char *unesc = g_uri_unescape_string(n->url, NULL);
        path = g_path_is_absolute(unesc ? unesc : n->url) ? g_strdup(unesc ? unesc : n->url)
                                                         : g_build_filename(c->base_dir, unesc ? unesc : n->url, NULL);
        g_free(unesc);
    }
    GdkPixbuf *pb = path ? gdk_pixbuf_new_from_file(path, NULL) : NULL;
    g_free(path);
    if (pb) {
        int w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), maxw = 760 * g_zoom / 100;
        if (n->width > 0 && n->width * g_zoom / 100 < maxw) maxw = n->width * g_zoom / 100; /* width="..." */
        if (w > maxw) {
            GdkPixbuf *s = gdk_pixbuf_scale_simple(pb, maxw, h * maxw / w, GDK_INTERP_BILINEAR);
            g_object_unref(pb);
            pb = s;
        }
        /* the picture takes the surrounding tags too (alignment, list indent) */
        GtkTextMark *m = gtk_text_buffer_create_mark(c->d->rbuf, NULL, &c->it, TRUE);
        gtk_text_buffer_insert_pixbuf(c->d->rbuf, &c->it, pb);
        GtkTextIter start;
        gtk_text_buffer_get_iter_at_mark(c->d->rbuf, &start, m);
        for (int i = 0; i < c->depth; i++) gtk_text_buffer_apply_tag(c->d->rbuf, c->stack[i], &start, &c->it);
        gtk_text_buffer_delete_mark(c->d->rbuf, m);
        g_object_unref(pb);
        return;
    }
    mdbuf alt;
    mdbuf_init(&alt);
    md_plain_text(n, &alt);
    int keep = c->depth;
    push(c, tag(c->d, "muted"));
    insert(c, "[Image", -1);
    if (alt.len) {
        insert(c, ": ", -1);
        insert(c, alt.data, (gssize)alt.len);
    }
    insert(c, "]", -1);
    pop(c, keep);
    mdbuf_free(&alt);
}

static void inline_node(render_ctx *c, const md_node *n)
{
    int keep = c->depth;
    switch (n->type) {
    case MD_TEXT: insert(c, n->text, (gssize)n->len); break;
    case MD_SOFTBREAK: insert(c, " ", 1); break;
    case MD_LINEBREAK: insert(c, "\n", 1); break;
    case MD_CODE:
        push(c, tag(c->d, "code"));
        insert(c, n->text, (gssize)n->len);
        break;
    case MD_EMPH:
        push(c, tag(c->d, "italic"));
        inlines(c, n);
        break;
    case MD_STRONG:
        push(c, tag(c->d, "bold"));
        inlines(c, n);
        break;
    case MD_STRIKE:
        push(c, tag(c->d, "strike"));
        inlines(c, n);
        break;
    case MD_LINK:
        if (n->url && md_url_is_safe(n->url, 0)) {
            GtkTextTag *t = gtk_text_buffer_create_tag(c->d->rbuf, NULL, NULL);
            g_object_set_data_full(G_OBJECT(t), "url", g_strdup(n->url), g_free);
            push(c, tag(c->d, "link"));
            push(c, t);
        }
        inlines(c, n);
        break;
    case MD_IMAGE: insert_image(c, n); break;
    case MD_HTML_INLINE:
        if (n->len >= 4 && g_ascii_strncasecmp(n->text, "<br", 3) == 0) insert(c, "\n", 1);
        break;
    default: inlines(c, n); break;
    }
    pop(c, keep);
}

static void inlines(render_ctx *c, const md_node *parent)
{
    for (const md_node *n = parent->first; n; n = n->next) inline_node(c, n);
}

static void mark_block(render_ctx *c, const md_node *n)
{
    GtkTextMark *m = gtk_text_buffer_create_mark(c->d->rbuf, NULL, &c->it, TRUE);
    g_ptr_array_add(c->d->marks, m);
    int line = n->line;
    g_array_append_val(c->d->mark_lines, line);
}

static void blocks(render_ctx *c, const md_node *parent);
static void block(render_ctx *c, const md_node *n);

/* Pango markup for a table cell (labels in a grid). */
static void cell_markup(const md_node *parent, GString *o)
{
    for (const md_node *n = parent->first; n; n = n->next) {
        switch (n->type) {
        case MD_TEXT: {
            char *e = g_markup_escape_text(n->text, (gssize)n->len);
            g_string_append(o, e);
            g_free(e);
            break;
        }
        case MD_SOFTBREAK: g_string_append_c(o, ' '); break;
        case MD_CODE: {
            char *e = g_markup_escape_text(n->text, (gssize)n->len);
            g_string_append_printf(o, "<tt>%s</tt>", e);
            g_free(e);
            break;
        }
        case MD_EMPH: g_string_append(o, "<i>"); cell_markup(n, o); g_string_append(o, "</i>"); break;
        case MD_STRONG: g_string_append(o, "<b>"); cell_markup(n, o); g_string_append(o, "</b>"); break;
        case MD_STRIKE: g_string_append(o, "<s>"); cell_markup(n, o); g_string_append(o, "</s>"); break;
        case MD_LINK:
            g_string_append_printf(o, "<span foreground=\"%s\" underline=\"single\">", pal()->link);
            cell_markup(n, o);
            g_string_append(o, "</span>");
            break;
        default: cell_markup(n, o); break;
        }
    }
}

static void block_table(render_ctx *c, const md_node *t)
{
    GtkTextChildAnchor *a = gtk_text_buffer_create_child_anchor(c->d->rbuf, &c->it);
    GtkWidget *grid = gtk_grid_new();
    int r = 0;
    for (const md_node *row = t->first; row; row = row->next, r++) {
        int col = 0;
        for (const md_node *cell = row->first; cell; cell = cell->next, col++) {
            GString *m = g_string_new(NULL);
            cell_markup(cell, m);
            GtkWidget *l = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(l), m->str);
            g_string_free(m, TRUE);
            gtk_label_set_xalign(GTK_LABEL(l), cell->align == MD_ALIGN_RIGHT    ? 1.0f
                                               : cell->align == MD_ALIGN_CENTER ? 0.5f
                                                                                : 0.0f);
            gtk_label_set_line_wrap(GTK_LABEL(l), TRUE);
            gtk_label_set_max_width_chars(GTK_LABEL(l), 40); /* natural width before wrapping */
            GtkStyleContext *sc = gtk_widget_get_style_context(l);
            gtk_style_context_add_class(sc, "domdly-cell");
            if (row->header) gtk_style_context_add_class(sc, "domdly-head");
            gtk_grid_attach(GTK_GRID(grid), l, col, r, 1, 1);
        }
    }
    gtk_widget_show_all(grid);
    gtk_text_view_add_child_at_anchor(GTK_TEXT_VIEW(c->d->read), grid, a);
    insert(c, "\n", 1);
}

static void block_list(render_ctx *c, const md_node *list)
{
    int number = list->ordered ? list->start : 0;
    c->list_depth++;
    char tname[8];
    g_snprintf(tname, sizeof tname, "li%d", c->list_depth > 8 ? 8 : c->list_depth);
    for (const md_node *item = list->first; item; item = item->next, number++) {
        int keep = c->depth;
        push(c, tag(c->d, tname));
        push(c, tag(c->d, list->tight ? "tight" : "para"));
        mark_block(c, item);
        char bullet[32];
        /* the bullet hangs in the negative indent of the item tag */
        if (item->task) g_snprintf(bullet, sizeof bullet, "%s ", item->task == MD_TASK_DONE ? "☑" : "☐");
        else if (list->ordered) g_snprintf(bullet, sizeof bullet, "%d%c ", number, list->marker == ')' ? ')' : '.');
        else g_snprintf(bullet, sizeof bullet, "%s  ", (c->list_depth % 3) == 1 ? "•" : (c->list_depth % 3) == 2 ? "◦" : "▪");
        insert(c, bullet, -1);
        const md_node *child = item->first;
        if (child && child->type == MD_PARAGRAPH) {
            inlines(c, child);
            child = child->next;
        }
        insert(c, "\n", 1);
        pop(c, keep);
        for (; child; child = child->next) {
            int k2 = c->depth;
            push(c, tag(c->d, tname));
            if (child->type == MD_PARAGRAPH) {
                push(c, tag(c->d, "para"));
                mark_block(c, child);
                inlines(c, child);
                insert(c, "\n", 1);
            } else if (child->type == MD_LIST) {
                pop(c, k2);
                block_list(c, child); /* nested list */
            } else {
                block(c, child); /* code block, quote... inside the item */
            }
            pop(c, k2);
        }
    }
    c->list_depth--;
}

static void block(render_ctx *c, const md_node *n)
{
    int keep = c->depth;
    switch (n->type) {
    case MD_PARAGRAPH:
        if (!n->from_html) mark_block(c, n);
        push(c, tag(c->d, "para"));
        if (n->align == MD_ALIGN_CENTER || n->align == MD_ALIGN_RIGHT)
            push(c, tag(c->d, n->align == MD_ALIGN_CENTER ? "center" : "right"));
        inlines(c, n);
        insert(c, "\n", 1);
        break;
    case MD_HEADING: {
        char name[8];
        g_snprintf(name, sizeof name, "h%d", n->level < 1 ? 1 : n->level > 6 ? 6 : n->level);
        if (!n->from_html) mark_block(c, n);
        push(c, tag(c->d, name));
        if (n->align == MD_ALIGN_CENTER || n->align == MD_ALIGN_RIGHT)
            push(c, tag(c->d, n->align == MD_ALIGN_CENTER ? "center" : "right"));
        inlines(c, n);
        insert(c, "\n", 1);
        break;
    }
    case MD_CODE_BLOCK: {
        mark_block(c, n);
        push(c, tag(c->d, "codeblock"));
        size_t len = n->len;
        while (len && n->text[len - 1] == '\n') len--;
        insert(c, n->text ? n->text : "", (gssize)len);
        insert(c, "\n", 1);
        break;
    }
    case MD_THEMATIC_BREAK:
        push(c, tag(c->d, "muted"));
        push(c, tag(c->d, "para"));
        insert(c, "─────────────────"
                  "───────────────\n",
               -1);
        break;
    case MD_BLOCKQUOTE:
        push(c, tag(c->d, "quote"));
        blocks(c, n);
        break;
    case MD_LIST: block_list(c, n); break;
    case MD_TABLE:
        mark_block(c, n);
        block_table(c, n);
        break;
    case MD_HTML_BLOCK: {
        mark_block(c, n);
        if (n->from_html) { /* HTML shown through the nodes md_expand_html() made */
            blocks(c, n);
            break;
        }
        push(c, tag(c->d, "muted"));
        push(c, tag(c->d, "code"));
        push(c, tag(c->d, "para"));
        size_t len = n->len;
        while (len && n->text[len - 1] == '\n') len--;
        insert(c, n->text, (gssize)len);
        insert(c, "\n", 1);
        break;
    }
    default: break;
    }
    pop(c, keep);
}

static void blocks(render_ctx *c, const md_node *parent)
{
    if (!parent) return;
    for (const md_node *n = parent->first; n; n = n->next) block(c, n);
}

static void render_read(doc *d)
{
    ensure_tags(d);
    char *text = buffer_text(d->ebuf);
    md_node *tree = md_parse(text, strlen(text));
    md_expand_html(tree); /* HTML in the document is shown, safely */
    g_free(text);
    for (guint i = 0; i < d->marks->len; i++) gtk_text_buffer_delete_mark(d->rbuf, g_ptr_array_index(d->marks, i));
    g_ptr_array_set_size(d->marks, 0);
    g_array_set_size(d->mark_lines, 0);
    gtk_text_buffer_set_text(d->rbuf, "", 0);
    render_ctx c;
    memset(&c, 0, sizeof c);
    c.d = d;
    c.base_dir = d->path ? g_path_get_dirname(d->path) : g_get_current_dir();
    gtk_text_buffer_get_start_iter(d->rbuf, &c.it);
    blocks(&c, tree);
    if (!tree->first) {
        push(&c, tag(d, "muted"));
        insert(&c, "Nothing to read yet. Press Ctrl+E to write, or open a file with Ctrl+O.", -1);
    }
    g_free(c.base_dir);
    md_free(tree);
    d->read_stale = 0;
}

/* ---- status, title, tab label ---------------------------------------------- */

static int count_words(const char *s)
{
    int w = 0, in = 0;
    for (; *s; s++) {
        int sp = *s == ' ' || *s == '\n' || *s == '\t' || *s == '\r';
        if (!sp && !in) w++;
        in = !sp;
    }
    return w;
}

static void update_status(void)
{
    doc *d = current_doc();
    if (!d) return;
    char *text = buffer_text(d->ebuf);
    int words = count_words(text);
    g_free(text);
    char *s;
    if (g_view == VIEW_READ) {
        s = g_strdup_printf("%d word%s  ·  %d min read  ·  Ctrl+click opens a link  ·  Ctrl+E shows the Markdown",
                            words, words == 1 ? "" : "s", words / 200 + 1);
    } else {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(d->ebuf, &it, gtk_text_buffer_get_insert(d->ebuf));
        s = g_strdup_printf("Ln %d, Col %d  ·  %d word%s  ·  UTF-8  ·  %s", gtk_text_iter_get_line(&it) + 1,
                            gtk_text_iter_get_line_offset(&it) + 1, words, words == 1 ? "" : "s", d->crlf ? "CRLF" : "LF");
    }
    gtk_label_set_text(GTK_LABEL(g_status), s);
    g_free(s);
    char z[16];
    g_snprintf(z, sizeof z, "%d%%", g_zoom);
    gtk_label_set_text(GTK_LABEL(g_zoom_label), z);
}

static void update_title(doc *d)
{
    if (!d) return;
    int mod = gtk_text_buffer_get_modified(d->ebuf);
    char *label = g_strdup_printf("%s%s", doc_name(d), mod ? " ●" : "");
    gtk_label_set_text(GTK_LABEL(d->tab_label), label);
    g_free(label);
    if (d == current_doc()) {
        gtk_header_bar_set_title(GTK_HEADER_BAR(g_header), doc_name(d));
        char *sub = d->path ? g_path_get_dirname(d->path) : NULL;
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(g_header), sub ? sub : "Domdly " DOMDLY_VERSION_STR);
        g_free(sub);
        char *wt = g_strdup_printf("%s%s - Domdly", mod ? "● " : "", doc_name(d));
        gtk_window_set_title(GTK_WINDOW(g_win), wt);
        g_free(wt);
        if (g_path_btn) {
            gtk_label_set_text(GTK_LABEL(g_path_label), d->path ? d->path : "");
            gtk_widget_set_visible(g_path_btn, d->path != NULL);
        }
    }
}

/* Clicking the path in the status bar opens the folder that holds the file. */
static void on_path_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    (void)data;
    doc *d = current_doc();
    if (!d || !d->path) return;
    char *dir = g_path_get_dirname(d->path);
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri) gtk_show_uri_on_window(GTK_WINDOW(g_win), uri, GDK_CURRENT_TIME, NULL);
    g_free(uri);
    g_free(dir);
}

/* ---- switching Edit / Read, keeping the block on screen -------------------- */

static int top_line_of_edit(doc *d)
{
    GdkRectangle r;
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(d->edit), &r);
    GtkTextIter it;
    gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(d->edit), &it, r.y, NULL);
    return gtk_text_iter_get_line(&it);
}

static void show_view(doc *d, int view)
{
    gtk_widget_set_sensitive(d->fmt, view != VIEW_READ);
    if (view == VIEW_READ) {
        int line = top_line_of_edit(d);
        if (d->read_stale) render_read(d);
        gtk_stack_set_visible_child_name(GTK_STACK(d->stack), "read");
        int best = -1;
        for (guint i = 0; i < d->mark_lines->len; i++)
            if (g_array_index(d->mark_lines, int, i) <= line) best = (int)i;
        if (best >= 0)
            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(d->read), g_ptr_array_index(d->marks, best), 0, TRUE, 0, 0);
        gtk_widget_grab_focus(d->read);
    } else {
        int line = -1;
        if (gtk_stack_get_visible_child(GTK_STACK(d->stack)) == gtk_widget_get_parent(d->read) && d->marks->len) {
            GdkRectangle r;
            gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(d->read), &r);
            GtkTextIter top;
            gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(d->read), &top, r.y, NULL);
            int off = gtk_text_iter_get_offset(&top);
            for (guint i = 0; i < d->marks->len; i++) {
                GtkTextIter m;
                gtk_text_buffer_get_iter_at_mark(d->rbuf, &m, g_ptr_array_index(d->marks, i));
                if (gtk_text_iter_get_offset(&m) <= off) line = g_array_index(d->mark_lines, int, i);
            }
        }
        gtk_stack_set_visible_child_name(GTK_STACK(d->stack), "edit");
        if (line >= 0) {
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(d->ebuf, &it, line);
            GtkTextMark *m = gtk_text_buffer_create_mark(d->ebuf, NULL, &it, TRUE);
            gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(d->edit), m, 0, TRUE, 0, 0);
            gtk_text_buffer_delete_mark(d->ebuf, m);
        }
        gtk_widget_grab_focus(d->edit);
    }
}

static void set_view(int view)
{
    g_view = view;
    g_switching = 1;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_btn_edit), view == VIEW_EDIT);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_btn_read), view == VIEW_READ);
    g_switching = 0;
    doc *d = current_doc();
    if (d) show_view(d, view);
    update_status();
    settings_save();
}

static void on_view_toggled(GtkToggleButton *b, gpointer data)
{
    (void)data;
    if (g_switching) return;
    int want = b == GTK_TOGGLE_BUTTON(g_btn_read) ? VIEW_READ : VIEW_EDIT;
    if (!gtk_toggle_button_get_active(b)) want = want == VIEW_READ ? VIEW_EDIT : VIEW_READ;
    set_view(want);
}

/* ---- undo for the Edit view (GTK 3 text views have none) ------------------- */

static void undo_free(gpointer p)
{
    undo_step *u = (undo_step *)p;
    g_free(u->text);
    g_free(u);
}

static void record(doc *d, int insert_, int offset, const char *text)
{
    if (d->in_undo) return;
    g_ptr_array_set_size(d->redo, 0);
    if (d->undo->len) { /* merge typing into one step */
        undo_step *last = g_ptr_array_index(d->undo, d->undo->len - 1);
        if (insert_ && last->insert && strlen(text) == 1 && text[0] != '\n' && !strchr(last->text, '\n') &&
            last->offset + (int)g_utf8_strlen(last->text, -1) == offset) {
            char *t = g_strconcat(last->text, text, NULL);
            g_free(last->text);
            last->text = t;
            return;
        }
    }
    undo_step *u = g_new0(undo_step, 1);
    u->insert = insert_;
    u->offset = offset;
    u->text = g_strdup(text);
    g_ptr_array_add(d->undo, u);
}

static void on_insert_text(GtkTextBuffer *b, GtkTextIter *at, gchar *text, gint len, gpointer data)
{
    (void)b;
    char *t = g_strndup(text, (gsize)len);
    record((doc *)data, 1, gtk_text_iter_get_offset(at), t);
    g_free(t);
}

static void on_delete_range(GtkTextBuffer *b, GtkTextIter *s, GtkTextIter *e, gpointer data)
{
    char *t = gtk_text_buffer_get_text(b, s, e, FALSE);
    record((doc *)data, 0, gtk_text_iter_get_offset(s), t);
    g_free(t);
}

static void undo_redo(doc *d, int redo)
{
    GPtrArray *from = redo ? d->redo : d->undo, *to = redo ? d->undo : d->redo;
    if (!from->len) return;
    undo_step *u = g_ptr_array_steal_index(from, from->len - 1);
    d->in_undo = 1;
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_offset(d->ebuf, &s, u->offset);
    int do_insert = redo ? u->insert : !u->insert;
    if (do_insert) {
        gtk_text_buffer_insert(d->ebuf, &s, u->text, -1);
    } else {
        gtk_text_buffer_get_iter_at_offset(d->ebuf, &e, u->offset + (int)g_utf8_strlen(u->text, -1));
        gtk_text_buffer_delete(d->ebuf, &s, &e);
    }
    gtk_text_buffer_get_iter_at_offset(d->ebuf, &s, u->offset);
    gtk_text_buffer_place_cursor(d->ebuf, &s);
    d->in_undo = 0;
    g_ptr_array_add(to, u);
}

/* ---- tabs ------------------------------------------------------------------ */

static void on_changed(GtkTextBuffer *b, gpointer data)
{
    (void)b;
    doc *d = (doc *)data;
    d->read_stale = 1;
    update_title(d);
    update_status();
}

static void on_modified(GtkTextBuffer *b, gpointer data)
{
    (void)b;
    update_title((doc *)data);
}

static void on_mark_set(GtkTextBuffer *b, GtkTextIter *it, GtkTextMark *m, gpointer data)
{
    (void)b;
    (void)it;
    (void)data;
    if (m == gtk_text_buffer_get_insert(b)) update_status();
}

static gboolean open_link_at(doc *d, GtkTextIter *it)
{
    for (GSList *l = gtk_text_iter_get_tags(it), *p = l; p; p = p->next) {
        const char *url = g_object_get_data(G_OBJECT(p->data), "url");
        if (!url) continue;
        if (url[0] == '#') {
            g_slist_free(l);
            return TRUE;
        }
        char *full = NULL;
        if (!strstr(url, "://") && !g_str_has_prefix(url, "mailto:") && d->path) {
            char *dir = g_path_get_dirname(d->path);
            char *path = g_build_filename(dir, url, NULL);
            full = g_filename_to_uri(path, NULL, NULL);
            g_free(dir);
            g_free(path);
        }
        gtk_show_uri_on_window(GTK_WINDOW(g_win), full ? full : url, GDK_CURRENT_TIME, NULL);
        g_free(full);
        g_slist_free(l);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_read_click(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    if (ev->type != GDK_BUTTON_RELEASE || ev->button != 1 || !(ev->state & GDK_CONTROL_MASK)) return FALSE;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(w), GTK_TEXT_WINDOW_WIDGET, (int)ev->x, (int)ev->y, &bx, &by);
    GtkTextIter it;
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(w), &it, bx, by);
    return open_link_at((doc *)data, &it);
}

/* Enter in a list continues it; Enter on an empty item ends the list. */
static gboolean on_edit_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    doc *d = (doc *)data;
    if ((ev->keyval != GDK_KEY_Return && ev->keyval != GDK_KEY_KP_Enter) || (ev->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK)))
        return FALSE;
    GtkTextIter cur, ls;
    gtk_text_buffer_get_iter_at_mark(d->ebuf, &cur, gtk_text_buffer_get_insert(d->ebuf));
    ls = cur;
    gtk_text_iter_set_line_offset(&ls, 0);
    char *line = gtk_text_buffer_get_text(d->ebuf, &ls, &cur, FALSE);
    const char *p = line;
    while (*p == ' ') p++;
    const char *m = p;
    char next[64] = "";
    if ((*m == '-' || *m == '*' || *m == '+') && m[1] == ' ') {
        m += 2;
        if (!strncmp(m, "[ ] ", 4) || !strncmp(m, "[x] ", 4) || !strncmp(m, "[X] ", 4)) m += 4;
        g_snprintf(next, sizeof next, "%.*s%c %s", (int)(p - line), line, *p, m - p > 2 ? "[ ] " : "");
    } else if (g_ascii_isdigit(*m)) {
        int n = atoi(m);
        while (g_ascii_isdigit(*m)) m++;
        if ((*m == '.' || *m == ')') && m[1] == ' ') {
            char delim = *m;
            m += 2;
            g_snprintf(next, sizeof next, "%.*s%d%c ", (int)(p - line), line, n + 1, delim);
        } else {
            m = p;
        }
    }
    gboolean handled = FALSE;
    if (next[0]) {
        if (!*m) { /* empty item: remove the marker */
            gtk_text_buffer_delete(d->ebuf, &ls, &cur);
        } else {
            char *ins = g_strconcat("\n", next, NULL);
            gtk_text_buffer_insert_at_cursor(d->ebuf, ins, -1);
            g_free(ins);
        }
        handled = TRUE;
    }
    g_free(line);
    return handled;
}

static void close_tab(doc *d);

/* ---- formatting the Markdown source ------------------------------------------
 * The same commands as the Windows version, applied to the Edit view: inline
 * marks wrap (or unwrap) the selection, line commands toggle a prefix on every
 * selected line. All of it is one undo step. */

static void fmt_wrap(doc *d, const char *mark, const char *placeholder)
{
    GtkTextBuffer *b = d->ebuf;
    GtkTextIter s, e;
    int n = (int)strlen(mark);
    gtk_text_buffer_begin_user_action(b);
    if (!gtk_text_buffer_get_selection_bounds(b, &s, &e)) {
        char *t = g_strconcat(mark, placeholder, mark, NULL);
        int at = gtk_text_iter_get_offset(&s);
        gtk_text_buffer_insert(b, &s, t, -1);
        g_free(t);
        gtk_text_buffer_get_iter_at_offset(b, &s, at + n);
        gtk_text_buffer_get_iter_at_offset(b, &e, at + n + (int)g_utf8_strlen(placeholder, -1));
        gtk_text_buffer_select_range(b, &s, &e);
        gtk_text_buffer_end_user_action(b);
        return;
    }
    int so = gtk_text_iter_get_offset(&s), eo = gtk_text_iter_get_offset(&e);
    GtkTextIter bs = s, ae = e;
    gtk_text_iter_backward_chars(&bs, n);
    gtk_text_iter_forward_chars(&ae, n);
    char *before = gtk_text_buffer_get_text(b, &bs, &s, FALSE), *after = gtk_text_buffer_get_text(b, &e, &ae, FALSE);
    char *inner = gtk_text_buffer_get_text(b, &s, &e, FALSE);
    size_t il = strlen(inner);
    if (so >= n && !strcmp(before, mark) && !strcmp(after, mark)) { /* marks just outside: remove them */
        gtk_text_buffer_delete(b, &e, &ae);
        gtk_text_buffer_get_iter_at_offset(b, &s, so);
        gtk_text_buffer_get_iter_at_offset(b, &bs, so - n);
        gtk_text_buffer_delete(b, &bs, &s);
        so -= n;
        eo -= n;
    } else if (il >= 2 * (size_t)n && !strncmp(inner, mark, n) && !strcmp(inner + il - n, mark)) { /* inside */
        gtk_text_buffer_get_iter_at_offset(b, &ae, eo);
        gtk_text_buffer_get_iter_at_offset(b, &e, eo - n);
        gtk_text_buffer_delete(b, &e, &ae);
        gtk_text_buffer_get_iter_at_offset(b, &s, so);
        gtk_text_buffer_get_iter_at_offset(b, &bs, so + n);
        gtk_text_buffer_delete(b, &s, &bs);
        eo -= 2 * n;
    } else {
        gtk_text_buffer_insert(b, &e, mark, -1);
        gtk_text_buffer_get_iter_at_offset(b, &s, so);
        gtk_text_buffer_insert(b, &s, mark, -1);
        so += n;
        eo += n;
    }
    g_free(before);
    g_free(after);
    g_free(inner);
    gtk_text_buffer_get_iter_at_offset(b, &s, so);
    gtk_text_buffer_get_iter_at_offset(b, &e, eo);
    gtk_text_buffer_select_range(b, &s, &e);
    gtk_text_buffer_end_user_action(b);
}

enum { PFX_HEADING, PFX_QUOTE, PFX_BULLET, PFX_NUMBER, PFX_TASK };

/* Length of the block prefix at the start of line l (after indentation), and
   which kind it is (-1 for none). */
static int line_prefix(const char *l, int *kind, int *level)
{
    const char *p = l;
    *kind = -1;
    *level = 0;
    while (*p == ' ') p++;
    const char *q = p;
    if (*q == '#') {
        while (*q == '#') q++;
        if (*q == ' ' && q - p <= 6) {
            *kind = PFX_HEADING;
            *level = (int)(q - p);
            return (int)(q + 1 - l);
        }
        return 0;
    }
    if (*q == '>' ) {
        *kind = PFX_QUOTE;
        return (int)(q + (q[1] == ' ' ? 2 : 1) - l);
    }
    if ((*q == '-' || *q == '*' || *q == '+') && q[1] == ' ') {
        if (q[2] == '[' && (q[3] == ' ' || q[3] == 'x' || q[3] == 'X') && q[4] == ']' && q[5] == ' ') {
            *kind = PFX_TASK;
            return (int)(q + 6 - l);
        }
        *kind = PFX_BULLET;
        return (int)(q + 2 - l);
    }
    if (g_ascii_isdigit(*q)) {
        while (g_ascii_isdigit(*q)) q++;
        if ((*q == '.' || *q == ')') && q[1] == ' ') {
            *kind = PFX_NUMBER;
            return (int)(q + 2 - l);
        }
    }
    return 0;
}

static void fmt_prefix(doc *d, int kind, int level)
{
    GtkTextBuffer *b = d->ebuf;
    GtkTextIter s, e;
    gtk_text_buffer_get_selection_bounds(b, &s, &e);
    int first = gtk_text_iter_get_line(&s), last = gtk_text_iter_get_line(&e);
    if (last > first && gtk_text_iter_starts_line(&e)) last--;
    /* when every line already has this prefix, the command removes it */
    int all = 1;
    for (int i = first; i <= last && all; i++) {
        GtkTextIter a, z;
        gtk_text_buffer_get_iter_at_line(b, &a, i);
        z = a;
        if (!gtk_text_iter_ends_line(&z)) gtk_text_iter_forward_to_line_end(&z);
        char *t = gtk_text_buffer_get_text(b, &a, &z, FALSE);
        int k, lv;
        line_prefix(t, &k, &lv);
        if (k != kind || (kind == PFX_HEADING && lv != level)) all = 0;
        g_free(t);
    }
    gtk_text_buffer_begin_user_action(b);
    for (int i = first, num = 1; i <= last; i++, num++) {
        GtkTextIter a, z;
        gtk_text_buffer_get_iter_at_line(b, &a, i);
        z = a;
        if (!gtk_text_iter_ends_line(&z)) gtk_text_iter_forward_to_line_end(&z);
        char *t = gtk_text_buffer_get_text(b, &a, &z, FALSE);
        int k, lv, len = line_prefix(t, &k, &lv);
        int indent = 0;
        while (t[indent] == ' ') indent++;
        if (k < 0) len = indent;
        char np[32] = "";
        if (!all) {
            switch (kind) {
            case PFX_HEADING: g_snprintf(np, sizeof np, "%.*s ", level, "######"); break;
            case PFX_QUOTE: g_strlcpy(np, "> ", sizeof np); break;
            case PFX_BULLET: g_strlcpy(np, "- ", sizeof np); break;
            case PFX_NUMBER: g_snprintf(np, sizeof np, "%d. ", num); break;
            default: g_strlcpy(np, "- [ ] ", sizeof np); break;
            }
        }
        GtkTextIter ps, pe;
        gtk_text_buffer_get_iter_at_line_offset(b, &ps, i, indent);
        gtk_text_buffer_get_iter_at_line_index(b, &pe, i, len);
        gtk_text_buffer_delete(b, &ps, &pe);
        gtk_text_buffer_insert(b, &ps, np, -1);
        g_free(t);
    }
    gtk_text_buffer_end_user_action(b);
}

/* Inserts text on lines of its own at the caret (code block, rule) and selects
   the part between pre and post. */
static void fmt_block(doc *d, const char *pre, const char *placeholder, const char *post)
{
    GtkTextBuffer *b = d->ebuf;
    GtkTextIter s, e;
    int had = gtk_text_buffer_get_selection_bounds(b, &s, &e);
    char *body = had ? gtk_text_buffer_get_text(b, &s, &e, FALSE) : g_strdup(placeholder);
    gtk_text_buffer_begin_user_action(b);
    if (had) gtk_text_buffer_delete(b, &s, &e);
    const char *lead = gtk_text_iter_starts_line(&s) ? "" : "\n";
    int at = gtk_text_iter_get_offset(&s) + (int)g_utf8_strlen(lead, -1) + (int)g_utf8_strlen(pre, -1);
    char *t = g_strconcat(lead, pre, body, post, NULL);
    gtk_text_buffer_insert(b, &s, t, -1);
    gtk_text_buffer_get_iter_at_offset(b, &s, at);
    gtk_text_buffer_get_iter_at_offset(b, &e, at + (int)g_utf8_strlen(body, -1));
    gtk_text_buffer_select_range(b, &s, &e);
    gtk_text_buffer_end_user_action(b);
    g_free(t);
    g_free(body);
}

static void fmt_link(doc *d)
{
    GtkTextBuffer *b = d->ebuf;
    GtkTextIter s, e;
    int had = gtk_text_buffer_get_selection_bounds(b, &s, &e);
    char *label = had ? gtk_text_buffer_get_text(b, &s, &e, FALSE) : g_strdup("link text");
    gtk_text_buffer_begin_user_action(b);
    if (had) gtk_text_buffer_delete(b, &s, &e);
    int at = gtk_text_iter_get_offset(&s), ll = (int)g_utf8_strlen(label, -1);
    char *t = g_strconcat("[", label, "](https://)", NULL);
    gtk_text_buffer_insert(b, &s, t, -1);
    /* with a label, the address is selected for typing; without, the label */
    int a = had ? at + ll + 3 : at + 1, z = had ? at + ll + 11 : at + 1 + ll;
    gtk_text_buffer_get_iter_at_offset(b, &s, a);
    gtk_text_buffer_get_iter_at_offset(b, &e, z);
    gtk_text_buffer_select_range(b, &s, &e);
    gtk_text_buffer_end_user_action(b);
    g_free(t);
    g_free(label);
}

/* Runs a formatting command by its action name; returns 0 if it is not one. */
static int format_command(const char *name)
{
    static const char *const names[] = {"bold", "italic", "strike", "code", "link", "h1", "h2", "h3",
                                        "quote", "bullet", "numbered", "task", "codeblock", "hr"};
    int id = -1;
    for (int i = 0; i < (int)G_N_ELEMENTS(names); i++)
        if (!strcmp(name, names[i])) id = i;
    if (id < 0) return 0;
    doc *d = current_doc();
    if (!d || g_view != VIEW_EDIT) return 1; /* the Read view is not editable on Linux */
    switch (id) {
    case 0: fmt_wrap(d, "**", "bold text"); break;
    case 1: fmt_wrap(d, "*", "italic text"); break;
    case 2: fmt_wrap(d, "~~", "struck text"); break;
    case 3: fmt_wrap(d, "`", "code"); break;
    case 4: fmt_link(d); break;
    case 5: case 6: case 7: fmt_prefix(d, PFX_HEADING, id - 4); break;
    case 8: fmt_prefix(d, PFX_QUOTE, 0); break;
    case 9: fmt_prefix(d, PFX_BULLET, 0); break;
    case 10: fmt_prefix(d, PFX_NUMBER, 0); break;
    case 11: fmt_prefix(d, PFX_TASK, 0); break;
    case 12: fmt_block(d, "```\n", "code", "\n```\n"); break;
    default: fmt_block(d, "\n---\n", "", "\n"); break;
    }
    gtk_widget_grab_focus(d->edit);
    return 1;
}

/* Toolbar icons drawn with cairo, so they look the same on every desktop
   theme and need no icon files. */
enum { FI_BULLETS = 1, FI_NUMBERS, FI_TASK, FI_QUOTE, FI_LINK, FI_HR };

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static gboolean fmt_icon_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int kind = GPOINTER_TO_INT(data);
    GdkRGBA c;
    gtk_style_context_get_color(gtk_widget_get_style_context(gtk_widget_get_parent(w)),
                                gtk_widget_get_state_flags(gtk_widget_get_parent(w)), &c);
    gdk_cairo_set_source_rgba(cr, &c);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(cr, 1.6);
    switch (kind) {
    case FI_BULLETS:
        for (int i = 0; i < 3; i++) {
            cairo_move_to(cr, 7.5, 4 + i * 5);
            cairo_line_to(cr, 16.5, 4 + i * 5);
        }
        cairo_stroke(cr);
        for (int i = 0; i < 3; i++) {
            cairo_new_sub_path(cr);
            cairo_arc(cr, 3.5, 4 + i * 5, 1.5, 0, 2 * G_PI);
        }
        cairo_fill(cr);
        break;
    case FI_NUMBERS:
        cairo_move_to(cr, 8, 5.5);
        cairo_line_to(cr, 16.5, 5.5);
        cairo_move_to(cr, 8, 12.5);
        cairo_line_to(cr, 16.5, 12.5);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1.3);
        cairo_move_to(cr, 2.2, 3.4); /* 1 */
        cairo_line_to(cr, 3.8, 2.4);
        cairo_line_to(cr, 3.8, 8.4);
        cairo_move_to(cr, 1.8, 10.2); /* 2 */
        cairo_line_to(cr, 5.2, 10.2);
        cairo_line_to(cr, 5.2, 12.2);
        cairo_line_to(cr, 1.8, 15.4);
        cairo_line_to(cr, 5.4, 15.4);
        cairo_stroke(cr);
        break;
    case FI_TASK:
        rounded(cr, 2.5, 2.5, 13, 13, 2.5);
        cairo_move_to(cr, 5.5, 9);
        cairo_line_to(cr, 8, 11.5);
        cairo_line_to(cr, 12.5, 6.5);
        cairo_stroke(cr);
        break;
    case FI_QUOTE:
        cairo_set_line_width(cr, 2.2);
        cairo_move_to(cr, 2.5, 2.5);
        cairo_line_to(cr, 2.5, 15.5);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1.6);
        cairo_move_to(cr, 7, 4);
        cairo_line_to(cr, 16.5, 4);
        cairo_move_to(cr, 7, 9);
        cairo_line_to(cr, 16.5, 9);
        cairo_move_to(cr, 7, 14);
        cairo_line_to(cr, 12.5, 14);
        cairo_stroke(cr);
        break;
    case FI_LINK:
        cairo_translate(cr, 9, 9);
        cairo_rotate(cr, -G_PI / 4);
        rounded(cr, -8.4, -3, 9.4, 6, 3);
        rounded(cr, -1, -3, 9.4, 6, 3);
        cairo_stroke(cr);
        break;
    default: /* FI_HR */
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, 4, 3.5);
        cairo_line_to(cr, 14, 3.5);
        cairo_move_to(cr, 4, 14.5);
        cairo_line_to(cr, 14, 14.5);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 2.0);
        cairo_move_to(cr, 1.5, 9);
        cairo_line_to(cr, 16.5, 9);
        cairo_stroke(cr);
        break;
    }
    return FALSE;
}

static GtkWidget *fmt_bar_new(void)
{
    static const struct {
        const char *action, *markup, *tip;
        int icon;
    } items[] = {
        {"bold", "<b>B</b>", "Bold (Ctrl+B)", 0},
        {"italic", "<i><span font_family=\"serif\">I</span></i>", "Italic (Ctrl+I)", 0},
        {"strike", "<s>S</s>", "Strikethrough (Ctrl+Shift+X)", 0},
        {"code", "<tt><b>&lt;/&gt;</b></tt>", "Inline code (Ctrl+`)", 0},
        {NULL, NULL, NULL, 0},
        {"h1", "<b>H1</b>", "Heading 1 (Ctrl+1)", 0},
        {"h2", "<b>H2</b>", "Heading 2 (Ctrl+2)", 0},
        {"h3", "<b>H3</b>", "Heading 3 (Ctrl+3)", 0},
        {NULL, NULL, NULL, 0},
        {"bullet", NULL, "Bulleted list", FI_BULLETS},
        {"numbered", NULL, "Numbered list", FI_NUMBERS},
        {"task", NULL, "Task list", FI_TASK},
        {"quote", NULL, "Quote (Ctrl+Shift+Q)", FI_QUOTE},
        {NULL, NULL, NULL, 0},
        {"link", NULL, "Link (Ctrl+K)", FI_LINK},
        {"codeblock", "<tt><b>{}</b></tt>", "Code block (Ctrl+Shift+`)", 0},
        {"hr", NULL, "Horizontal rule", FI_HR},
    };
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "domdly-fmt");
    for (int i = 0; i < (int)G_N_ELEMENTS(items); i++) {
        if (!items[i].action) {
            gtk_box_pack_start(GTK_BOX(bar), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
            continue;
        }
        GtkWidget *b = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_widget_set_focus_on_click(b, FALSE); /* the editor keeps the caret */
        gtk_widget_set_tooltip_text(b, items[i].tip);
        char full[32];
        g_snprintf(full, sizeof full, "app.%s", items[i].action);
        gtk_actionable_set_action_name(GTK_ACTIONABLE(b), full);
        GtkWidget *child;
        if (items[i].icon) {
            child = gtk_drawing_area_new();
            gtk_widget_set_size_request(child, 18, 18);
            g_signal_connect(child, "draw", G_CALLBACK(fmt_icon_draw), GINT_TO_POINTER(items[i].icon));
        } else {
            child = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(child), items[i].markup);
            gtk_widget_set_size_request(child, 18, 18);
        }
        gtk_container_add(GTK_CONTAINER(b), child);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    }
    gtk_widget_set_no_show_all(bar, FALSE);
    return bar;
}

static void fmt_bar_toggle(void)
{
    g_fmt_bar = !g_fmt_bar;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        doc *d = nth_doc(i);
        if (d) gtk_widget_set_visible(d->fmt, g_fmt_bar);
    }
    settings_save();
}

static void on_tab_close(GtkButton *b, gpointer data)
{
    (void)b;
    close_tab((doc *)data);
}

static gboolean on_tab_middle(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    if (ev->type == GDK_BUTTON_RELEASE && ev->button == 2) {
        close_tab((doc *)data);
        return TRUE;
    }
    return FALSE;
}

static void on_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel, guint info, guint t,
                    gpointer data);

static GtkWidget *scrolled(GtkWidget *child)
{
    GtkWidget *s = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(s), child);
    return s;
}

static int offers_files(GdkDragContext *ctx)
{
    GdkAtom want = gdk_atom_intern_static_string("text/uri-list");
    for (GList *l = gdk_drag_context_list_targets(ctx); l; l = l->next)
        if (GDK_POINTER_TO_ATOM(l->data) == want) return 1;
    return 0;
}

/* A read-only text view (the Read view) turns every drop down by itself, so
   files are accepted here before it gets the chance. */
static gboolean on_drag_motion(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, guint t, gpointer data)
{
    (void)w;
    (void)x;
    (void)y;
    (void)data;
    if (!offers_files(ctx)) return FALSE;
    gdk_drag_status(ctx, GDK_ACTION_COPY, t);
    return TRUE;
}

static gboolean on_drag_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, guint t, gpointer data)
{
    (void)x;
    (void)y;
    (void)data;
    if (!offers_files(ctx)) return FALSE;
    gtk_drag_get_data(w, ctx, gdk_atom_intern_static_string("text/uri-list"), t);
    return TRUE;
}

static void accept_files(GtkWidget *w)
{
    static GtkTargetEntry targets[] = {{(gchar *)"text/uri-list", 0, 0}};
    gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(w, "drag-motion", G_CALLBACK(on_drag_motion), NULL);
    g_signal_connect(w, "drag-drop", G_CALLBACK(on_drag_drop), NULL);
    g_signal_connect(w, "drag-data-received", G_CALLBACK(on_drop), NULL);
}

static doc *new_tab(void)
{
    doc *d = g_new0(doc, 1);
    d->marks = g_ptr_array_new();
    d->mark_lines = g_array_new(FALSE, FALSE, sizeof(int));
    d->undo = g_ptr_array_new_with_free_func(undo_free);
    d->redo = g_ptr_array_new_with_free_func(undo_free);
    d->read_stale = 1;

    d->edit = gtk_text_view_new();
    d->ebuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->edit));
    gtk_style_context_add_class(gtk_widget_get_style_context(d->edit), "edit");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->edit), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(d->edit), 40);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(d->edit), 40);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(d->edit), 20);
    gtk_text_view_set_pixels_below_lines(GTK_TEXT_VIEW(d->edit), 3);

    d->read = gtk_text_view_new();
    d->rbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->read));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(d->read), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(d->read), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->read), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(d->read), READ_MARGIN);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(d->read), READ_MARGIN);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(d->read), 24);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(d->read), 40);
    g_signal_connect(d->read, "button-release-event", G_CALLBACK(on_read_click), d);

    d->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(d->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(d->stack), 120);
    gtk_stack_add_named(GTK_STACK(d->stack), scrolled(d->edit), "edit");
    gtk_stack_add_named(GTK_STACK(d->stack), scrolled(d->read), "read");
    d->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    d->fmt = fmt_bar_new();
    gtk_box_pack_start(GTK_BOX(d->page), d->fmt, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(d->page), d->stack, TRUE, TRUE, 0);
    g_object_set_data(G_OBJECT(d->page), "doc", d);
    accept_files(d->edit);
    accept_files(d->read);

    g_signal_connect(d->ebuf, "changed", G_CALLBACK(on_changed), d);
    g_signal_connect(d->ebuf, "modified-changed", G_CALLBACK(on_modified), d);
    g_signal_connect(d->ebuf, "mark-set", G_CALLBACK(on_mark_set), d);
    g_signal_connect(d->ebuf, "insert-text", G_CALLBACK(on_insert_text), d);
    g_signal_connect(d->ebuf, "delete-range", G_CALLBACK(on_delete_range), d);
    g_signal_connect(d->edit, "key-press-event", G_CALLBACK(on_edit_key), d);

    d->tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ev = gtk_event_box_new();
    d->tab_label = gtk_label_new("Untitled");
    gtk_container_add(GTK_CONTAINER(ev), d->tab_label);
    g_signal_connect(ev, "button-release-event", G_CALLBACK(on_tab_middle), d);
    GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close, "Close tab (Ctrl+W)");
    g_signal_connect(close, "clicked", G_CALLBACK(on_tab_close), d);
    gtk_box_pack_start(GTK_BOX(d->tab_box), ev, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(d->tab_box), close, FALSE, FALSE, 0);
    gtk_widget_show_all(d->tab_box);

    gtk_widget_show_all(d->page);
    gtk_widget_set_visible(d->fmt, g_fmt_bar);
    int n = gtk_notebook_append_page(GTK_NOTEBOOK(g_notebook), d->page, d->tab_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(g_notebook), d->page, TRUE);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), n);
    show_view(d, g_view);
    update_title(d);
    return d;
}

static int doc_is_blank(doc *d)
{
    return !d->path && gtk_text_buffer_get_char_count(d->ebuf) == 0 && !gtk_text_buffer_get_modified(d->ebuf);
}

static void set_doc_text(doc *d, const char *text)
{
    d->in_undo = 1;
    gtk_text_buffer_set_text(d->ebuf, text, -1);
    d->in_undo = 0;
    g_ptr_array_set_size(d->undo, 0);
    g_ptr_array_set_size(d->redo, 0);
    gtk_text_buffer_set_modified(d->ebuf, FALSE);
    GtkTextIter s;
    gtk_text_buffer_get_start_iter(d->ebuf, &s);
    gtk_text_buffer_place_cursor(d->ebuf, &s);
    d->read_stale = 1;
}

static void open_path(const char *path)
{
    char *full = g_canonicalize_filename(path, NULL);
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        doc *o = nth_doc(i);
        if (o && o->path && strcmp(o->path, full) == 0) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), i);
            g_free(full);
            return;
        }
    }
    char *data = NULL;
    gsize len = 0;
    GError *err = NULL;
    if (!g_file_get_contents(full, &data, &len, &err)) {
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                              "Could not open %s", full);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", err ? err->message : "");
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        g_clear_error(&err);
        g_free(full);
        return;
    }
    char *text = data;
    if (len >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF)
        text = data + 3;
    char *utf8 = g_utf8_validate(text, -1, NULL) ? g_strdup(text)
                                                  : g_convert(text, -1, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
    g_free(data);
    if (!utf8) utf8 = g_strdup("");
    int crlf = strstr(utf8, "\r\n") != NULL;
    if (crlf) { /* keep \n inside, restore CRLF when saving */
        char *o = utf8, *w = utf8;
        for (; *o; o++)
            if (*o != '\r') *w++ = *o;
        *w = 0;
    }
    doc *d = current_doc();
    if (!d || !doc_is_blank(d)) d = new_tab();
    g_free(d->path);
    d->path = full;
    d->crlf = crlf;
    set_doc_text(d, utf8);
    g_free(utf8);
    gtk_recent_manager_add_item(gtk_recent_manager_get_default(), g_filename_to_uri(full, NULL, NULL));
    show_view(d, g_view);
    update_title(d);
    update_status();
    tree_sync(d);
}

/* ---- saving ------------------------------------------------------------------ */

static int write_markdown(doc *d, const char *path)
{
    char *text = buffer_text(d->ebuf);
    GString *out = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (*p == '\n' && d->crlf) g_string_append_c(out, '\r');
        g_string_append_c(out, *p);
    }
    g_free(text);
    GError *err = NULL;
    int ok = g_file_set_contents(path, out->str, (gssize)out->len, &err);
    g_string_free(out, TRUE);
    if (!ok) {
        GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                              "Could not save %s", path);
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(m), "%s", err ? err->message : "");
        gtk_dialog_run(GTK_DIALOG(m));
        gtk_widget_destroy(m);
        g_clear_error(&err);
    }
    return ok;
}

static void export_html(doc *d, const char *path)
{
    char *text = buffer_text(d->ebuf);
    md_node *tree = md_parse(text, strlen(text));
    g_free(text);
    mdbuf html;
    mdbuf_init(&html);
    md_render_html_page(tree, doc_name(d), &html);
    md_free(tree);
    g_file_set_contents(path, html.data ? html.data : "", (gssize)html.len, NULL);
    mdbuf_free(&html);
}

/* PDF: the document laid out with Pango, page by page, onto a cairo PDF
 * surface (A4, always in the light colours). */
typedef struct pdf_ctx {
    cairo_t *cr;
    double y, width, margin, page_h;
    char *base_dir;
    PangoAlignment align; /* for the next paragraph (HTML align="...") */
} pdf_ctx;

static void pdf_markup(const md_node *parent, GString *o)
{
    for (const md_node *n = parent->first; n; n = n->next) {
        switch (n->type) {
        case MD_TEXT: {
            char *e = g_markup_escape_text(n->text, (gssize)n->len);
            g_string_append(o, e);
            g_free(e);
            break;
        }
        case MD_SOFTBREAK: g_string_append_c(o, ' '); break;
        case MD_LINEBREAK: g_string_append_c(o, '\n'); break;
        case MD_CODE: {
            char *e = g_markup_escape_text(n->text, (gssize)n->len);
            g_string_append_printf(o, "<span font_family=\"monospace\" background=\"#f0f2f5\">%s</span>", e);
            g_free(e);
            break;
        }
        case MD_EMPH: g_string_append(o, "<i>"); pdf_markup(n, o); g_string_append(o, "</i>"); break;
        case MD_STRONG: g_string_append(o, "<b>"); pdf_markup(n, o); g_string_append(o, "</b>"); break;
        case MD_STRIKE: g_string_append(o, "<s>"); pdf_markup(n, o); g_string_append(o, "</s>"); break;
        case MD_LINK:
            g_string_append(o, "<span foreground=\"#0969da\" underline=\"single\">");
            pdf_markup(n, o);
            g_string_append(o, "</span>");
            break;
        case MD_IMAGE: break;
        default: pdf_markup(n, o); break;
        }
    }
}

static void pdf_text(pdf_ctx *p, const char *markup, double indent, double size, double space_after)
{
    PangoLayout *l = pango_cairo_create_layout(p->cr);
    PangoFontDescription *f = pango_font_description_from_string("Sans");
    pango_font_description_set_absolute_size(f, size * PANGO_SCALE);
    pango_layout_set_font_description(l, f);
    pango_font_description_free(f);
    pango_layout_set_width(l, (int)((p->width - indent) * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_markup(l, markup, -1);
    pango_layout_set_alignment(l, p->align);
    p->align = PANGO_ALIGN_LEFT;
    int w, h;
    pango_layout_get_pixel_size(l, &w, &h);
    if (p->y + h > p->page_h - p->margin && p->y > p->margin) {
        cairo_show_page(p->cr);
        p->y = p->margin;
    }
    cairo_set_source_rgb(p->cr, 0.12, 0.14, 0.16);
    cairo_move_to(p->cr, p->margin + indent, p->y);
    pango_cairo_show_layout(p->cr, l);
    p->y += h + space_after;
    g_object_unref(l);
}

static void pdf_blocks(pdf_ctx *p, const md_node *parent, double indent, int depth);

static void pdf_block(pdf_ctx *p, const md_node *n, double indent, int depth)
{
    GString *m = g_string_new(NULL);
    switch (n->type) {
    case MD_PARAGRAPH:
        pdf_markup(n, m);
        p->align = n->align == MD_ALIGN_CENTER ? PANGO_ALIGN_CENTER : n->align == MD_ALIGN_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT;
        pdf_text(p, m->str, indent, 10.5, 8);
        break;
    case MD_HEADING: {
        static const double sz[6] = {22, 18, 15, 13, 11.5, 10.5};
        int lv = n->level < 1 ? 1 : n->level > 6 ? 6 : n->level;
        p->y += lv <= 2 ? 10 : 6;
        g_string_append(m, "<b>");
        pdf_markup(n, m);
        g_string_append(m, "</b>");
        p->align = n->align == MD_ALIGN_CENTER ? PANGO_ALIGN_CENTER : n->align == MD_ALIGN_RIGHT ? PANGO_ALIGN_RIGHT : PANGO_ALIGN_LEFT;
        pdf_text(p, m->str, indent, sz[lv - 1], 6);
        break;
    }
    case MD_CODE_BLOCK: {
        size_t len = n->len;
        while (len && n->text[len - 1] == '\n') len--;
        char *e = g_markup_escape_text(n->text ? n->text : "", (gssize)len);
        g_string_append_printf(m, "<span font_family=\"monospace\">%s</span>", e);
        g_free(e);
        pdf_text(p, m->str, indent + 12, 9.5, 10);
        break;
    }
    case MD_THEMATIC_BREAK:
        cairo_set_source_rgb(p->cr, 0.82, 0.85, 0.88);
        cairo_rectangle(p->cr, p->margin + indent, p->y + 4, p->width - indent, 1);
        cairo_fill(p->cr);
        p->y += 14;
        break;
    case MD_BLOCKQUOTE: pdf_blocks(p, n, indent + 18, depth); break;
    case MD_LIST: {
        int num = n->ordered ? n->start : 0;
        for (const md_node *it = n->first; it; it = it->next, num++) {
            GString *li = g_string_new(NULL);
            if (it->task) g_string_append(li, it->task == MD_TASK_DONE ? "☑  " : "☐  ");
            else if (n->ordered) g_string_append_printf(li, "%d.  ", num);
            else g_string_append(li, "•  ");
            const md_node *c = it->first;
            if (c && c->type == MD_PARAGRAPH) {
                pdf_markup(c, li);
                c = c->next;
            }
            pdf_text(p, li->str, indent + 14, 10.5, 3);
            g_string_free(li, TRUE);
            for (; c; c = c->next) pdf_block(p, c, indent + 28, depth + 1);
        }
        p->y += 5;
        break;
    }
    case MD_TABLE: {
        int cols = n->columns > 0 ? n->columns : 1;
        PangoTabArray *tabs = pango_tab_array_new(cols, TRUE);
        for (int i = 0; i < cols; i++) pango_tab_array_set_tab(tabs, i, PANGO_TAB_LEFT, (int)(i * (p->width - indent) / cols));
        for (const md_node *row = n->first; row; row = row->next) {
            GString *r = g_string_new(NULL);
            if (row->header) g_string_append(r, "<b>");
            for (const md_node *cell = row->first; cell; cell = cell->next) {
                pdf_markup(cell, r);
                if (cell->next) g_string_append_c(r, '\t');
            }
            if (row->header) g_string_append(r, "</b>");
            PangoLayout *l = pango_cairo_create_layout(p->cr);
            PangoFontDescription *f = pango_font_description_from_string("Sans");
            pango_font_description_set_absolute_size(f, 10 * PANGO_SCALE);
            pango_layout_set_font_description(l, f);
            pango_font_description_free(f);
            pango_layout_set_tabs(l, tabs);
            pango_layout_set_markup(l, r->str, -1);
            int w, h;
            pango_layout_get_pixel_size(l, &w, &h);
            if (p->y + h > p->page_h - p->margin) {
                cairo_show_page(p->cr);
                p->y = p->margin;
            }
            cairo_set_source_rgb(p->cr, 0.12, 0.14, 0.16);
            cairo_move_to(p->cr, p->margin + indent, p->y);
            pango_cairo_show_layout(p->cr, l);
            p->y += h + 4;
            cairo_set_source_rgb(p->cr, 0.85, 0.87, 0.9);
            cairo_rectangle(p->cr, p->margin + indent, p->y - 2, p->width - indent, 0.6);
            cairo_fill(p->cr);
            g_object_unref(l);
            g_string_free(r, TRUE);
        }
        pango_tab_array_free(tabs);
        p->y += 8;
        break;
    }
    case MD_HTML_BLOCK: {
        if (n->from_html) {
            pdf_blocks(p, n, indent, depth);
            break;
        }
        char *e = g_markup_escape_text(n->text ? n->text : "", (gssize)n->len);
        g_string_append_printf(m, "<span font_family=\"monospace\" foreground=\"#6e7781\">%s</span>", e);
        g_free(e);
        pdf_text(p, m->str, indent, 9.5, 8);
        break;
    }
    default: break;
    }
    /* pictures of a paragraph, drawn after its text */
    if (n->type == MD_PARAGRAPH)
        for (const md_node *c = n->first; c; c = c->next) {
            if (c->type != MD_IMAGE || !c->url || strstr(c->url, "://")) continue;
            char *path = g_path_is_absolute(c->url) ? g_strdup(c->url) : g_build_filename(p->base_dir, c->url, NULL);
            GdkPixbuf *pb = gdk_pixbuf_new_from_file(path, NULL);
            g_free(path);
            if (!pb) continue;
            double w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb), s = 0.75;
            if (c->width > 0 && c->width * 0.75 < w * s) s = c->width * 0.75 / w; /* width="..." from HTML */
            if (w * s > p->width - indent) s = (p->width - indent) / w;
            if (p->y + h * s > p->page_h - p->margin) {
                cairo_show_page(p->cr);
                p->y = p->margin;
            }
            cairo_save(p->cr);
            cairo_translate(p->cr, p->margin + indent, p->y);
            cairo_scale(p->cr, s, s);
            gdk_cairo_set_source_pixbuf(p->cr, pb, 0, 0);
            cairo_paint(p->cr);
            cairo_restore(p->cr);
            p->y += h * s + 10;
            g_object_unref(pb);
        }
    g_string_free(m, TRUE);
}

static void pdf_blocks(pdf_ctx *p, const md_node *parent, double indent, int depth)
{
    for (const md_node *n = parent->first; n; n = n->next) pdf_block(p, n, indent, depth);
}

static int export_pdf(doc *d, const char *path)
{
    const double W = 595.28, H = 841.89, M = 54;
    cairo_surface_t *s = cairo_pdf_surface_create(path, W, H);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return 0;
    }
    cairo_pdf_surface_set_metadata(s, CAIRO_PDF_METADATA_TITLE, doc_name(d));
    cairo_pdf_surface_set_metadata(s, CAIRO_PDF_METADATA_CREATOR, "Domdly " DOMDLY_VERSION_STR);
    pdf_ctx p;
    p.cr = cairo_create(s);
    p.margin = M;
    p.y = M;
    p.width = W - 2 * M;
    p.page_h = H;
    p.base_dir = d->path ? g_path_get_dirname(d->path) : g_get_current_dir();
    p.align = PANGO_ALIGN_LEFT;
    char *text = buffer_text(d->ebuf);
    md_node *tree = md_parse(text, strlen(text));
    md_expand_html(tree);
    g_free(text);
    pdf_blocks(&p, tree, 0, 0);
    md_free(tree);
    g_free(p.base_dir);
    cairo_destroy(p.cr);
    cairo_surface_finish(s);
    int ok = cairo_surface_status(s) == CAIRO_STATUS_SUCCESS;
    cairo_surface_destroy(s);
    return ok;
}

/* Reopens the most recently closed tab. A file that was moved or deleted
 * since is skipped (and said so in the status bar); the one before it is
 * tried instead. */
static void reopen_closed(void)
{
    char *missing = NULL;
    closed_tab *c;
    while ((c = g_queue_pop_tail(&g_closed)) != NULL) {
        int ok = 0;
        if (c->path) {
            if (g_file_test(c->path, G_FILE_TEST_IS_REGULAR)) {
                open_path(c->path);
                ok = 1;
            } else {
                g_free(missing);
                missing = g_path_get_basename(c->path);
            }
        } else if (c->text) {
            doc *d = current_doc();
            if (!d || !doc_is_blank(d)) d = new_tab();
            set_doc_text(d, c->text);
            gtk_text_buffer_set_modified(d->ebuf, TRUE); /* still unsaved */
            show_view(d, g_view);
            update_title(d);
            ok = 1;
        }
        g_free(c->path);
        g_free(c->text);
        g_free(c);
        if (ok) break;
    }
    if (missing) {
        char *msg = g_strdup_printf("%s was moved or deleted, so it could not be reopened.", missing);
        gtk_label_set_text(GTK_LABEL(g_status), msg);
        g_free(msg);
        g_free(missing);
    }
}

static GtkFileFilter *filter(const char *name, const char *pattern)
{
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_set_name(f, name);
    gtk_file_filter_add_pattern(f, pattern);
    return f;
}

/* One dialog for every format: Markdown, PDF or HTML. */
static int save_doc(doc *d, int save_as)
{
    if (!d) return 0;
    if (d->path && !save_as) {
        if (!write_markdown(d, d->path)) return 0;
        gtk_text_buffer_set_modified(d->ebuf, FALSE);
        tree_refresh();
        return 1;
    }
    GtkFileChooserNative *dlg =
        gtk_file_chooser_native_new("Save as", GTK_WINDOW(g_win), GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
    GtkFileChooser *fc = GTK_FILE_CHOOSER(dlg);
    gtk_file_chooser_set_do_overwrite_confirmation(fc, TRUE);
    GtkFileFilter *fmd = filter("Markdown (*.md)", "*.md"), *fpdf = filter("PDF document (*.pdf)", "*.pdf"),
                  *fhtml = filter("HTML page (*.html)", "*.html");
    gtk_file_chooser_add_filter(fc, fmd);
    gtk_file_chooser_add_filter(fc, fpdf);
    gtk_file_chooser_add_filter(fc, fhtml);
    char *base = g_strdup(doc_name(d));
    if (!d->path) {
        g_free(base);
        base = g_strdup("Untitled.md");
    }
    gtk_file_chooser_set_current_name(fc, base);
    g_free(base);
    if (d->path) {
        char *dir = g_path_get_dirname(d->path);
        gtk_file_chooser_set_current_folder(fc, dir);
        g_free(dir);
    }
    int ok = 0;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(fc);
        GtkFileFilter *chosen = gtk_file_chooser_get_filter(fc);
        char *final = NULL;
        if (chosen == fpdf && !g_str_has_suffix(path, ".pdf")) final = g_strconcat(path, ".pdf", NULL);
        else if (chosen == fhtml && !g_str_has_suffix(path, ".html")) final = g_strconcat(path, ".html", NULL);
        else final = g_strdup(path);
        if (g_str_has_suffix(final, ".pdf")) {
            ok = export_pdf(d, final);
        } else if (g_str_has_suffix(final, ".html") || g_str_has_suffix(final, ".htm")) {
            export_html(d, final);
            ok = 1;
        } else if ((ok = write_markdown(d, final))) {
            g_free(d->path);
            d->path = g_strdup(final);
            gtk_text_buffer_set_modified(d->ebuf, FALSE);
            d->read_stale = 1;
            update_title(d);
        }
        g_free(final);
        g_free(path);
    }
    g_object_unref(dlg);
    if (ok) {
        tree_sync(d);
        tree_refresh();
    }
    return ok;
}

/* 1 = fine to close (saved or discarded), 0 = cancelled. */
static int confirm_close(doc *d)
{
    if (!gtk_text_buffer_get_modified(d->ebuf)) return 1;
    GtkWidget *m = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          "Save changes to %s?", doc_name(d));
    gtk_dialog_add_buttons(GTK_DIALOG(m), "Do_n't Save", GTK_RESPONSE_NO, "_Cancel", GTK_RESPONSE_CANCEL, "_Save",
                           GTK_RESPONSE_YES, NULL);
    int r = gtk_dialog_run(GTK_DIALOG(m));
    gtk_widget_destroy(m);
    if (r == GTK_RESPONSE_YES) return save_doc(d, 0);
    return r == GTK_RESPONSE_NO;
}

static void close_tab(doc *d)
{
    int page = gtk_notebook_page_num(GTK_NOTEBOOK(g_notebook), d->page);
    if (page < 0) return;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), page);
    /* Closing the one empty tab that is left closes the window, like a
       browser: Ctrl+W pressed once more after the last document is gone. */
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) == 1 && !d->path &&
        gtk_text_buffer_get_char_count(d->ebuf) == 0 && !gtk_text_buffer_get_modified(d->ebuf)) {
        gtk_window_close(GTK_WINDOW(g_win));
        return;
    }
    if (!confirm_close(d)) return;
    if (d->path || gtk_text_buffer_get_char_count(d->ebuf) > 0) { /* remember it for Ctrl+Shift+T */
        closed_tab *c = g_new0(closed_tab, 1);
        if (d->path) c->path = g_strdup(d->path);
        else c->text = buffer_text(d->ebuf);
        g_queue_push_tail(&g_closed, c);
        if (g_queue_get_length(&g_closed) > CLOSED_MAX) {
            closed_tab *old = g_queue_pop_head(&g_closed);
            g_free(old->path);
            g_free(old->text);
            g_free(old);
        }
    }
    if (gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)) == 1) { /* keep one empty tab */
        g_free(d->path);
        d->path = NULL;
        d->crlf = 0;
        set_doc_text(d, "");
        update_title(d);
        show_view(d, g_view);
        return;
    }
    gtk_notebook_remove_page(GTK_NOTEBOOK(g_notebook), page);
    g_ptr_array_free(d->marks, TRUE);
    g_array_free(d->mark_lines, TRUE);
    g_ptr_array_free(d->undo, TRUE);
    g_ptr_array_free(d->redo, TRUE);
    g_free(d->path);
    g_free(d);
    settings_save();
}

/* Pictures dropped on the document are linked from it as Markdown images,
 * ![name](path), instead of being opened. */
static int is_image_name(const char *name)
{
    static const char *const ext[] = {".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp", ".svg"};
    char *low = g_ascii_strdown(name, -1);
    int hit = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(ext); i++)
        if (g_str_has_suffix(low, ext[i])) hit = 1;
    g_free(low);
    return hit;
}

/* file relative to folder dir ("img/a.png", "../pics/a.png"); the full path
   when the two share nothing but the root. */
static char *relative_path(const char *dir, const char *file)
{
    size_t i = 0, last = 0;
    while (dir[i] && file[i] && dir[i] == file[i]) {
        if (dir[i] == '/') last = i + 1;
        i++;
    }
    if (!dir[i] && file[i] == '/') last = i + 1;
    if (last <= 1) return g_strdup(file);
    GString *out = g_string_new("");
    const char *r = last > strlen(dir) ? "" : dir + last;
    if (*r) {
        g_string_append(out, "../");
        for (const char *q = r; *q; q++)
            if (*q == '/' && q[1]) g_string_append(out, "../");
    }
    g_string_append(out, file + last);
    return g_string_free(out, FALSE);
}

/* "![name](path)": relative to the document once it is saved, with spaces
   and brackets percent-encoded so that it stays a valid Markdown link. */
static void append_image_md(GString *o, const doc *d, const char *file)
{
    char *dir = d && d->path ? g_path_get_dirname(d->path) : NULL;
    char *rel = dir ? relative_path(dir, file) : g_strdup(file);
    char *name = g_path_get_basename(file);
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = 0;
    g_string_append(o, "![");
    for (const char *c = name; *c; c++) {
        if (*c == '[' || *c == ']' || *c == '\\') g_string_append_c(o, '\\');
        g_string_append_c(o, *c);
    }
    g_string_append(o, "](");
    for (const unsigned char *c = (const unsigned char *)rel; *c; c++) {
        if (*c == ' ' || *c == '(' || *c == ')' || *c == '<' || *c == '>' || *c == '%') g_string_append_printf(o, "%%%02X", *c);
        else g_string_append_c(o, (char)*c);
    }
    g_string_append_c(o, ')');
    g_free(name);
    g_free(rel);
    g_free(dir);
}

/* Inserts the links where they were dropped: at that spot of the source in
   the Edit view, or as a paragraph after the block under the pointer in the
   Read view. */
static void insert_images(doc *d, GtkWidget *w, int x, int y, const char *md)
{
    GtkTextBuffer *b = d->ebuf;
    GtkTextIter at;
    gtk_text_buffer_begin_user_action(b);
    if (w == d->edit) {
        int bx, by;
        gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(d->edit), GTK_TEXT_WINDOW_WIDGET, x, y, &bx, &by);
        gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(d->edit), &at, bx, by);
        gtk_text_buffer_insert(b, &at, md, -1);
        gtk_text_buffer_place_cursor(b, &at);
    } else {
        int next = -1; /* source line of the block after the one under the pointer */
        if (w == d->read && d->marks->len) {
            int bx, by;
            GtkTextIter it;
            gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(d->read), GTK_TEXT_WINDOW_WIDGET, x, y, &bx, &by);
            gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(d->read), &it, bx, by);
            int off = gtk_text_iter_get_offset(&it), k = -1;
            for (guint i = 0; i < d->marks->len; i++) {
                GtkTextIter m;
                gtk_text_buffer_get_iter_at_mark(d->rbuf, &m, g_ptr_array_index(d->marks, i));
                if (gtk_text_iter_get_offset(&m) <= off) k = (int)i;
            }
            if (k >= 0 && k + 1 < (int)d->mark_lines->len) next = g_array_index(d->mark_lines, int, k + 1);
        } else if (w != d->read) {
            gtk_text_buffer_get_iter_at_mark(b, &at, gtk_text_buffer_get_insert(b));
            next = gtk_text_iter_get_line(&at) + 1; /* window edge: below the caret line */
        }
        GString *t = g_string_new("");
        if (next >= 0 && next < gtk_text_buffer_get_line_count(b)) {
            gtk_text_buffer_get_iter_at_line(b, &at, next);
            for (const char *q = md; *q; q++) {
                g_string_append_c(t, *q);
                if (*q == '\n') g_string_append_c(t, '\n');
            }
            g_string_append(t, "\n\n");
        } else {
            gtk_text_buffer_get_end_iter(b, &at);
            if (!gtk_text_iter_starts_line(&at)) g_string_append_c(t, '\n');
            if (gtk_text_buffer_get_char_count(b) > 0) g_string_append_c(t, '\n');
            for (const char *q = md; *q; q++) {
                g_string_append_c(t, *q);
                if (*q == '\n') g_string_append_c(t, '\n');
            }
            g_string_append_c(t, '\n');
        }
        gtk_text_buffer_insert(b, &at, t->str, -1);
        g_string_free(t, TRUE);
    }
    gtk_text_buffer_end_user_action(b);
    if (g_view == VIEW_READ) {
        render_read(d);
        d->read_stale = 0;
    }
}

static void on_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sel, guint info, guint t,
                    gpointer data)
{
    (void)info;
    (void)data;
    /* A text view asks for the dropped data a second time on its own; the
       same drop (same context) is handled once. */
    static GdkDragContext *last;
    static guint last_t;
    if (ctx == last && t == last_t) {
        g_signal_stop_emission_by_name(w, "drag-data-received");
        return;
    }
    last = ctx;
    last_t = t;
    doc *d = current_doc();
    GString *imgs = g_string_new("");
    char **uris = gtk_selection_data_get_uris(sel);
    for (int i = 0; uris && uris[i]; i++) {
        char *path = g_filename_from_uri(uris[i], NULL, NULL);
        if (path && d && is_image_name(path)) {
            if (imgs->len) g_string_append_c(imgs, '\n');
            append_image_md(imgs, d, path);
        } else if (path) {
            open_path(path);
        }
        g_free(path);
    }
    g_strfreev(uris);
    if (imgs->len && d) insert_images(d, w, x, y, imgs->str);
    g_string_free(imgs, TRUE);
    gtk_drag_finish(ctx, TRUE, FALSE, t);
    g_signal_stop_emission_by_name(w, "drag-data-received");
}

/* ---- Files sidebar ----------------------------------------------------------- */

static int is_md_name(const char *name)
{
    static const char *const ext[] = {".md", ".markdown", ".mdown", ".mkd", ".txt"};
    char *low = g_ascii_strdown(name, -1);
    int hit = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(ext); i++)
        if (g_str_has_suffix(low, ext[i])) hit = 1;
    g_free(low);
    return hit;
}

typedef struct tree_ent {
    char *name, *key, *path;
    int dir;
} tree_ent;

static void tree_ent_free(gpointer p)
{
    tree_ent *e = (tree_ent *)p;
    g_free(e->name);
    g_free(e->key);
    g_free(e->path);
    g_free(e);
}

static gint tree_ent_cmp(gconstpointer a, gconstpointer b)
{
    const tree_ent *x = *(tree_ent *const *)a, *y = *(tree_ent *const *)b;
    if (x->dir != y->dir) return y->dir - x->dir; /* folders first */
    return strcmp(x->key, y->key);
}

/* Fills the children of `parent` (NULL = top level) with the entries of `dir`.
 * Folders get a placeholder child so the expander shows; they load on expand. */
static void tree_fill(GtkTreeIter *parent, const char *dir)
{
    GtkTreeIter c;
    while (parent && gtk_tree_model_iter_children(GTK_TREE_MODEL(g_tree_store), &c, parent))
        gtk_tree_store_remove(g_tree_store, &c);
    GDir *gd = g_dir_open(dir, 0, NULL);
    if (!gd) return;
    GPtrArray *list = g_ptr_array_new_with_free_func(tree_ent_free);
    const char *name;
    while ((name = g_dir_read_name(gd)) != NULL && list->len < TREE_MAX_ENTRIES) {
        if (name[0] == '.') continue;
        tree_ent *e = g_new0(tree_ent, 1);
        e->path = g_build_filename(dir, name, NULL);
        e->name = g_filename_display_name(name);
        e->key = g_utf8_collate_key_for_filename(e->name, -1);
        e->dir = g_file_test(e->path, G_FILE_TEST_IS_DIR);
        g_ptr_array_add(list, e);
    }
    g_dir_close(gd);
    g_ptr_array_sort(list, tree_ent_cmp);
    for (guint i = 0; i < list->len; i++) {
        tree_ent *e = g_ptr_array_index(list, i);
        GtkTreeIter it, dummy;
        gtk_tree_store_append(g_tree_store, &it, parent);
        gtk_tree_store_set(g_tree_store, &it, TC_NAME, e->name, TC_PATH, e->path, TC_DIR, e->dir, TC_MD,
                           !e->dir && is_md_name(e->name), -1);
        if (e->dir) gtk_tree_store_append(g_tree_store, &dummy, &it);
    }
    g_ptr_array_free(list, TRUE);
}

static gboolean on_tree_test_expand(GtkTreeView *v, GtkTreeIter *it, GtkTreePath *tp, gpointer data)
{
    (void)v;
    (void)tp;
    (void)data;
    GtkTreeIter c;
    GtkTreeModel *m = GTK_TREE_MODEL(g_tree_store);
    if (gtk_tree_model_iter_children(m, &c, it)) {
        char *cp = NULL;
        gtk_tree_model_get(m, &c, TC_PATH, &cp, -1);
        if (!cp) { /* still the placeholder */
            char *path = NULL;
            gtk_tree_model_get(m, it, TC_PATH, &path, -1);
            if (path) tree_fill(it, path);
            g_free(path);
        }
        g_free(cp);
    }
    return FALSE;
}

/* Finds the row of `target`; with expand set, opens the folders on the way. */
static gboolean tree_find(const char *target, GtkTreeIter *out, int expand)
{
    GtkTreeModel *m = GTK_TREE_MODEL(g_tree_store);
    GtkTreeIter it, parent;
    int have_parent = 0;
    for (;;) {
        gboolean ok = have_parent ? gtk_tree_model_iter_children(m, &it, &parent) : gtk_tree_model_get_iter_first(m, &it);
        int found = 0;
        while (ok) {
            char *p = NULL;
            gtk_tree_model_get(m, &it, TC_PATH, &p, -1);
            if (p) {
                size_t n = strlen(p);
                if (strcmp(p, target) == 0) {
                    g_free(p);
                    *out = it;
                    return TRUE;
                }
                if (strncmp(p, target, n) == 0 && target[n] == '/') found = 1;
            }
            g_free(p);
            if (found) break;
            ok = gtk_tree_model_iter_next(m, &it);
        }
        if (!found) return FALSE;
        if (expand) {
            GtkTreePath *tp = gtk_tree_model_get_path(m, &it);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(g_tree), tp, FALSE);
            gtk_tree_path_free(tp);
        }
        parent = it;
        have_parent = 1;
    }
}

/* Selects the row of `path` (the active tab's file). */
static void tree_reveal(const char *path, int expand, int scroll)
{
    if (!path || !g_tree_cur) return;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_tree));
    GtkTreeIter it;
    if (!tree_find(path, &it, expand)) {
        gtk_tree_selection_unselect_all(sel);
        return;
    }
    GtkTreePath *tp = gtk_tree_model_get_path(GTK_TREE_MODEL(g_tree_store), &it);
    gtk_tree_selection_select_path(sel, tp);
    if (scroll) gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(g_tree), tp, NULL, FALSE, 0, 0);
    gtk_tree_path_free(tp);
}

static void tree_rebuild(void)
{
    gtk_tree_store_clear(g_tree_store);
    if (g_tree_cur) {
        tree_fill(NULL, g_tree_cur);
        char *base = g_path_get_basename(g_tree_cur);
        char *disp = g_filename_display_name(base);
        gtk_button_set_label(GTK_BUTTON(g_tree_head), disp);
        gtk_widget_set_tooltip_text(g_tree_head, g_tree_cur);
        g_free(disp);
        g_free(base);
    } else {
        gtk_button_set_label(GTK_BUTTON(g_tree_head), "Files");
        gtk_widget_set_tooltip_text(g_tree_head, NULL);
    }
    GtkWidget *lbl = gtk_bin_get_child(GTK_BIN(g_tree_head));
    if (GTK_IS_LABEL(lbl)) {
        gtk_label_set_xalign(GTK_LABEL(lbl), 0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(g_tree_stack), g_tree_cur ? "tree" : "hint");
}

/* Picks the root (pin, else the active file's folder, else the previous one)
 * and shows the active file in the tree. */
static void tree_sync(const doc *d)
{
    if (!g_tree_open || !g_tree) return;
    if (!d) d = current_doc();
    char *want = NULL;
    if (g_tree_root && *g_tree_root) want = g_strdup(g_tree_root);
    else if (d && d->path) want = g_path_get_dirname(d->path);
    if (want && (!g_tree_cur || strcmp(want, g_tree_cur) != 0)) {
        g_free(g_tree_cur);
        g_tree_cur = want;
        tree_rebuild();
    } else {
        g_free(want);
    }
    if (d && d->path) tree_reveal(d->path, 1, 1);
}

static void tree_collect_expanded(GtkTreeView *v, GtkTreePath *tp, gpointer data)
{
    (void)v;
    GtkTreeIter it;
    GtkTreeModel *m = GTK_TREE_MODEL(g_tree_store);
    char *p = NULL;
    if (gtk_tree_model_get_iter(m, &it, tp)) gtk_tree_model_get(m, &it, TC_PATH, &p, -1);
    if (p) g_ptr_array_add((GPtrArray *)data, p);
}

static gint str_len_cmp(gconstpointer a, gconstpointer b)
{
    return (gint)strlen(*(char *const *)a) - (gint)strlen(*(char *const *)b);
}

/* Reloads the folders from disk and keeps the expanded ones expanded. */
static void tree_refresh(void)
{
    if (!g_tree_open || !g_tree || !g_tree_cur) return;
    GPtrArray *open = g_ptr_array_new_with_free_func(g_free);
    gtk_tree_view_map_expanded_rows(GTK_TREE_VIEW(g_tree), tree_collect_expanded, open);
    g_ptr_array_sort(open, str_len_cmp); /* parents before children */
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(gtk_widget_get_parent(g_tree)));
    double pos = gtk_adjustment_get_value(adj);
    tree_rebuild();
    for (guint i = 0; i < open->len; i++) {
        GtkTreeIter it;
        if (tree_find(g_ptr_array_index(open, i), &it, 1)) {
            GtkTreePath *tp = gtk_tree_model_get_path(GTK_TREE_MODEL(g_tree_store), &it);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(g_tree), tp, FALSE);
            gtk_tree_path_free(tp);
        }
    }
    g_ptr_array_free(open, TRUE);
    gtk_adjustment_set_value(adj, pos);
    doc *d = current_doc();
    if (d && d->path) tree_reveal(d->path, 0, 0);
}

static void tree_set_open(int open)
{
    g_tree_open = open;
    if (g_side) {
        if (open) {
            /* show_all skips a widget marked no-show-all, so lift it briefly */
            gtk_widget_set_no_show_all(g_side, FALSE);
            gtk_widget_show_all(g_side);
            gtk_widget_set_no_show_all(g_side, TRUE);
            gtk_paned_set_position(GTK_PANED(g_paned), g_tree_width);
        } else {
            gtk_widget_hide(g_side);
        }
    }
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(g_app), "tree");
    if (a) g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(open));
    if (g_tree_icon) gtk_widget_queue_draw(g_tree_icon);
    if (open) {
        tree_rebuild();
        tree_sync(NULL);
    }
    settings_save();
}

static void on_tree_state(GSimpleAction *a, GVariant *v, gpointer data)
{
    (void)a;
    (void)data;
    tree_set_open(g_variant_get_boolean(v));
}

static void on_tree_pos(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)o;
    (void)ps;
    (void)data;
    if (!g_tree_open || !g_side || !gtk_widget_get_mapped(g_side)) return;
    int pos = gtk_paned_get_position(GTK_PANED(g_paned));
    if (pos >= TREE_MIN_WIDTH) g_tree_width = pos;
}

static void on_win_active(GObject *o, GParamSpec *ps, gpointer data)
{
    (void)ps;
    (void)data;
    if (gtk_window_is_active(GTK_WINDOW(o))) tree_refresh();
}

static void tree_row_info(GtkTreePath *tp, char **path, int *dir, int *md)
{
    GtkTreeIter it;
    GtkTreeModel *m = GTK_TREE_MODEL(g_tree_store);
    gboolean d = FALSE, k = FALSE;
    *path = NULL;
    *dir = *md = 0;
    if (tp && gtk_tree_model_get_iter(m, &it, tp)) gtk_tree_model_get(m, &it, TC_PATH, path, TC_DIR, &d, TC_MD, &k, -1);
    *dir = d;
    *md = k;
}

static void tree_toggle_row(GtkTreePath *tp)
{
    if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(g_tree), tp)) gtk_tree_view_collapse_row(GTK_TREE_VIEW(g_tree), tp);
    else gtk_tree_view_expand_row(GTK_TREE_VIEW(g_tree), tp, FALSE);
}

static void tree_launch(const char *path)
{
    char *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri) g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri);
}

/* The press only records what is under the pointer (and whether the expander
 * arrow already toggled the row); the release acts on it. */
/* F5 lists the folder again, as on Windows. */
static gboolean on_tree_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (ev->keyval != GDK_KEY_F5) return FALSE;
    tree_refresh();
    return TRUE;
}

static gboolean on_tree_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)data;
    if (ev->button != 1) return FALSE;
    if (ev->type == GDK_2BUTTON_PRESS) g_tree_dbl = 1;
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    g_free(g_tree_press);
    g_tree_press = NULL;
    GtkTreePath *tp = NULL;
    if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &tp, NULL, NULL, NULL)) {
        int dir, md;
        tree_row_info(tp, &g_tree_press, &dir, &md);
        g_tree_press_exp = gtk_tree_view_row_expanded(GTK_TREE_VIEW(w), tp);
        gtk_tree_path_free(tp);
    }
    return FALSE;
}

static gboolean on_tree_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)data;
    if (ev->button != 1 || (ev->state & (GDK_SHIFT_MASK | GDK_CONTROL_MASK))) return FALSE;
    if (g_tree_dbl) { /* second click of a double click: row-activated handles it */
        g_tree_dbl = 0;
        return FALSE;
    }
    GtkTreePath *tp = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (int)ev->x, (int)ev->y, &tp, NULL, NULL, NULL)) return FALSE;
    char *path;
    int dir, md;
    tree_row_info(tp, &path, &dir, &md);
    if (path && g_tree_press && strcmp(path, g_tree_press) == 0) {
        if (dir) {
            if (gtk_tree_view_row_expanded(GTK_TREE_VIEW(w), tp) == g_tree_press_exp) tree_toggle_row(tp);
        } else if (md) {
            open_path(path);
        }
    }
    g_free(path);
    gtk_tree_path_free(tp);
    return FALSE;
}

static void on_tree_activated(GtkTreeView *v, GtkTreePath *tp, GtkTreeViewColumn *col, gpointer data)
{
    (void)v;
    (void)col;
    (void)data;
    char *path;
    int dir, md;
    tree_row_info(tp, &path, &dir, &md);
    if (path) {
        if (dir) {
            if (!g_tree_dbl) tree_toggle_row(tp); /* Enter; a double click already toggled on release */
        } else if (md) {
            open_path(path);
        } else {
            tree_launch(path);
        }
    }
    g_free(path);
}

static void tree_cell_data(GtkTreeViewColumn *col, GtkCellRenderer *r, GtkTreeModel *m, GtkTreeIter *it, gpointer data)
{
    (void)col;
    (void)data;
    gboolean dir = FALSE, md = FALSE;
    char *path = NULL;
    gtk_tree_model_get(m, it, TC_DIR, &dir, TC_MD, &md, TC_PATH, &path, -1);
    doc *d = current_doc();
    int active = path && d && d->path && strcmp(path, d->path) == 0; /* the open document is bold */
    g_object_set(r, "foreground", dir || md ? pal()->fg : pal()->muted, "weight",
                 active ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, NULL);
    g_free(path);
}

static void on_tree_menu(GtkMenuItem *item, gpointer data)
{
    (void)item;
    int which = GPOINTER_TO_INT(data);
    if (which == 0) { /* Open Folder... */
        GtkFileChooserNative *dlg = gtk_file_chooser_native_new("Open Folder", GTK_WINDOW(g_win),
                                                                GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Open", "_Cancel");
        if (g_tree_cur) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), g_tree_cur);
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            char *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
            if (dir) {
                g_free(g_tree_root);
                g_tree_root = dir;
                tree_sync(NULL);
                settings_save();
            }
        }
        g_object_unref(dlg);
    } else if (which == 1) { /* Follow Open File */
        g_free(g_tree_root);
        g_tree_root = NULL;
        tree_sync(NULL);
        settings_save();
    } else if (which == 2) {
        tree_refresh();
    } else if (which == 3 && g_tree_cur) {
        tree_launch(g_tree_cur);
    }
}

static void on_tree_head(GtkButton *b, gpointer data)
{
    (void)data;
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(g_tree_follow), !(g_tree_root && *g_tree_root));
    gtk_widget_show_all(g_tree_menu);
    gtk_menu_popup_at_widget(GTK_MENU(g_tree_menu), GTK_WIDGET(b), GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static GtkWidget *tree_menu_item(GtkWidget *menu, GtkWidget *item, int which)
{
    g_signal_connect(item, "activate", G_CALLBACK(on_tree_menu), GINT_TO_POINTER(which));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

static GtkWidget *build_sidebar(void)
{
    g_side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_side), "domdly-side");
    gtk_widget_set_size_request(g_side, TREE_MIN_WIDTH, -1);
    gtk_widget_set_no_show_all(g_side, TRUE);

    g_tree_head = gtk_button_new_with_label("Files");
    gtk_button_set_relief(GTK_BUTTON(g_tree_head), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_tree_head), "domdly-sidehead");
    g_signal_connect(g_tree_head, "clicked", G_CALLBACK(on_tree_head), NULL);
    gtk_box_pack_start(GTK_BOX(g_side), g_tree_head, FALSE, FALSE, 0);

    g_tree_menu = gtk_menu_new();
    tree_menu_item(g_tree_menu, gtk_menu_item_new_with_label("Open Folder..."), 0);
    g_tree_follow = tree_menu_item(g_tree_menu, gtk_check_menu_item_new_with_label("Follow Open File"), 1);
    tree_menu_item(g_tree_menu, gtk_menu_item_new_with_label("Refresh"), 2);
    tree_menu_item(g_tree_menu, gtk_menu_item_new_with_label("Show in File Manager"), 3);
    gtk_menu_attach_to_widget(GTK_MENU(g_tree_menu), g_tree_head, NULL);

    g_tree_store = gtk_tree_store_new(TC_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
    g_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_tree_store));
    gtk_style_context_add_class(gtk_widget_get_style_context(g_tree), "domdly-tree");
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(g_tree), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(g_tree), FALSE);
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    g_object_set(r, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 1, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes("Name", r, "text", TC_NAME, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_column_set_cell_data_func(col, r, tree_cell_data, NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_tree), col);
    g_signal_connect(g_tree, "test-expand-row", G_CALLBACK(on_tree_test_expand), NULL);
    g_signal_connect(g_tree, "button-press-event", G_CALLBACK(on_tree_press), NULL);
    g_signal_connect(g_tree, "button-release-event", G_CALLBACK(on_tree_release), NULL);
    g_signal_connect(g_tree, "row-activated", G_CALLBACK(on_tree_activated), NULL);
    g_signal_connect(g_tree, "key-press-event", G_CALLBACK(on_tree_key), NULL);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sw), g_tree);
    GtkWidget *hint = gtk_label_new("Open a file or choose a folder");
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_margin_start(hint, 12);
    gtk_widget_set_margin_end(hint, 12);
    gtk_widget_set_margin_top(hint, 12);
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_widget_set_valign(hint, GTK_ALIGN_START);
    g_tree_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(g_tree_stack), sw, "tree");
    gtk_stack_add_named(GTK_STACK(g_tree_stack), hint, "hint");
    gtk_box_pack_start(GTK_BOX(g_side), g_tree_stack, TRUE, TRUE, 0);
    return g_side;
}

/* Sidebar button: a panel outline with a left column (filled while open). */
static gboolean draw_tree_icon(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    gdk_cairo_set_source_rgba(cr, &fg);
    double cx = gtk_widget_get_allocated_width(w) / 2.0, cy = gtk_widget_get_allocated_height(w) / 2.0;
    double x0 = cx - 8, y0 = cy - 6.5, bw = 16, bh = 13, col = 5.5;
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_rectangle(cr, x0, y0, bw, bh);
    cairo_stroke(cr);
    if (g_tree_open) {
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.45);
        cairo_rectangle(cr, x0, y0, col, bh);
        cairo_fill(cr);
        gdk_cairo_set_source_rgba(cr, &fg);
    }
    cairo_move_to(cr, x0 + col, y0);
    cairo_line_to(cr, x0 + col, y0 + bh);
    cairo_stroke(cr);
    return TRUE;
}

/* ---- theme button: sun, half sun and half moon, moon (drawn with cairo) --- */

static gboolean draw_theme(GtkWidget *w, cairo_t *cr, gpointer data)
{
    (void)data;
    GtkStyleContext *sc = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    gtk_style_context_get_color(sc, gtk_style_context_get_state(sc), &fg);
    gdk_cairo_set_source_rgba(cr, &fg);
    double cx = gtk_widget_get_allocated_width(w) / 2.0, cy = gtk_widget_get_allocated_height(w) / 2.0, s = 18;
    int kind = g_theme == THEME_NIGHT ? 2 : is_dark() ? 1 : 0;
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    if (kind == 0) { /* sun */
        cairo_arc(cr, cx, cy, s * 0.22, 0, 2 * G_PI);
        cairo_fill(cr);
        for (int i = 0; i < 8; i++) {
            double a = i * G_PI / 4;
            cairo_move_to(cr, cx + cos(a) * s * 0.36, cy + sin(a) * s * 0.36);
            cairo_line_to(cr, cx + cos(a) * s * 0.47, cy + sin(a) * s * 0.47);
        }
        cairo_stroke(cr);
    } else if (kind == 1) { /* half sun, half moon */
        cairo_arc(cr, cx - 1, cy, s * 0.25, G_PI / 2, 3 * G_PI / 2);
        cairo_close_path(cr);
        cairo_fill(cr);
        for (int i = 0; i < 3; i++) {
            double a = G_PI * (0.75 + 0.25 * i);
            cairo_move_to(cr, cx - 1 + cos(a) * s * 0.36, cy + sin(a) * s * 0.36);
            cairo_line_to(cr, cx - 1 + cos(a) * s * 0.47, cy + sin(a) * s * 0.47);
        }
        cairo_stroke(cr);
        cairo_set_line_width(cr, s * 0.14);
        cairo_arc(cr, cx + 1, cy, s * 0.29, -G_PI / 2, G_PI / 2);
        cairo_stroke(cr);
    } else { /* moon */
        cairo_push_group(cr);
        cairo_arc(cr, cx, cy, s * 0.42, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
        cairo_arc(cr, cx + s * 0.22, cy - s * 0.18, s * 0.34, 0, 2 * G_PI);
        cairo_fill(cr);
        cairo_pop_group_to_source(cr);
        cairo_paint(cr);
    }
    return TRUE;
}

static void refresh_all(void)
{
    apply_css();
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        doc *d = nth_doc(i);
        if (!d) continue;
        d->read_stale = 1;
        if (g_view == VIEW_READ && d == current_doc()) render_read(d);
    }
    gtk_widget_queue_draw(g_theme_btn);
    if (g_tree) gtk_widget_queue_draw(g_tree);
    if (g_tree_icon) gtk_widget_queue_draw(g_tree_icon);
    update_status();
}

static void set_theme(int t)
{
    g_theme = t;
    const char *tip = t == THEME_NIGHT ? "Night theme; click for Light (Ctrl+Shift+L)"
                      : is_dark()      ? "Dark navy theme; click for Night (Ctrl+Shift+L)"
                                       : "Light theme; click for Dark navy (Ctrl+Shift+L)";
    gtk_widget_set_tooltip_text(g_theme_btn, tip);
    refresh_all();
    settings_save();
}

static void next_theme(void) { set_theme(g_theme == THEME_NIGHT ? THEME_LIGHT : is_dark() ? THEME_NIGHT : THEME_DARK); }

static void set_zoom(int z)
{
    g_zoom = z < 50 ? 50 : z > 300 ? 300 : z;
    refresh_all();
    settings_save();
}

/* ---- find ------------------------------------------------------------------- */

static void find_next(void)
{
    doc *d = current_doc();
    const char *what = gtk_entry_get_text(GTK_ENTRY(g_search_entry));
    if (!d || !*what) return;
    GtkTextBuffer *b = g_view == VIEW_READ ? d->rbuf : d->ebuf;
    GtkTextView *v = GTK_TEXT_VIEW(g_view == VIEW_READ ? d->read : d->edit);
    GtkTextIter from, ms, me;
    gtk_text_buffer_get_selection_bounds(b, NULL, &from);
    if (!gtk_text_iter_forward_search(&from, what, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &ms, &me, NULL)) {
        gtk_text_buffer_get_start_iter(b, &from);
        if (!gtk_text_iter_forward_search(&from, what, GTK_TEXT_SEARCH_CASE_INSENSITIVE, &ms, &me, NULL)) return;
    }
    gtk_text_buffer_select_range(b, &ms, &me);
    gtk_text_view_scroll_to_iter(v, &ms, 0.1, FALSE, 0, 0);
}

/* ---- update check (manual only; never downloads or runs anything) --------------- */

typedef struct update_result {
    int status;     /* HTTP status, 0 on any transport failure */
    char *tag;      /* tag_name value, may be NULL */
    char *url;      /* html_url value, may be NULL */
} update_result;

static int g_update_busy;

static void open_uri(const char *uri)
{
    GError *err = NULL;
    gtk_show_uri_on_window(GTK_WINDOW(g_win), uri, GDK_CURRENT_TIME, &err);
    if (err) g_error_free(err);
}

/* Find the string value of "key" in a raw buffer. Returns a newly allocated string or NULL. */
static char *json_string_value(const char *buf, const char *key)
{
    char *pat = g_strdup_printf("\"%s\"", key);
    const char *p = strstr(buf, pat);
    g_free(pat);
    if (!p) return NULL;
    p += strlen(key) + 2;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return NULL;
    p++;
    const char *e = p;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1]) e++;
        e++;
    }
    if (*e != '"' || e - p > 512) return NULL;
    return g_strndup(p, (gsize)(e - p));
}

static void update_result_free(gpointer p)
{
    update_result *r = p;
    g_free(r->tag);
    g_free(r->url);
    g_free(r);
}

static void update_thread(GTask *task, gpointer src, gpointer data, GCancellable *cancel)
{
    (void)src;
    (void)data;
    (void)cancel;
    update_result *res = g_new0(update_result, 1);
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_tls(client, TRUE);
    g_socket_client_set_timeout(client, 15);
    GError *err = NULL;
    GSocketConnection *conn = g_socket_client_connect_to_host(client, "api.github.com", 443, NULL, &err);
    if (conn) {
        char *req = g_strdup_printf("GET /repos/dogukansahil/Domdly/releases/latest HTTP/1.1\r\n"
                                    "Host: api.github.com\r\nUser-Agent: Domdly/%s\r\n"
                                    "Accept: application/vnd.github+json\r\nConnection: close\r\n\r\n",
                                    DOMDLY_VERSION_STR);
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
        GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
        gboolean ok = g_output_stream_write_all(out, req, strlen(req), NULL, NULL, NULL);
        g_free(req);
        GString *resp = g_string_new(NULL);
        if (ok) {
            char chunk[4096];
            while (resp->len < 1024 * 1024) {
                gssize n = g_input_stream_read(in, chunk, sizeof chunk, NULL, NULL);
                if (n <= 0) break;
                g_string_append_len(resp, chunk, n);
            }
        }
        if (resp->len > 12 && !strncmp(resp->str, "HTTP/1.", 7)) {
            res->status = atoi(resp->str + 9);
            const char *body = strstr(resp->str, "\r\n\r\n");
            if (res->status == 200 && body) {
                res->tag = json_string_value(body, "tag_name");
                res->url = json_string_value(body, "html_url");
            }
        }
        g_string_free(resp, TRUE);
        g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
        g_object_unref(conn);
    }
    if (err) g_error_free(err);
    g_object_unref(client);
    g_task_return_pointer(task, res, update_result_free);
}

/* Parse "X.Y.Z" (optional leading v); returns 1 on success. */
static int parse_version(const char *s, int v[3])
{
    if (*s == 'v' || *s == 'V') s++;
    return sscanf(s, "%d.%d.%d", &v[0], &v[1], &v[2]) == 3;
}

static void update_done(GObject *src, GAsyncResult *ar, gpointer data)
{
    (void)src;
    (void)data;
    update_result *r = g_task_propagate_pointer(G_TASK(ar), NULL);
    g_update_busy = 0;
    if (!r) return;
    int cur[3] = {DOMDLY_VERSION_MAJOR, DOMDLY_VERSION_MINOR, DOMDLY_VERSION_PATCH}, nv[3] = {0, 0, 0};
    const char *msg = NULL;
    char *text = NULL;
    const char *b1 = NULL, *b2 = NULL;
    int kind;                                    /* 0 = newer, 1 = current, 2 = no release, 3 = failure */
    if (r->status == 404) {
        kind = 2;
    } else if (r->status == 200 && r->tag && parse_version(r->tag, nv)) {
        kind = (nv[0] > cur[0] || (nv[0] == cur[0] && (nv[1] > cur[1] || (nv[1] == cur[1] && nv[2] > cur[2])))) ? 0 : 1;
    } else {
        kind = 3;
    }
    if (kind == 0) {
        text = g_strdup_printf("Domdly %d.%d.%d is available. You have %s.", nv[0], nv[1], nv[2], DOMDLY_VERSION_STR);
        b1 = "Open Download Page";
        b2 = "Later";
    } else if (kind == 1) {
        text = g_strdup_printf("You have the latest version (%s).", DOMDLY_VERSION_STR);
    } else if (kind == 2) {
        msg = "No release has been published on GitHub yet.";
    } else {
        msg = "Could not reach GitHub.";
        b1 = "Open Releases Page";
        b2 = "Close";
    }
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(g_win), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                            kind == 3 ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO,
                                            b1 ? GTK_BUTTONS_NONE : GTK_BUTTONS_OK, "%s", text ? text : msg);
    gtk_window_set_title(GTK_WINDOW(dlg), "Check for Updates");
    if (b1) {
        gtk_dialog_add_button(GTK_DIALOG(dlg), b2, GTK_RESPONSE_CLOSE);
        gtk_dialog_add_button(GTK_DIALOG(dlg), b1, GTK_RESPONSE_ACCEPT);
        gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    }
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        static const char prefix[] = "https://github.com/dogukansahil/Domdly/";
        if (kind == 0 && r->url && g_str_has_prefix(r->url, prefix)) open_uri(r->url);
        else open_uri(DOMDLY_URL_STR "/releases");
    }
    gtk_widget_destroy(dlg);
    g_free(text);
    update_result_free(r);
}

static void check_for_updates(void)
{
    if (g_update_busy) return;
    g_update_busy = 1;
    GTask *task = g_task_new(NULL, NULL, update_done, NULL);
    g_task_run_in_thread(task, update_thread);
    g_object_unref(task);
}

static gboolean on_ver_click(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (ev->button == 1) open_uri(DOMDLY_URL_STR);
    return TRUE;
}

static gboolean on_ver_cross(GtkWidget *w, GdkEventCrossing *ev, gpointer data)
{
    (void)data;
    GdkWindow *win = gtk_widget_get_window(w);
    if (!win) return FALSE;
    GdkCursor *c = ev->type == GDK_ENTER_NOTIFY ? gdk_cursor_new_from_name(gdk_window_get_display(win), "pointer") : NULL;
    gdk_window_set_cursor(win, c);
    if (c) g_object_unref(c);
    return FALSE;
}

/* ---- actions and shortcuts ---------------------------------------------------- */

static void act(GSimpleAction *a, GVariant *p, gpointer data)
{
    (void)p;
    (void)data;
    const char *name = g_action_get_name(G_ACTION(a));
    doc *d = current_doc();
    if (!strcmp(name, "new")) {
        new_tab();
        set_view(VIEW_EDIT);
    } else if (!strcmp(name, "open")) {
        GtkFileChooserNative *dlg = gtk_file_chooser_native_new("Open", GTK_WINDOW(g_win), GTK_FILE_CHOOSER_ACTION_OPEN,
                                                                "_Open", "_Cancel");
        gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);
        GtkFileFilter *f = filter("Markdown (*.md)", "*.md");
        gtk_file_filter_add_pattern(f, "*.markdown");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), f);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter("All files", "*"));
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
            for (GSList *l = files; l; l = l->next) open_path(l->data);
            g_slist_free_full(files, g_free);
        }
        g_object_unref(dlg);
    } else if (!strcmp(name, "save")) {
        save_doc(d, 0);
    } else if (!strcmp(name, "saveas")) {
        save_doc(d, 1);
    } else if (!strcmp(name, "pdf")) {
        if (!d) return;
        GtkFileChooserNative *dlg = gtk_file_chooser_native_new("Export as PDF", GTK_WINDOW(g_win),
                                                                GTK_FILE_CHOOSER_ACTION_SAVE, "_Export", "_Cancel");
        gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
        char *nm = g_strdup(doc_name(d));
        char *dot = strrchr(nm, '.');
        if (dot) *dot = 0;
        char *pdf = g_strconcat(nm, ".pdf", NULL);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), pdf);
        g_free(nm);
        g_free(pdf);
        if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
            char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
            export_pdf(d, path);
            g_free(path);
        }
        g_object_unref(dlg);
    } else if (format_command(name)) {
    } else if (!strcmp(name, "fmtbar")) {
        fmt_bar_toggle();
    } else if (!strcmp(name, "reopen")) {
        reopen_closed();
    } else if (!strcmp(name, "close")) {
        if (d) close_tab(d);
    } else if (!strcmp(name, "nexttab") || !strcmp(name, "prevtab")) {
        int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook)), c = gtk_notebook_get_current_page(GTK_NOTEBOOK(g_notebook));
        gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), (c + (name[0] == 'n' ? 1 : n - 1)) % n);
    } else if (!strcmp(name, "toggle")) {
        set_view(g_view == VIEW_READ ? VIEW_EDIT : VIEW_READ);
    } else if (!strcmp(name, "theme")) {
        next_theme();
    } else if (!strcmp(name, "zoomin")) {
        set_zoom(g_zoom + 10);
    } else if (!strcmp(name, "zoomout")) {
        set_zoom(g_zoom - 10);
    } else if (!strcmp(name, "zoomreset")) {
        set_zoom(100);
    } else if (!strcmp(name, "undo") || !strcmp(name, "redo")) {
        if (d && g_view == VIEW_EDIT) undo_redo(d, name[0] == 'r');
    } else if (!strcmp(name, "find")) {
        gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(g_search_bar), TRUE);
        gtk_widget_grab_focus(g_search_entry);
    } else if (!strcmp(name, "findnext")) {
        find_next();
    } else if (!strcmp(name, "about")) {
        GdkPixbuf *logo = g_object_get_data(G_OBJECT(g_win), "logo");
        gtk_show_about_dialog(GTK_WINDOW(g_win), "program-name", "Domdly", "version", DOMDLY_VERSION_STR, "comments",
                              "A small, fast Markdown reader and editor written in plain C.", "website",
                              DOMDLY_URL_STR, "copyright", DOMDLY_COPYRIGHT, "license-type", GTK_LICENSE_GPL_3_0_ONLY,
                              "logo", logo, NULL);
    } else if (!strcmp(name, "update")) {
        check_for_updates();
    } else if (!strcmp(name, "quit")) {
        gtk_window_close(GTK_WINDOW(g_win));
    }
}

static void add_action(const char *name, const char *accel1, const char *accel2)
{
    GSimpleAction *a = g_simple_action_new(name, NULL);
    g_signal_connect(a, "activate", G_CALLBACK(act), NULL);
    g_action_map_add_action(G_ACTION_MAP(g_app), G_ACTION(a));
    g_object_unref(a);
    char full[64];
    g_snprintf(full, sizeof full, "app.%s", name);
    const char *accels[3] = {accel1, accel2, NULL};
    if (accel1) gtk_application_set_accels_for_action(g_app, full, accels);
}

static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint n, gpointer data)
{
    (void)nb;
    (void)n;
    (void)data;
    doc *d = (doc *)g_object_get_data(G_OBJECT(page), "doc");
    if (!d) return;
    show_view(d, g_view);
    update_title(d);
    update_status();
    tree_sync(d);
}

static gboolean on_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (!(ev->state & GDK_CONTROL_MASK)) return FALSE;
    double dy = 0;
    if (ev->direction == GDK_SCROLL_UP) dy = -1;
    else if (ev->direction == GDK_SCROLL_DOWN) dy = 1;
    else if (ev->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas((GdkEvent *)ev, NULL, &dy);
    if (dy < 0) set_zoom(g_zoom + 10);
    else if (dy > 0) set_zoom(g_zoom - 10);
    return TRUE;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer data)
{
    (void)w;
    (void)e;
    (void)data;
    int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(g_notebook));
    for (int i = 0; i < n; i++) {
        doc *d = nth_doc(i);
        if (d && gtk_text_buffer_get_modified(d->ebuf)) {
            gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), i);
            if (!confirm_close(d)) return TRUE;
        }
    }
    settings_save();
    return FALSE;
}

static GdkPixbuf *make_logo(int size)
{
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, size, size);
    guchar *px = gdk_pixbuf_get_pixels(pb);
    int stride = gdk_pixbuf_get_rowstride(pb);
    const int ss = 4;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            double bg = 0, fg = 0;
            for (int sy = 0; sy < ss; sy++)
                for (int sx = 0; sx < ss; sx++) {
                    double u = (x + (sx + 0.5) / ss) / size, v = (y + (sy + 0.5) / ss) / size;
                    if (sd_round_rect(u, v, 0.5, 0.5, 0.47, 0.47, icon_radius) <= 0) {
                        bg += 1;
                        if (in_mark(u, v)) fg += 1;
                    }
                }
            bg /= ss * ss;
            fg /= ss * ss;
            double t = (double)y / (size - 1), k = bg > 0 ? fg / bg : 0;
            guchar *p = px + y * stride + x * 4;
            for (int ch = 0; ch < 3; ch++) {
                double c = icon_top[ch] + (icon_bottom[ch] - icon_top[ch]) * t;
                p[ch] = (guchar)((c * (1 - k) + k) * 255);
            }
            p[3] = (guchar)(bg * 255);
        }
    return pb;
}

static GtkWidget *bar_button(const char *label, const char *tip)
{
    GtkWidget *b = gtk_toggle_button_new_with_label(label);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "domdly-seg");
    gtk_widget_set_tooltip_text(b, tip);
    return b;
}

static void on_activate(GtkApplication *app, gpointer data);

static void build_window(GtkApplication *app)
{
    g_win = gtk_application_window_new(app);
    gtk_window_set_default_size(GTK_WINDOW(g_win), 1100, 780);
    GdkPixbuf *logo = make_logo(128);
    gtk_window_set_icon(GTK_WINDOW(g_win), logo);
    g_object_set_data_full(G_OBJECT(g_win), "logo", logo, g_object_unref);

    g_header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(g_header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(g_header), "Domdly");
    gtk_window_set_titlebar(GTK_WINDOW(g_win), g_header);

    GMenu *menu = g_menu_new();
    GMenu *s1 = g_menu_new(), *s2 = g_menu_new(), *s3 = g_menu_new();
    g_menu_append(s1, "New Tab", "app.new");
    g_menu_append(s1, "Open...", "app.open");
    g_menu_append(s1, "Save", "app.save");
    g_menu_append(s1, "Save As (Markdown, PDF, HTML)...", "app.saveas");
    g_menu_append(s1, "Export as PDF...", "app.pdf");
    g_menu_append(s1, "Close Tab", "app.close");
    g_menu_append(s1, "Reopen Closed Tab", "app.reopen");
    g_menu_append(s2, "Files Sidebar", "app.tree");
    g_menu_append(s2, "Formatting Toolbar", "app.fmtbar");
    g_menu_append(s2, "Switch Edit / Read", "app.toggle");
    g_menu_append(s2, "Find", "app.find");
    g_menu_append(s2, "Next Theme", "app.theme");
    g_menu_append(s2, "Zoom In", "app.zoomin");
    g_menu_append(s2, "Zoom Out", "app.zoomout");
    g_menu_append(s2, "Reset Zoom", "app.zoomreset");
    g_menu_append(s3, "About Domdly", "app.about");
    g_menu_append(s3, "Check for Updates...", "app.update");
    g_menu_append(s3, "Quit", "app.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s1));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s2));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(s3));
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(mb), G_MENU_MODEL(menu));
    gtk_button_set_image(GTK_BUTTON(mb), gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(mb, "Menu (F10)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(g_header), mb);
    GtkWidget *tb = gtk_button_new();
    g_tree_icon = gtk_drawing_area_new();
    gtk_widget_set_size_request(g_tree_icon, 22, 22);
    g_signal_connect(g_tree_icon, "draw", G_CALLBACK(draw_tree_icon), NULL);
    gtk_container_add(GTK_CONTAINER(tb), g_tree_icon);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(tb), "app.tree");
    gtk_widget_set_tooltip_text(tb, "Files sidebar (Ctrl+Shift+B)");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(g_header), tb);
    GtkWidget *save = gtk_button_new_with_label("Save as");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(save), "app.saveas");
    gtk_widget_set_tooltip_text(save, "Save a copy as Markdown, PDF or HTML (Ctrl+Shift+S)");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(g_header), save);

    /* small muted version link, left of "Save as" */
    GtkWidget *ver_box = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(ver_box), FALSE);
    gtk_widget_add_events(ver_box, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK);
    GtkWidget *ver = gtk_label_new("v" DOMDLY_VERSION_STR);
    gtk_container_add(GTK_CONTAINER(ver_box), ver);
    GtkCssProvider *vp = gtk_css_provider_new();
    gtk_css_provider_load_from_data(vp, "label { font-size: 85%; opacity: 0.65; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(ver), GTK_STYLE_PROVIDER(vp),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(vp);
    gtk_widget_set_tooltip_text(ver_box, "Open Domdly on GitHub");
    g_signal_connect(ver_box, "button-press-event", G_CALLBACK(on_ver_click), NULL);
    g_signal_connect(ver_box, "enter-notify-event", G_CALLBACK(on_ver_cross), NULL);
    g_signal_connect(ver_box, "leave-notify-event", G_CALLBACK(on_ver_cross), NULL);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(g_header), ver_box);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_search_bar = gtk_search_bar_new();
    g_search_entry = gtk_search_entry_new();
    gtk_container_add(GTK_CONTAINER(g_search_bar), g_search_entry);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(g_search_bar), GTK_ENTRY(g_search_entry));
    g_signal_connect_swapped(g_search_entry, "activate", G_CALLBACK(find_next), NULL);
    gtk_box_pack_start(GTK_BOX(box), g_search_bar, FALSE, FALSE, 0);

    g_notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(g_notebook), TRUE);
    gtk_notebook_set_show_border(GTK_NOTEBOOK(g_notebook), FALSE);
    GtkWidget *plus = gtk_button_new_from_icon_name("list-add-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(plus), GTK_RELIEF_NONE);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(plus), "app.new");
    gtk_widget_set_tooltip_text(plus, "New tab (Ctrl+T)");
    gtk_widget_show(plus);
    gtk_notebook_set_action_widget(GTK_NOTEBOOK(g_notebook), plus, GTK_PACK_END);
    g_signal_connect(g_notebook, "switch-page", G_CALLBACK(on_switch_page), NULL);
    gtk_box_pack_start(GTK_BOX(box), g_notebook, TRUE, TRUE, 0);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), "domdly-bar");
    g_path_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_path_label), 0);
    gtk_label_set_ellipsize(GTK_LABEL(g_path_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(g_path_label), 60);
    gtk_widget_show(g_path_label);
    g_path_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(g_path_btn), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(g_path_btn), g_path_label);
    gtk_widget_set_tooltip_text(g_path_btn, "Show this file in its folder");
    gtk_style_context_add_class(gtk_widget_get_style_context(g_path_btn), "dim-label");
    gtk_widget_set_no_show_all(g_path_btn, TRUE); /* shown only while the tab has a file */
    g_signal_connect(g_path_btn, "clicked", G_CALLBACK(on_path_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), g_path_btn, FALSE, TRUE, 0);
    g_status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(g_status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(g_status), PANGO_ELLIPSIZE_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_status), "dim-label");
    gtk_box_pack_start(GTK_BOX(bar), g_status, TRUE, TRUE, 4);
    g_zoom_label = gtk_label_new("100%");
    gtk_widget_set_tooltip_text(g_zoom_label, "Zoom: Ctrl+Plus / Ctrl+Minus / Ctrl+0 (or Ctrl+wheel)");
    gtk_box_pack_start(GTK_BOX(bar), g_zoom_label, FALSE, FALSE, 4);
    GtkWidget *seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(seg), "linked");
    g_btn_edit = bar_button("Edit", "Edit the Markdown source (Ctrl+E)");
    g_btn_read = bar_button("Read", "Read the formatted document (Ctrl+E)");
    g_signal_connect(g_btn_edit, "toggled", G_CALLBACK(on_view_toggled), NULL);
    g_signal_connect(g_btn_read, "toggled", G_CALLBACK(on_view_toggled), NULL);
    gtk_box_pack_start(GTK_BOX(seg), g_btn_edit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(seg), g_btn_read, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), seg, FALSE, FALSE, 4);
    g_theme_btn = gtk_button_new();
    GtkWidget *icon = gtk_drawing_area_new();
    gtk_widget_set_size_request(icon, 22, 22);
    g_signal_connect(icon, "draw", G_CALLBACK(draw_theme), NULL);
    gtk_container_add(GTK_CONTAINER(g_theme_btn), icon);
    gtk_button_set_relief(GTK_BUTTON(g_theme_btn), GTK_RELIEF_NONE);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(g_theme_btn), "app.theme");
    gtk_box_pack_start(GTK_BOX(bar), g_theme_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);

    g_paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_pack1(GTK_PANED(g_paned), build_sidebar(), FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(g_paned), box, TRUE, TRUE);
    g_signal_connect(g_paned, "notify::position", G_CALLBACK(on_tree_pos), NULL);
    g_signal_connect(g_win, "notify::is-active", G_CALLBACK(on_win_active), NULL);
    gtk_container_add(GTK_CONTAINER(g_win), g_paned);
    accept_files(g_win);
    g_signal_connect(g_win, "delete-event", G_CALLBACK(on_delete), NULL);
    g_signal_connect(g_win, "scroll-event", G_CALLBACK(on_scroll), NULL);
    gtk_widget_add_events(g_win, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    gtk_widget_show_all(g_win);
}

static void on_startup(GApplication *app, gpointer data)
{
    (void)app;
    (void)data;
    g_css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(g_css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    add_action("new", "<Primary>t", "<Primary>n");
    add_action("open", "<Primary>o", NULL);
    add_action("save", "<Primary>s", NULL);
    add_action("saveas", "<Primary><Shift>s", NULL);
    add_action("pdf", "<Primary><Shift>p", NULL);
    add_action("close", "<Primary>w", NULL);
    add_action("reopen", "<Primary><Shift>t", NULL);
    add_action("nexttab", "<Primary>Tab", "<Primary>Page_Down");
    add_action("prevtab", "<Primary><Shift>ISO_Left_Tab", "<Primary>Page_Up");
    add_action("toggle", "<Primary>e", NULL);
    GSimpleAction *ta = g_simple_action_new_stateful("tree", NULL, g_variant_new_boolean(FALSE));
    g_signal_connect(ta, "change-state", G_CALLBACK(on_tree_state), NULL);
    g_action_map_add_action(G_ACTION_MAP(g_app), G_ACTION(ta));
    g_object_unref(ta);
    const char *tree_accels[] = {"<Primary><Shift>b", NULL};
    gtk_application_set_accels_for_action(g_app, "app.tree", tree_accels);
    add_action("theme", "<Primary><Shift>l", NULL);
    add_action("bold", "<Primary>b", NULL);
    add_action("italic", "<Primary>i", NULL);
    add_action("strike", "<Primary><Shift>x", NULL);
    add_action("code", "<Primary>grave", NULL);
    add_action("link", "<Primary>k", NULL);
    add_action("h1", "<Primary>1", NULL);
    add_action("h2", "<Primary>2", NULL);
    add_action("h3", "<Primary>3", NULL);
    add_action("quote", "<Primary><Shift>q", NULL);
    add_action("bullet", NULL, NULL);
    add_action("numbered", NULL, NULL);
    add_action("task", NULL, NULL);
    add_action("codeblock", "<Primary><Shift>asciitilde", NULL);
    add_action("hr", NULL, NULL);
    add_action("fmtbar", NULL, NULL);
    add_action("zoomin", "<Primary>plus", "<Primary>equal");
    add_action("zoomout", "<Primary>minus", NULL);
    add_action("zoomreset", "<Primary>0", NULL);
    add_action("undo", "<Primary>z", NULL);
    add_action("redo", "<Primary>y", "<Primary><Shift>z");
    add_action("find", "<Primary>f", NULL);
    add_action("findnext", "F3", NULL);
    add_action("about", NULL, NULL);
    add_action("update", NULL, NULL);
    add_action("quit", "<Primary>q", NULL);

    g_settings_path = g_build_filename(g_get_user_config_dir(), "domdly", "settings.ini", NULL);
}

static void restore_or_blank(void)
{
    GKeyFile *k = g_key_file_new();
    int restored = 0;
    if (g_key_file_load_from_file(k, g_settings_path, G_KEY_FILE_NONE, NULL)) {
        g_view = g_key_file_get_integer(k, "view", "mode", NULL) == VIEW_READ ? VIEW_READ : VIEW_EDIT;
        g_theme = CLAMP(g_key_file_get_integer(k, "view", "theme", NULL), 0, 3);
        int z = g_key_file_get_integer(k, "view", "zoom", NULL);
        g_zoom = z >= 50 && z <= 300 ? z : 100;
        g_tree_open = g_key_file_get_boolean(k, "view", "tree_open", NULL);
        if (g_key_file_has_key(k, "view", "format_bar", NULL))
            g_fmt_bar = g_key_file_get_boolean(k, "view", "format_bar", NULL);
        int tw = g_key_file_get_integer(k, "view", "tree_width", NULL);
        g_tree_width = tw >= TREE_MIN_WIDTH && tw <= 1200 ? tw : 240;
        char *tr = g_key_file_get_string(k, "view", "tree_root", NULL);
        g_free(g_tree_root);
        g_tree_root = tr && *tr ? tr : NULL;
        if (!g_tree_root) g_free(tr);
        gsize n = 0;
        char **tabs = g_key_file_get_string_list(k, "session", "tabs", &n, NULL);
        int active = g_key_file_get_integer(k, "session", "active", NULL);
        for (gsize i = 0; tabs && i < n; i++)
            if (g_file_test(tabs[i], G_FILE_TEST_IS_REGULAR)) {
                open_path(tabs[i]);
                restored++;
            }
        if (restored && active >= 0 && active < restored) gtk_notebook_set_current_page(GTK_NOTEBOOK(g_notebook), active);
        g_strfreev(tabs);
    }
    g_key_file_free(k);
    if (!restored) new_tab();
    tree_set_open(g_tree_open);
}

static void on_activate(GtkApplication *app, gpointer data)
{
    (void)data;
    if (g_win) {
        gtk_window_present(GTK_WINDOW(g_win));
        return;
    }
    build_window(app);
    restore_or_blank();
    set_theme(g_theme);
    set_view(g_view);
    update_title(current_doc()); /* the restored active tab, not the last one opened */
}

/* Files given on the command line, or by a second start: new tabs. */
static void on_open(GApplication *app, GFile **files, gint n, const gchar *hint, gpointer data)
{
    (void)hint;
    (void)data;
    if (!g_win) {
        build_window(GTK_APPLICATION(app));
        restore_or_blank();
        set_theme(g_theme);
    }
    for (int i = 0; i < n; i++) {
        char *path = g_file_get_path(files[i]);
        if (path) open_path(path);
        g_free(path);
    }
    set_view(g_view);
    update_title(current_doc());
    gtk_window_present(GTK_WINDOW(g_win));
}

int main(int argc, char **argv)
{
    /* domdly file.md --pdf out.pdf: convert without a window */
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--pdf")) {
            const char *in = NULL;
            for (int j = 1; j < argc; j++)
                if (j != i && j != i + 1 && argv[j][0] != '-') in = argv[j];
            if (!in) return 2;
            char *data = NULL;
            if (!g_file_get_contents(in, &data, NULL, NULL)) return 1;
            doc d;
            memset(&d, 0, sizeof d);
            gtk_init(&argc, &argv);
            d.ebuf = gtk_text_buffer_new(NULL);
            gtk_text_buffer_set_text(d.ebuf, data, -1);
            d.path = g_canonicalize_filename(in, NULL);
            g_free(data);
            return export_pdf(&d, argv[i + 1]) ? 0 : 1;
        }
    g_set_prgname("domdly");
    g_set_application_name("Domdly");
    g_app = gtk_application_new("io.github.dogukansahil.Domdly", G_APPLICATION_HANDLES_OPEN);
    g_signal_connect(g_app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(g_app, "activate", G_CALLBACK(on_activate), NULL);
    g_signal_connect(g_app, "open", G_CALLBACK(on_open), NULL);
    int status = g_application_run(G_APPLICATION(g_app), argc, argv);
    g_object_unref(g_app);
    return status;
}
