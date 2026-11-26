/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "render_html.h"

#include <string.h>

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (lower((unsigned char)*s) != *prefix) return 0;
        s++;
        prefix++;
    }
    return 1;
}

int md_url_is_safe(const char *url, int is_image)
{
    const char *p = url;
    while (*p == ' ' || *p == '\t') p++;
    if (starts_with_ci(p, "javascript:") || starts_with_ci(p, "vbscript:") || starts_with_ci(p, "file:"))
        return 0;
    if (starts_with_ci(p, "data:")) {
        if (!is_image) return 0;
        return starts_with_ci(p, "data:image/png") || starts_with_ci(p, "data:image/gif") ||
               starts_with_ci(p, "data:image/jpeg") || starts_with_ci(p, "data:image/webp");
    }
    return 1;
}

static void esc(mdbuf *o, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        switch (c) {
        case '&': mdbuf_puts(o, "&amp;"); break;
        case '<': mdbuf_puts(o, "&lt;"); break;
        case '>': mdbuf_puts(o, "&gt;"); break;
        case '"': mdbuf_puts(o, "&quot;"); break;
        default: mdbuf_putc(o, c); break;
        }
    }
}

static void esc_url(mdbuf *o, const char *url)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        unsigned char c = *p;
        if (c == '&') mdbuf_puts(o, "&amp;");
        else if (c == '"') mdbuf_puts(o, "%22");
        else if (c == '<') mdbuf_puts(o, "%3C");
        else if (c == '>') mdbuf_puts(o, "%3E");
        else if (c <= ' ' || c == '\\' || c == '`' || c >= 0x7F) {
            mdbuf_putc(o, '%');
            mdbuf_putc(o, hex[c >> 4]);
            mdbuf_putc(o, hex[c & 15]);
        } else {
            mdbuf_putc(o, (char)c);
        }
    }
}

static void render_inlines(const md_node *parent, mdbuf *o);

static void render_inline(const md_node *n, mdbuf *o)
{
    switch (n->type) {
    case MD_TEXT:
        esc(o, n->text, n->len);
        break;
    case MD_SOFTBREAK:
        mdbuf_putc(o, '\n');
        break;
    case MD_LINEBREAK:
        mdbuf_puts(o, "<br />\n");
        break;
    case MD_CODE:
        mdbuf_puts(o, "<code>");
        esc(o, n->text, n->len);
        mdbuf_puts(o, "</code>");
        break;
    case MD_EMPH:
        mdbuf_puts(o, "<em>");
        render_inlines(n, o);
        mdbuf_puts(o, "</em>");
        break;
    case MD_STRONG:
        mdbuf_puts(o, "<strong>");
        render_inlines(n, o);
        mdbuf_puts(o, "</strong>");
        break;
    case MD_STRIKE:
        mdbuf_puts(o, "<del>");
        render_inlines(n, o);
        mdbuf_puts(o, "</del>");
        break;
    case MD_LINK:
        mdbuf_puts(o, "<a href=\"");
        if (md_url_is_safe(n->url, 0)) esc_url(o, n->url);
        mdbuf_putc(o, '"');
        if (n->title) {
            mdbuf_puts(o, " title=\"");
            esc(o, n->title, strlen(n->title));
            mdbuf_putc(o, '"');
        }
        mdbuf_putc(o, '>');
        render_inlines(n, o);
        mdbuf_puts(o, "</a>");
        break;
    case MD_IMAGE: {
        mdbuf alt;
        mdbuf_init(&alt);
        md_plain_text(n, &alt);
        mdbuf_puts(o, "<img src=\"");
        if (md_url_is_safe(n->url, 1)) esc_url(o, n->url);
        mdbuf_puts(o, "\" alt=\"");
        esc(o, alt.data ? alt.data : "", alt.len);
        mdbuf_putc(o, '"');
        if (n->title) {
            mdbuf_puts(o, " title=\"");
            esc(o, n->title, strlen(n->title));
            mdbuf_putc(o, '"');
        }
        mdbuf_puts(o, " />");
        mdbuf_free(&alt);
        break;
    }
    case MD_HTML_INLINE:
        mdbuf_put(o, n->text, n->len);
        break;
    default:
        render_inlines(n, o);
        break;
    }
}

