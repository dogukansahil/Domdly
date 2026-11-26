/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "md_write.h"

#include <string.h>

#define EMPH (MD_RUN_BOLD | MD_RUN_ITALIC | MD_RUN_STRIKE)

/* Nesting order of the open markers, outermost first. */
static const unsigned order[4] = {MD_RUN_LINK, MD_RUN_STRIKE, MD_RUN_BOLD, MD_RUN_ITALIC};

typedef struct open_t {
    unsigned kind;
    const char *url;
} open_t;

typedef struct writer {
    mdbuf *o;
    int table_cell;
    const char *break_prefix;
    int line_start; /* at the start of a line: block syntax must be escaped */
    char prev;      /* last character written (ASCII view) */
    open_t st[4];
    int depth;
} writer;

static int is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (unsigned char)c >= 0x80;
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static void put(writer *w, const char *s, size_t n)
{
    if (!n) return;
    mdbuf_put(w->o, s, n);
    w->prev = s[n - 1];
}

static void puts_(writer *w, const char *s) { put(w, s, strlen(s)); }

/* "&name;" or "&#123;" would be read as an entity. */
static int looks_like_entity(const char *s, size_t n, size_t i)
{
    size_t j = i + 1;
    if (j < n && s[j] == '#') j++;
    size_t start = j;
    while (j < n && j - start < 32 && is_alnum(s[j]) && (unsigned char)s[j] < 0x80) j++;
    return j > start && j < n && s[j] == ';';
}

static void write_text(writer *w, const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i], next = i + 1 < n ? s[i + 1] : 0;
        char prev = i > 0 ? s[i - 1] : w->prev;
        if (w->line_start) {
            if (c == ' ' || c == '\t') continue; /* leading blanks would change the block */
            w->line_start = 0;
            if (c == '#' || c == '>' || ((c == '-' || c == '+') && (next == ' ' || next == 0 || next == c)) ||
                (c == '=' && (next == '=' || next == 0))) {
                put(w, "\\", 1);
                put(w, &c, 1);
                continue;
            }
            if (c >= '0' && c <= '9') {
                size_t j = i;
                while (j < n && s[j] >= '0' && s[j] <= '9') j++;
                if (j < n && (s[j] == '.' || s[j] == ')') && (j + 1 == n || s[j + 1] == ' ')) {
                    put(w, s + i, j - i);
                    put(w, "\\", 1);
                    put(w, s + j, 1);
                    i = j;
                    continue;
                }
            }
        }
        int esc = 0;
        switch (c) {
        case '\\':
        case '`':
        case '*':
        case '[':
        case ']':
            esc = 1;
            break;
        case '_': esc = !(is_alnum(prev) && is_alnum(next)); break;
        case '<': esc = is_alpha(next) || next == '/' || next == '!' || next == '?'; break;
        case '~': esc = next == '~' || prev == '~'; break;
        case '&': esc = looks_like_entity(s, n, i); break;
        case '|': esc = w->table_cell; break;
        default: break;
        }
        if (esc) put(w, "\\", 1);
        put(w, &c, 1);
    }
}

static int url_needs_brackets(const char *u)
{
    int depth = 0;
    if (!*u) return 1;
    for (const char *p = u; *p; p++) {
        if ((unsigned char)*p <= ' ' || *p == '<' || *p == '>') return 1;
        if (*p == '(') depth++;
        if (*p == ')' && --depth < 0) return 1;
    }
    return depth != 0;
}

static void write_url(writer *w, const char *u)
{
    if (!url_needs_brackets(u)) {
        puts_(w, u);
        return;
    }
    put(w, "<", 1);
    for (const char *p = u; *p; p++) {
        if (*p == '<' || *p == '>') put(w, "\\", 1);
        put(w, p, 1);
    }
    put(w, ">", 1);
}

static void open_marker(writer *w, unsigned kind, const char *url)
{
    if (kind == MD_RUN_LINK) puts_(w, "[");
    else if (kind == MD_RUN_STRIKE) puts_(w, "~~");
    else if (kind == MD_RUN_BOLD) puts_(w, "**");
    else puts_(w, "*");
    w->st[w->depth].kind = kind;
    w->st[w->depth].url = url;
    w->depth++;
    w->line_start = 0;
}

static void close_top(writer *w)
{
    open_t t = w->st[--w->depth];
    if (t.kind == MD_RUN_LINK) {
        puts_(w, "](");
        write_url(w, t.url ? t.url : "");
        puts_(w, ")");
    } else if (t.kind == MD_RUN_STRIKE) {
        puts_(w, "~~");
    } else if (t.kind == MD_RUN_BOLD) {
        puts_(w, "**");
    } else {
        puts_(w, "*");
    }
}

/* Makes the open markers match the style of the next run. */
static void set_style(writer *w, unsigned flags, const char *url)
{
    open_t want[4];
    int nw = 0;
    for (int k = 0; k < 4; k++)
        if (flags & order[k]) {
            want[nw].kind = order[k];
            want[nw].url = order[k] == MD_RUN_LINK ? url : NULL;
            nw++;
        }
    int same = 0;
    while (same < w->depth && same < nw && w->st[same].kind == want[same].kind &&
           (want[same].kind != MD_RUN_LINK ||
            (w->st[same].url && want[same].url && strcmp(w->st[same].url, want[same].url) == 0)))
        same++;
    while (w->depth > same) close_top(w);
    for (int k = same; k < nw; k++) open_marker(w, want[k].kind, want[k].url);
}