static void render_inlines(const md_node *parent, mdbuf *o)
{
    for (const md_node *c = parent->first; c; c = c->next) render_inline(c, o);
}

static const char *align_attr(md_align a)
{
    switch (a) {
    case MD_ALIGN_LEFT: return " style=\"text-align:left\"";
    case MD_ALIGN_CENTER: return " style=\"text-align:center\"";
    case MD_ALIGN_RIGHT: return " style=\"text-align:right\"";
    default: return "";
    }
}

static void render_block(const md_node *n, mdbuf *o, int tight);

static void render_children(const md_node *n, mdbuf *o, int tight)
{
    for (const md_node *c = n->first; c; c = c->next) render_block(c, o, tight);
}

static void render_block(const md_node *n, mdbuf *o, int tight)
{
    switch (n->type) {
    case MD_DOCUMENT:
        render_children(n, o, 0);
        break;
    case MD_PARAGRAPH:
        if (tight) {
            render_inlines(n, o);
            if (n->next) mdbuf_putc(o, '\n');
        } else {
            mdbuf_puts(o, "<p>");
            render_inlines(n, o);
            mdbuf_puts(o, "</p>\n");
        }
        break;
    case MD_HEADING: {
        mdbuf slug;
        mdbuf_init(&slug);
        md_heading_slug(n, &slug);
        mdbuf_printf(o, "<h%d", n->level);
        if (slug.len) {
            mdbuf_puts(o, " id=\"");
            esc(o, slug.data, slug.len);
            mdbuf_putc(o, '"');
        }
        mdbuf_putc(o, '>');
        render_inlines(n, o);
        mdbuf_printf(o, "</h%d>\n", n->level);
        mdbuf_free(&slug);
        break;
    }
    case MD_BLOCKQUOTE:
        mdbuf_puts(o, "<blockquote>\n");
        render_children(n, o, 0);
        mdbuf_puts(o, "</blockquote>\n");
        break;
    case MD_LIST: {
        int has_task = 0;
        for (const md_node *it = n->first; it; it = it->next)
            if (it->task) has_task = 1;
        if (n->ordered) {
            if (n->start != 1) mdbuf_printf(o, "<ol start=\"%d\">\n", n->start);
            else mdbuf_puts(o, "<ol>\n");
        } else {
            mdbuf_puts(o, has_task ? "<ul class=\"task-list\">\n" : "<ul>\n");
        }
        for (const md_node *it = n->first; it; it = it->next) {
            if (it->task) {
                mdbuf_puts(o, "<li class=\"task-list-item\"><input type=\"checkbox\" disabled");
                if (it->task == MD_TASK_DONE) mdbuf_puts(o, " checked");
                mdbuf_puts(o, " /> ");
            } else {
                mdbuf_puts(o, "<li>");
            }
            if (!n->tight && it->first) mdbuf_putc(o, '\n');
            for (const md_node *c = it->first; c; c = c->next) {
                if (n->tight && c->type != MD_PARAGRAPH && c->prev && c->prev->type == MD_PARAGRAPH)
                    ; /* newline already written after the paragraph */
                else if (n->tight && c->type != MD_PARAGRAPH && !c->prev)
                    mdbuf_putc(o, '\n');
                render_block(c, o, n->tight);
            }
            mdbuf_puts(o, "</li>\n");
        }
        mdbuf_puts(o, n->ordered ? "</ol>\n" : "</ul>\n");
        break;
    }
    case MD_CODE_BLOCK: {
        mdbuf_puts(o, "<pre><code");
        if (n->info && *n->info) {
            size_t k = 0;
            while (n->info[k] && n->info[k] != ' ') k++;
            mdbuf_puts(o, " class=\"language-");
            esc(o, n->info, k);
            mdbuf_putc(o, '"');
        }
        mdbuf_putc(o, '>');
        esc(o, n->text, n->len);
        mdbuf_puts(o, "</code></pre>\n");
        break;
    }
    case MD_HTML_BLOCK:
        mdbuf_put(o, n->text, n->len);
        break;
    case MD_THEMATIC_BREAK:
        mdbuf_puts(o, "<hr />\n");
        break;
    case MD_TABLE: {
        mdbuf_puts(o, "<table>\n");
        int body_open = 0;
        for (const md_node *row = n->first; row; row = row->next) {
            if (row->header) mdbuf_puts(o, "<thead>\n");
            else if (!body_open) {
                mdbuf_puts(o, "<tbody>\n");
                body_open = 1;
            }
            mdbuf_puts(o, "<tr>\n");
            for (const md_node *cell = row->first; cell; cell = cell->next) {
                const char *tag = row->header ? "th" : "td";
                mdbuf_printf(o, "<%s%s>", tag, align_attr(cell->align));
                render_inlines(cell, o);
                mdbuf_printf(o, "</%s>\n", tag);
            }
            mdbuf_puts(o, "</tr>\n");
            if (row->header) mdbuf_puts(o, "</thead>\n");
        }
        if (body_open) mdbuf_puts(o, "</tbody>\n");
        mdbuf_puts(o, "</table>\n");
        break;
    }
    default:
        break;
    }
}

void md_render_html(const md_node *doc, mdbuf *out)
{
    render_block(doc, out, 0);
}

static const char page_css[] =
    ":root{color-scheme:light dark;--fg:#1f2328;--bg:#ffffff;--muted:#59636e;--border:#d1d9e0;"
    "--code-bg:#f2f4f7;--link:#0969da}"
    "@media (prefers-color-scheme:dark){:root{--fg:#e6edf3;--bg:#16181d;--muted:#9198a1;"
    "--border:#3d444d;--code-bg:#262b33;--link:#4493f8}}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.6 system-ui,-apple-system,"
    "\"Segoe UI\",Roboto,\"Noto Sans\",sans-serif}"
    "main{max-width:860px;margin:0 auto;padding:32px 20px 64px}"
    "h1,h2,h3,h4,h5,h6{line-height:1.25;margin:1.5em 0 .6em}"
    "h1,h2{padding-bottom:.3em;border-bottom:1px solid var(--border)}"
    "h1:first-child,h2:first-child{margin-top:0}"
    "a{color:var(--link)}"
    "code,pre{font-family:ui-monospace,\"Cascadia Mono\",Consolas,\"DejaVu Sans Mono\",monospace;"
    "font-size:.9em}"
    "code{background:var(--code-bg);padding:.15em .35em;border-radius:5px}"
    "pre{background:var(--code-bg);padding:14px 16px;border-radius:8px;overflow:auto;line-height:1.45}"
    "pre code{background:none;padding:0}"
    "blockquote{margin:0 0 1em;padding:0 1em;color:var(--muted);border-left:4px solid var(--border)}"
    "table{border-collapse:collapse;display:block;overflow:auto;margin:0 0 1em}"
    "th,td{border:1px solid var(--border);padding:6px 13px}"
    "th{background:var(--code-bg)}"
    "img{max-width:100%}"
    "hr{border:0;border-top:1px solid var(--border);margin:24px 0}"
    "ul.task-list{list-style:none;padding-left:1.2em}"
    "li.task-list-item input{margin:0 .4em 0 -1.2em}";

void md_render_html_page(const md_node *doc, const char *title, mdbuf *out)
{
    mdbuf_puts(out, "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\" />\n"
                    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />\n"
                    "<meta name=\"generator\" content=\"Domdly\" />\n"
                    /* HTML from the document is kept as written; scripts, plugins and
                     * frames in it are blocked when the page is opened in a browser */
                    "<meta http-equiv=\"Content-Security-Policy\" content=\"script-src 'none'; object-src 'none'; "
                    "frame-src 'none'; base-uri 'none'; form-action 'none'\" />\n<title>");
    esc(out, title ? title : "", title ? strlen(title) : 0);
    mdbuf_puts(out, "</title>\n<style>");
    mdbuf_puts(out, page_css);
    mdbuf_puts(out, "</style>\n</head>\n<body>\n<main>\n");
    md_render_html(doc, out);
    mdbuf_puts(out, "</main>\n</body>\n</html>\n");
}