static void write_code(writer *w, const md_run *runs, size_t from, size_t to)
{
    size_t longest = 0, cur = 0, total = 0;
    for (size_t r = from; r < to; r++)
        for (size_t i = 0; i < runs[r].len; i++) {
            cur = runs[r].text[i] == '`' ? cur + 1 : 0;
            if (cur > longest) longest = cur;
            total++;
        }
    if (!total) return;
    char first = runs[from].len ? runs[from].text[0] : 0;
    char last = 0;
    for (size_t r = to; r > from; r--)
        if (runs[r - 1].len) {
            last = runs[r - 1].text[runs[r - 1].len - 1];
            break;
        }
    int pad = first == '`' || last == '`' || first == ' ' || last == ' ';
    for (size_t k = 0; k <= longest; k++) put(w, "`", 1);
    if (pad) put(w, " ", 1);
    for (size_t r = from; r < to; r++)
        for (size_t i = 0; i < runs[r].len; i++) {
            if (runs[r].text[i] == '|' && w->table_cell) put(w, "\\", 1);
            put(w, runs[r].text + i, 1);
        }
    if (pad) put(w, " ", 1);
    for (size_t k = 0; k <= longest; k++) put(w, "`", 1);
    w->line_start = 0;
}

static int is_autolink_target(const char *u)
{
    return strstr(u, "://") != NULL || strncmp(u, "mailto:", 7) == 0;
}

/* Emphasis markers cannot hug spaces, so blanks at an edge where emphasis
 * starts (or ends) are written outside of it. prev and next are the styles
 * of the neighbouring runs. */
static void write_run(writer *w, const md_run *r, unsigned prev, unsigned next)
{
    const char *s = r->text;
    size_t a = 0, b = r->len;
    unsigned style = r->flags & (EMPH | MD_RUN_LINK);
    unsigned opening = style & EMPH & ~prev, closing = style & EMPH & ~next;
    if (opening)
        while (a < b && s[a] == ' ') a++;
    if (closing)
        while (b > a && s[b - 1] == ' ') b--;
    if (a > 0) {
        set_style(w, style & ~opening, r->url);
        write_text(w, s, a);
    }
    if (b > a) {
        set_style(w, style, r->url);
        write_text(w, s + a, b - a);
    }
    if (b < r->len && b > a) {
        set_style(w, style & ~closing, r->url);
        write_text(w, s + b, r->len - b);
    }
}

void md_write_inline(const md_run *runs, size_t n, int table_cell, const char *break_prefix, int close, mdbuf *out)
{
    writer w;
    memset(&w, 0, sizeof w);
    w.o = out;
    w.table_cell = table_cell;
    w.break_prefix = break_prefix;
    w.line_start = 1;
    w.prev = ' ';

    size_t last_content = 0; /* a hard break at the very end is dropped */
    for (size_t i = 0; i < n; i++)
        if (!(runs[i].flags & MD_RUN_BREAK) && runs[i].len) last_content = i + 1;

    size_t i = 0;
    while (i < n) {
        const md_run *r = &runs[i];
        if (r->flags & MD_RUN_BREAK) {
            if (i < last_content) {
                puts_(&w, "\\\n");
                if (break_prefix) puts_(&w, break_prefix);
                w.line_start = 1;
                w.prev = ' ';
            }
            i++;
            continue;
        }
        if (!r->len) {
            i++;
            continue;
        }
        if ((r->flags & MD_RUN_LINK) && r->url && !(r->flags & (EMPH | MD_RUN_CODE)) &&
            is_autolink_target(r->url) && strlen(r->url) == r->len && memcmp(r->url, r->text, r->len) == 0 &&
            (i + 1 >= n || !(runs[i + 1].flags & MD_RUN_LINK) || !runs[i + 1].url ||
             strcmp(runs[i + 1].url, r->url) != 0)) {
            /* the text is the address itself: an autolink */
            set_style(&w, 0, NULL);
            put(&w, "<", 1);
            puts_(&w, r->url);
            put(&w, ">", 1);
            w.line_start = 0;
            i++;
            continue;
        }
        if (r->flags & MD_RUN_CODE) {
            size_t j = i;
            while (j < n && (runs[j].flags & MD_RUN_CODE) && !(runs[j].flags & MD_RUN_BREAK)) j++;
            set_style(&w, r->flags & (EMPH | MD_RUN_LINK), r->url);
            write_code(&w, runs, i, j);
            i = j;
            continue;
        }
        unsigned prev = i > 0 && !(runs[i - 1].flags & MD_RUN_BREAK) ? runs[i - 1].flags : 0;
        unsigned next = i + 1 < n && !(runs[i + 1].flags & MD_RUN_BREAK) ? runs[i + 1].flags : 0;
        write_run(&w, r, prev & (EMPH | MD_RUN_LINK), next & (EMPH | MD_RUN_LINK));
        i++;
    }
    if (close)
        while (w.depth) close_top(&w);
}
