/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Markdown parser.
 *
 * Block structure is parsed with a "gather and recurse" strategy: container
 * blocks (block quotes and list items) collect their lines, strip their
 * prefix and parse the result recursively. Line numbers are kept 1:1 so each
 * block knows the source line where it starts. Inline content is parsed in a
 * second pass (so link reference definitions anywhere in the document work)
 * using the CommonMark delimiter-stack algorithm for emphasis and links.
 */
#include "markdown.h"
#include "mdbuf.h"

#include <stdlib.h>
#include <string.h>

#define MD_MAX_DEPTH   48
#define MD_MAX_COLUMNS 64

/* ------------------------------------------------------------------------ */
/* Character classes (ASCII only, locale independent)                       */
/* ------------------------------------------------------------------------ */

static int c_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
static int c_digit(int c) { return c >= '0' && c <= '9'; }
static int c_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int c_alnum(int c) { return c_alpha(c) || c_digit(c); }
static int c_xdigit(int c) { return c_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static int c_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int c_punct(int c)
{
    return (c >= 33 && c <= 47) || (c >= 58 && c <= 64) || (c >= 91 && c <= 96) || (c >= 123 && c <= 126);
}

/* ------------------------------------------------------------------------ */
/* Nodes                                                                    */
/* ------------------------------------------------------------------------ */

static md_node *node_new(md_type type)
{
    md_node *n = (md_node *)md_xcalloc(1, sizeof *n);
    n->type = type;
    return n;
}

static md_node *node_text(md_type type, const char *s, size_t len)
{
    md_node *n = node_new(type);
    n->text = md_xstrndup(s, len);
    n->len = len;
    return n;
}

static void node_append(md_node *parent, md_node *child)
{
    child->parent = parent;
    child->next = NULL;
    child->prev = parent->last;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static void node_unlink(md_node *n)
{
    if (n->prev) n->prev->next = n->next;
    else if (n->parent) n->parent->first = n->next;
    if (n->next) n->next->prev = n->prev;
    else if (n->parent) n->parent->last = n->prev;
    n->prev = n->next = n->parent = NULL;
}

static void node_insert_after(md_node *ref, md_node *n)
{
    n->parent = ref->parent;
    n->prev = ref;
    n->next = ref->next;
    if (ref->next) ref->next->prev = n;
    else if (ref->parent) ref->parent->last = n;
    ref->next = n;
}

void md_free(md_node *node)
{
    if (!node) return;
    md_node *c = node->first;
    while (c) {
        md_node *next = c->next;
        md_free(c);
        c = next;
    }
    free(node->text);
    free(node->url);
    free(node->title);
    free(node->info);
    free(node);
}

void md_plain_text(const md_node *node, mdbuf *out)
{
    for (const md_node *c = node->first; c; c = c->next) {
        switch (c->type) {
        case MD_TEXT:
        case MD_CODE:
            mdbuf_put(out, c->text, c->len);
            break;
        case MD_SOFTBREAK:
        case MD_LINEBREAK:
            mdbuf_putc(out, ' ');
            break;
        case MD_HTML_INLINE:
            break;
        default:
            md_plain_text(c, out);
            break;
        }
    }
}

void md_heading_slug(const md_node *heading, mdbuf *out)
{
    mdbuf t;
    mdbuf_init(&t);
    md_plain_text(heading, &t);
    for (size_t i = 0; i < t.len; i++) {
        unsigned char c = (unsigned char)t.data[i];
        if (c >= 0x80 || c_alnum(c) || c == '-' || c == '_') mdbuf_putc(out, (char)c_lower(c));
        else if (c == ' ') mdbuf_putc(out, '-');
    }
    mdbuf_free(&t);
}

/* ------------------------------------------------------------------------ */
/* Parser state and link reference definitions                              */
/* ------------------------------------------------------------------------ */

typedef struct md_ref {
    char *label;
    char *url;
    char *title;
} md_ref;

typedef struct md_parser {
    md_ref *refs;
    size_t  nrefs, caprefs;
} md_parser;

/* Lower-cases ASCII and collapses whitespace, as CommonMark label matching. */
static char *normalize_label(const char *s, size_t n)
{
    mdbuf b;
    mdbuf_init(&b);
    int pending_space = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c_space(c)) {
            pending_space = 1;
            continue;
        }
        if (pending_space && b.len) mdbuf_putc(&b, ' ');
        pending_space = 0;
        mdbuf_putc(&b, (char)c_lower(c));
    }
    return mdbuf_detach(&b);
}

/* Removes backslash escapes before ASCII punctuation. */
static char *unescape(const char *s, size_t n)
{
    mdbuf b;
    mdbuf_init(&b);
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && c_punct((unsigned char)s[i + 1])) i++;
        mdbuf_putc(&b, s[i]);
    }
    return mdbuf_detach(&b);
}

static const md_ref *ref_find(const md_parser *P, const char *label, size_t n)
{
    char *key = normalize_label(label, n);
    const md_ref *found = NULL;
    if (*key) {
        for (size_t i = 0; i < P->nrefs; i++) {
            if (strcmp(P->refs[i].label, key) == 0) {
                found = &P->refs[i];
                break;
            }
        }
    }
    free(key);
    return found;
}

static void ref_add(md_parser *P, const char *label, size_t ln, const char *url, size_t un,
                    const char *title, size_t tn, int has_title)
{
    if (ref_find(P, label, ln)) return; /* first definition wins */
    if (P->nrefs == P->caprefs) {
        P->caprefs = P->caprefs ? P->caprefs * 2 : 8;
        P->refs = (md_ref *)md_xrealloc(P->refs, P->caprefs * sizeof *P->refs);
    }
    md_ref *r = &P->refs[P->nrefs++];
    r->label = normalize_label(label, ln);
    r->url = unescape(url, un);
    r->title = has_title ? unescape(title, tn) : NULL;
}

/* ------------------------------------------------------------------------ */
/* Lines                                                                    */
/* ------------------------------------------------------------------------ */

typedef struct line_t {
    const char *p;
    size_t n;
} line_t;

typedef struct lines_t {
    line_t *v;
    size_t n, cap;
} lines_t;

static void lines_split(lines_t *L, const char *s, size_t n)
{
    L->v = NULL;
    L->n = L->cap = 0;
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && s[j] != '\n') j++;
        if (L->n == L->cap) {
            L->cap = L->cap ? L->cap * 2 : 64;
            L->v = (line_t *)md_xrealloc(L->v, L->cap * sizeof *L->v);
        }
        L->v[L->n].p = s + i;
        L->v[L->n].n = j - i;
        L->n++;
        i = j + 1;
    }
}

static size_t ln_indent(line_t l)
{
    size_t i = 0;
    while (i < l.n && l.p[i] == ' ') i++;
    return i;
}

static int ln_blank(line_t l)
{
    for (size_t i = 0; i < l.n; i++)
        if (l.p[i] != ' ' && l.p[i] != '\t') return 0;
    return 1;
}

static line_t ln_from(line_t l, size_t k)
{
    if (k > l.n) k = l.n;
    line_t r;
    r.p = l.p + k;
    r.n = l.n - k;
    return r;
}

static line_t ln_trim(line_t l)
{
    while (l.n && l.p[0] == ' ') { l.p++; l.n--; }
    while (l.n && l.p[l.n - 1] == ' ') l.n--;
    return l;
}

static line_t ln_make(const char *p, size_t n)
{
    line_t l;
    l.p = p;
    l.n = n;
    return l;
}

/* ------------------------------------------------------------------------ */
/* Block start detection                                                    */
/* ------------------------------------------------------------------------ */

static int det_atx(line_t l, int *level, line_t *content)
{
    size_t i = ln_indent(l);
    if (i > 3) return 0;
    size_t k = 0;
    while (i + k < l.n && l.p[i + k] == '#') k++;
    if (k < 1 || k > 6) return 0;
    size_t p = i + k;
    if (p < l.n && l.p[p] != ' ') return 0;
    line_t c = ln_trim(ln_from(l, p));
    size_t h = c.n;
    while (h > 0 && c.p[h - 1] == '#') h--;
    if (h < c.n) {
        if (h == 0) c.n = 0;
        else if (c.p[h - 1] == ' ') {
            c.n = h;
            c = ln_trim(c);
        }
    }
    *level = (int)k;
    *content = c;
    return 1;
}

static int det_hr(line_t l)
{
    size_t i = ln_indent(l);
    if (i > 3) return 0;
    char ch = 0;
    int count = 0;
    for (; i < l.n; i++) {
        char c = l.p[i];
        if (c == ' ') continue;
        if (c != '-' && c != '*' && c != '_') return 0;
        if (!ch) ch = c;
        else if (c != ch) return 0;
        count++;
    }
    return count >= 3;
}

static int det_fence(line_t l, char *fc, size_t *flen, size_t *findent, line_t *info)
{
    size_t i = ln_indent(l);
    if (i > 3 || i >= l.n) return 0;
    char c = l.p[i];
    if (c != '`' && c != '~') return 0;
    size_t k = 0;
    while (i + k < l.n && l.p[i + k] == c) k++;
    if (k < 3) return 0;
    line_t inf = ln_trim(ln_from(l, i + k));
    if (c == '`') {
        for (size_t j = 0; j < inf.n; j++)
            if (inf.p[j] == '`') return 0;
    }
    *fc = c;
    *flen = k;
    *findent = i;
    *info = inf;
    return 1;
}

static int det_fence_close(line_t l, char fc, size_t flen)
{
    size_t i = ln_indent(l);
    if (i > 3) return 0;
    size_t k = 0;
    while (i + k < l.n && l.p[i + k] == fc) k++;
    if (k < flen) return 0;
    return ln_blank(ln_from(l, i + k));
}

static int det_quote(line_t l)
{
    size_t i = ln_indent(l);
    return i <= 3 && i < l.n && l.p[i] == '>';
}

typedef struct listmark {
    int ordered;
    int start;
    char marker;
    size_t content; /* column where item content starts */
    int empty;
} listmark;

static int det_list(line_t l, listmark *m)
{
    size_t i = ln_indent(l);
    if (i > 3 || i >= l.n) return 0;
    size_t p;
    char c = l.p[i];
    if (c == '-' || c == '+' || c == '*') {
        m->ordered = 0;
        m->marker = c;
        m->start = 0;
        p = i + 1;
    } else if (c_digit((unsigned char)c)) {
        size_t k = i;
        long v = 0;
        while (k < l.n && c_digit((unsigned char)l.p[k]) && k - i < 9) {
            v = v * 10 + (l.p[k] - '0');
            k++;
        }
        if (k >= l.n || (l.p[k] != '.' && l.p[k] != ')')) return 0;
        m->ordered = 1;
        m->marker = l.p[k];
        m->start = (int)v;
        p = k + 1;
    } else {
        return 0;
    }
    if (p < l.n && l.p[p] != ' ') return 0;
    size_t sp = 0;
    while (p + sp < l.n && l.p[p + sp] == ' ') sp++;
    if (p + sp >= l.n) {
        m->empty = 1;
        m->content = p + 1;
    } else {
        m->empty = 0;
        m->content = (sp >= 5) ? p + 1 : p + sp;
    }
    return 1;
}

static int name_eq(const char *s, size_t n, const char *name)
{
    size_t k = strlen(name);
    if (k != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (c_lower((unsigned char)s[i]) != name[i]) return 0;
    return 1;
}

static int is_block_tag(const char *s, size_t n)
{
    static const char *const tags[] = {
        "address", "article", "aside", "blockquote", "body", "center", "details", "dialog", "dd", "div",
        "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form", "h1", "h2", "h3", "h4", "h5",
        "h6", "header", "hr", "html", "iframe", "legend", "li", "main", "menu", "nav", "ol", "p", "pre",
        "picture", "script", "section", "style", "summary", "table", "tbody", "td", "tfoot", "th", "thead",
        "tr", "ul", "video", "audio", "source", "br", NULL
    };
    for (int i = 0; tags[i]; i++)
        if (name_eq(s, n, tags[i])) return 1;
    return 0;
}

/* 0: not HTML, 1: HTML block that cannot interrupt a paragraph,
 * 2: block-level HTML, 3: HTML comment. */
static int det_html(line_t l)
{
    size_t i = ln_indent(l);
    if (i > 3 || i + 1 >= l.n || l.p[i] != '<') return 0;
    if (l.n - i >= 4 && memcmp(l.p + i, "<!--", 4) == 0) return 3;
    char c = l.p[i + 1];
    if (c == '!' || c == '?') return 2;
    size_t k = i + 1;
    if (c == '/') k++;
    if (k >= l.n || !c_alpha((unsigned char)l.p[k])) return 0;
    size_t ns = k;
    while (k < l.n && (c_alnum((unsigned char)l.p[k]) || l.p[k] == '-')) k++;
    size_t ne = k;
    if (k < l.n && l.p[k] != ' ' && l.p[k] != '>' && l.p[k] != '/') return 0;
    if (is_block_tag(l.p + ns, ne - ns)) return 2;
    /* Any other tag only starts a block when it is alone on its line. */
    line_t t = ln_trim(l);
    if (t.n && t.p[t.n - 1] == '>') {
        size_t lt = 0;
        for (size_t j = 0; j < t.n; j++)
            if (t.p[j] == '<') lt++;
        if (lt == 1) return 1;
    }
    return 0;
}

static int row_has_pipe(line_t l)
{
    for (size_t k = 0; k < l.n; k++) {
        if (l.p[k] == '\\') { k++; continue; }
        if (l.p[k] == '|') return 1;
    }
    return 0;
}

static size_t split_cells(line_t l, line_t *cells, size_t max)
{
    line_t t = ln_trim(l);
    size_t i = 0, n = t.n;
    if (n && t.p[0] == '|') i = 1;
    if (n > i && t.p[n - 1] == '|' && !(n >= 2 && t.p[n - 2] == '\\')) n--;
    size_t count = 0, start = i;
    for (size_t k = i; k <= n; k++) {
        if (k + 1 < n && t.p[k] == '\\') { k++; continue; }
        if (k == n || t.p[k] == '|') {
            if (count < max) cells[count] = ln_trim(ln_make(t.p + start, k - start));
            count++;
            start = k + 1;
        }
    }
    return count;
}

static int parse_delim_row(line_t l, md_align *al, size_t *ncols)
{
    if (ln_indent(l) > 3 || !row_has_pipe(l)) return 0;
    line_t cells[MD_MAX_COLUMNS];
    size_t n = split_cells(l, cells, MD_MAX_COLUMNS);
    if (n == 0 || n > MD_MAX_COLUMNS) return 0;
    for (size_t c = 0; c < n; c++) {
        line_t x = cells[c];
        if (!x.n) return 0;
        int left = x.p[0] == ':';
        int right = x.n > 1 && x.p[x.n - 1] == ':';
        size_t a = left ? 1 : 0, b = x.n - (right ? 1 : 0);
        if (b <= a) return 0;
        for (size_t k = a; k < b; k++)
            if (x.p[k] != '-') return 0;
        al[c] = (left && right) ? MD_ALIGN_CENTER : right ? MD_ALIGN_RIGHT : left ? MD_ALIGN_LEFT : MD_ALIGN_NONE;
    }
    *ncols = n;
    return 1;
}

static int is_block_start(line_t l)
{
    int level;
    line_t c, info;
    char fc;
    size_t flen, find;
    listmark lm;
    int h;
    if (det_atx(l, &level, &c)) return 1;
    if (det_hr(l)) return 1;
    if (det_fence(l, &fc, &flen, &find, &info)) return 1;
    if (det_quote(l)) return 1;
    if (det_list(l, &lm) && !lm.empty) return 1;
    h = det_html(l);
    if (h == 2 || h == 3) return 1;
    return 0;
}

/* [label]: destination "optional title" on a single line. */
static int det_refdef(md_parser *P, line_t l)
{
    size_t i = ln_indent(l);
    if (i > 3 || i >= l.n || l.p[i] != '[') return 0;
    size_t k = i + 1;
    while (k < l.n && l.p[k] != ']') {
        if (l.p[k] == '\\') k++;
        else if (l.p[k] == '[') return 0;
        k++;
    }
    if (k >= l.n) return 0;
    line_t label = ln_make(l.p + i + 1, k - i - 1);
    if (ln_blank(label)) return 0;
    k++;
    if (k >= l.n || l.p[k] != ':') return 0;
    k++;
    while (k < l.n && l.p[k] == ' ') k++;
    if (k >= l.n) return 0;
    size_t ds, de;
    if (l.p[k] == '<') {
        ds = k + 1;
        de = ds;
        while (de < l.n && l.p[de] != '>') de++;
        if (de >= l.n) return 0;
        k = de + 1;
    } else {
        ds = k;
        while (k < l.n && l.p[k] != ' ') k++;
        de = k;
    }
    size_t after_dest = k;
    while (k < l.n && l.p[k] == ' ') k++;
    size_t ts = 0, te = 0;
    int has_title = 0;
    if (k < l.n) {
        if (k == after_dest) return 0;
        char open = l.p[k];
        char close = open == '(' ? ')' : open;
        if (open != '"' && open != '\'' && open != '(') return 0;
        line_t rest = ln_trim(ln_from(l, k));
        if (rest.n < 2 || rest.p[rest.n - 1] != close) return 0;
        ts = (size_t)(rest.p - l.p) + 1;
        te = (size_t)(rest.p - l.p) + rest.n - 1;
        has_title = 1;
    }
    ref_add(P, label.p, label.n, l.p + ds, de - ds, has_title ? l.p + ts : NULL, has_title ? te - ts : 0, has_title);
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Block parsing                                                            */
/* ------------------------------------------------------------------------ */

static void parse_blocks(md_parser *P, md_node *parent, const char *s, size_t n, int base_line, int depth);

static void close_para(md_node *parent, mdbuf *para, int *in_para, int line)
{
    if (!*in_para) return;
    size_t n = para->len;
    while (n && para->data[n - 1] == ' ') n--;
    md_node *p = node_text(MD_PARAGRAPH, para->data ? para->data : "", n);
    p->line = line;
    p->end_line = line;
    for (size_t k = 0; k < n; k++)
        if (para->data[k] == '\n') p->end_line++;
    node_append(parent, p);
    *in_para = 0;
    para->len = 0;
}

static int all_same(line_t t, char c)
{
    if (!t.n) return 0;
    for (size_t i = 0; i < t.n; i++)
        if (t.p[i] != c) return 0;
    return 1;
}

static void put_line(mdbuf *b, line_t l)
{
    mdbuf_put(b, l.p, l.n);
    mdbuf_putc(b, '\n');
}

static void parse_blocks(md_parser *P, md_node *parent, const char *s, size_t n, int base_line, int depth)
{
    lines_t L;
    lines_split(&L, s, n);

    mdbuf para;
    mdbuf_init(&para);
    int in_para = 0, para_line = 0;
    md_node *cur_list = NULL;
    int blank_before = 0;
    int nest_ok = depth < MD_MAX_DEPTH;
    size_t i = 0;

    while (i < L.n) {
        line_t l = L.v[i];
        int lineno = base_line + (int)i;

        if (ln_blank(l)) {
            close_para(parent, &para, &in_para, para_line);
            blank_before = 1;
            i++;
            continue;
        }
        int had_blank = blank_before;
        blank_before = 0;
        size_t ind = ln_indent(l);

        int level;
        line_t content, info;
        listmark lm;
        char fc;
        size_t flen, find, ncols;
        md_align aligns[MD_MAX_COLUMNS];

        /* Setext heading underline. */
        if (in_para && ind < 4) {
            line_t t = ln_trim(l);
            if (all_same(t, '=') || all_same(t, '-')) {
                size_t pn = para.len;
                while (pn && para.data[pn - 1] == ' ') pn--;
                md_node *h = node_text(MD_HEADING, para.data, pn);
                h->level = t.p[0] == '=' ? 1 : 2;
                h->line = para_line;
                h->end_line = lineno;
                node_append(parent, h);
                in_para = 0;
                para.len = 0;
                cur_list = NULL;
                i++;
                continue;
            }
        }

        /* Indented code block (or lazy paragraph continuation). */
        if (ind >= 4) {
            if (in_para) {
                mdbuf_putc(&para, '\n');
                mdbuf_put(&para, l.p + ind, l.n - ind);
                i++;
                continue;
            }
            mdbuf code;
            mdbuf_init(&code);
            size_t j = i, last = i;
            size_t keep = 0;
            while (j < L.n && (ln_blank(L.v[j]) || ln_indent(L.v[j]) >= 4)) {
                put_line(&code, ln_from(L.v[j], 4));
                if (!ln_blank(L.v[j])) {
                    last = j;
                    keep = code.len;
                }
                j++;
            }
            md_node *cb = node_text(MD_CODE_BLOCK, code.data, keep);
            cb->line = lineno;
            cb->end_line = base_line + (int)last;
            node_append(parent, cb);
            mdbuf_free(&code);
            cur_list = NULL;
            i = last + 1;
            continue;
        }

        /* Fenced code block. */
        if (det_fence(l, &fc, &flen, &find, &info)) {
            close_para(parent, &para, &in_para, para_line);
            md_node *cb = node_new(MD_CODE_BLOCK);
            cb->line = lineno;
            if (info.n) cb->info = unescape(info.p, info.n);
            mdbuf code;
            mdbuf_init(&code);
            size_t j = i + 1;
            while (j < L.n) {
                if (det_fence_close(L.v[j], fc, flen)) {
                    j++;
                    break;
                }
                line_t c = L.v[j];
                size_t k = 0;
                while (k < find && k < c.n && c.p[k] == ' ') k++;
                put_line(&code, ln_from(c, k));
                j++;
            }
            cb->len = code.len;
            cb->text = mdbuf_detach(&code);
            cb->end_line = base_line + (int)j - 1;
            node_append(parent, cb);
            cur_list = NULL;
            i = j;
            continue;
        }

        /* ATX heading. */
        if (det_atx(l, &level, &content)) {
            close_para(parent, &para, &in_para, para_line);
            md_node *h = node_text(MD_HEADING, content.p, content.n);
            h->level = level;
            h->line = lineno;
            h->end_line = lineno;
            node_append(parent, h);
            cur_list = NULL;
            i++;
            continue;
        }

        /* Thematic break. */
        if (det_hr(l)) {
            close_para(parent, &para, &in_para, para_line);
            md_node *hr = node_new(MD_THEMATIC_BREAK);
            hr->line = lineno;
            hr->end_line = lineno;
            node_append(parent, hr);
            cur_list = NULL;
            i++;
            continue;
        }

        /* Block quote. */
        if (nest_ok && det_quote(l)) {
            close_para(parent, &para, &in_para, para_line);
            mdbuf sub;
            mdbuf_init(&sub);
            size_t j = i;
            int last_text = 1;
            while (j < L.n) {
                line_t q = L.v[j];
                size_t qi = ln_indent(q);
                if (qi <= 3 && qi < q.n && q.p[qi] == '>') {
                    size_t k = qi + 1;
                    if (k < q.n && q.p[k] == ' ') k++;
                    line_t r = ln_from(q, k);
                    put_line(&sub, r);
                    last_text = !ln_blank(r);
                    j++;
                } else if (!ln_blank(q) && last_text && !is_block_start(q)) {
                    put_line(&sub, ln_from(q, qi));
                    j++;
                } else {
                    break;
                }
            }
            md_node *bq = node_new(MD_BLOCKQUOTE);
            bq->line = lineno;
            node_append(parent, bq);
            parse_blocks(P, bq, sub.data ? sub.data : "", sub.len, lineno, depth + 1);
            mdbuf_free(&sub);
            cur_list = NULL;
            i = j;
            continue;
        }

        /* List item. */
        if (nest_ok && det_list(l, &lm) && !(in_para && (lm.empty || (lm.ordered && lm.start != 1)))) {
            close_para(parent, &para, &in_para, para_line);
            size_t W = lm.content;
            line_t first = ln_from(l, W);
            int task = MD_TASK_NONE;
            if (first.n >= 3 && first.p[0] == '[' && first.p[2] == ']' &&
                (first.p[1] == ' ' || first.p[1] == 'x' || first.p[1] == 'X') &&
                (first.n == 3 || first.p[3] == ' ')) {
                task = first.p[1] == ' ' ? MD_TASK_OPEN : MD_TASK_DONE;
                first = ln_from(first, first.n == 3 ? 3 : 4);
            }
            mdbuf sub;
            mdbuf_init(&sub);
            put_line(&sub, first);
            size_t j = i + 1, pend = 0;
            int internal_blank = 0;
            while (j < L.n) {
                line_t q = L.v[j];
                if (ln_blank(q)) {
                    if (lm.empty && j == i + 1) break;
                    pend++;
                    j++;
                    continue;
                }
                size_t qi = ln_indent(q);
                listmark tmp;
                if (qi >= W) {
                    if (pend) internal_blank = 1;
                    for (; pend; pend--) mdbuf_putc(&sub, '\n');
                    put_line(&sub, ln_from(q, W));
                    j++;
                    continue;
                }
                if (pend == 0 && !lm.empty && !is_block_start(q) && !det_list(q, &tmp)) {
                    put_line(&sub, ln_from(q, qi));
                    j++;
                    continue;
                }
                break;
            }
            size_t end = j - pend;

            md_node *item = node_new(MD_ITEM);
            item->line = lineno;
            item->task = task;
            parse_blocks(P, item, sub.data, sub.len, lineno, depth + 1);
            mdbuf_free(&sub);

            int compatible = cur_list && parent->last == cur_list && cur_list->ordered == lm.ordered &&
                             cur_list->marker == lm.marker;
            if (!compatible) {
                cur_list = node_new(MD_LIST);
                cur_list->ordered = lm.ordered;
                cur_list->start = lm.start;
                cur_list->marker = lm.marker;
                cur_list->tight = 1;
                cur_list->line = lineno;
                node_append(parent, cur_list);
            } else if (had_blank) {
                cur_list->tight = 0;
            }
            if (internal_blank && item->first && item->first != item->last) cur_list->tight = 0;
            node_append(cur_list, item);
            i = end;
            continue;
        }

        /* HTML block. */
        {
            int ht = det_html(l);
            if (ht && !(in_para && ht == 1)) {
                close_para(parent, &para, &in_para, para_line);
                mdbuf html;
                mdbuf_init(&html);
                size_t j = i;
                if (ht == 3) {
                    while (j < L.n) {
                        line_t q = L.v[j];
                        put_line(&html, q);
                        j++;
                        int done = 0;
                        for (size_t k = 0; k + 2 < q.n; k++)
                            if (q.p[k] == '-' && q.p[k + 1] == '-' && q.p[k + 2] == '>') done = 1;
                        if (done) break;
                    }
                } else {
                    while (j < L.n && !ln_blank(L.v[j])) put_line(&html, L.v[j++]);
                }
                md_node *hb = node_new(MD_HTML_BLOCK);
                hb->line = lineno;
                hb->end_line = base_line + (int)j - 1;
                hb->len = html.len;
                hb->text = mdbuf_detach(&html);
                node_append(parent, hb);
                cur_list = NULL;
                i = j;
                continue;
            }
        }

        /* Table (GitHub extension). */
        if (i + 1 < L.n && ind < 4 && row_has_pipe(l) && parse_delim_row(L.v[i + 1], aligns, &ncols)) {
            line_t cells[MD_MAX_COLUMNS];
            size_t nh = split_cells(l, cells, MD_MAX_COLUMNS);
            if (nh == ncols) {
                close_para(parent, &para, &in_para, para_line);
                md_node *table = node_new(MD_TABLE);
                table->line = lineno;
                table->columns = (int)ncols;
                node_append(parent, table);
                size_t j = i;
                while (j < L.n) {
                    if (j == i + 1) { j++; continue; }
                    line_t q = L.v[j];
                    if (j > i + 1 && (ln_blank(q) || is_block_start(q))) break;
                    size_t nc = split_cells(q, cells, MD_MAX_COLUMNS);
                    md_node *row = node_new(MD_TABLE_ROW);
                    row->header = j == i;
                    row->line = base_line + (int)j;
                    row->end_line = row->line;
                    table->end_line = row->line;
                    node_append(table, row);
                    for (size_t c = 0; c < ncols; c++) {
                        md_node *cell = node_new(MD_TABLE_CELL);
                        mdbuf ct;
                        mdbuf_init(&ct);
                        if (c < nc) {
                            /* GFM: "\|" is a literal pipe everywhere in a cell, even in code spans. */
                            for (size_t k = 0; k < cells[c].n; k++) {
                                if (cells[c].p[k] == '\\' && k + 1 < cells[c].n && cells[c].p[k + 1] == '|') k++;
                                mdbuf_putc(&ct, cells[c].p[k]);
                            }
                        }
                        cell->len = ct.len;
                        cell->text = mdbuf_detach(&ct);
                        cell->align = aligns[c];
                        cell->header = row->header;
                        node_append(row, cell);
                    }
                    j++;
                }
                cur_list = NULL;
                i = j;
                continue;
            }
        }

        /* Link reference definition. */
        if (!in_para && det_refdef(P, l)) {
            i++;
            continue;
        }

        /* Paragraph text. */
        if (!in_para) {
            in_para = 1;
            para_line = lineno;
            para.len = 0;
            cur_list = NULL;
        } else {
            mdbuf_putc(&para, '\n');
        }
        mdbuf_put(&para, l.p + ind, l.n - ind);
        i++;
    }
    close_para(parent, &para, &in_para, para_line);
    mdbuf_free(&para);
    free(L.v);
}

/* ------------------------------------------------------------------------ */
/* Inline parsing                                                           */
/* ------------------------------------------------------------------------ */

typedef struct delim_t {
    md_node *node;
    char c;
    int count, orig;
    int can_open, can_close;
    int alive;
} delim_t;

typedef struct bracket_t {
    md_node *node;
    int image;
    int active;
    size_t delim_bottom;
    size_t pos; /* source offset right after the opening bracket */
} bracket_t;

typedef struct inl_t {
    md_parser *P;
    md_node *parent;
    const char *s;
    size_t n, i;
    mdbuf text;
    delim_t *d;
    size_t nd, capd;
    bracket_t *b;
    size_t nb, capb;
} inl_t;

static void flush_text(inl_t *I)
{
    if (!I->text.len) return;
    node_append(I->parent, node_text(MD_TEXT, I->text.data, I->text.len));
    I->text.len = 0;
}

static md_node *add_inline(inl_t *I, md_type type, const char *s, size_t n)
{
    flush_text(I);
    md_node *x = s ? node_text(type, s, n) : node_new(type);
    node_append(I->parent, x);
    return x;
}

static void put_utf8(mdbuf *b, unsigned long cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) {
        mdbuf_putc(b, (char)cp);
    } else if (cp < 0x800) {
        mdbuf_putc(b, (char)(0xC0 | (cp >> 6)));
        mdbuf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        mdbuf_putc(b, (char)(0xE0 | (cp >> 12)));
        mdbuf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        mdbuf_putc(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        mdbuf_putc(b, (char)(0xF0 | (cp >> 18)));
        mdbuf_putc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        mdbuf_putc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        mdbuf_putc(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static size_t parse_entity(const char *s, size_t n, mdbuf *out);

size_t md_decode_entity(const char *s, size_t n, mdbuf *out) { return parse_entity(s, n, out); }

/* Decodes an entity at s[0]=='&'. Returns bytes consumed or 0. */
static size_t parse_entity(const char *s, size_t n, mdbuf *out)
{
    static const struct { const char *name; unsigned long cp; } named[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0},
        {"copy", 0xA9}, {"reg", 0xAE}, {"trade", 0x2122}, {"hellip", 0x2026}, {"mdash", 0x2014},
        {"ndash", 0x2013}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"deg", 0xB0}, {"times", 0xD7},
        {"divide", 0xF7}, {"euro", 0x20AC}, {"middot", 0xB7}, {"bull", 0x2022}, {"larr", 0x2190},
        {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"check", 0x2713}, {"sect", 0xA7},
        {"para", 0xB6}, {"plusmn", 0xB1}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
        {"rdquo", 0x201D}, {"emsp", 0x2003}, {"ensp", 0x2002}, {"thinsp", 0x2009}, {NULL, 0}
    };
    if (n < 3) return 0;
    size_t k = 1;
    if (s[1] == '#') {
        unsigned long v = 0;
        size_t digits = 0;
        k = 2;
        if (k < n && (s[k] == 'x' || s[k] == 'X')) {
            k++;
            while (k < n && c_xdigit((unsigned char)s[k]) && digits < 6) {
                char c = s[k];
                v = v * 16 + (unsigned long)(c_digit((unsigned char)c) ? c - '0' : c_lower((unsigned char)c) - 'a' + 10);
                k++;
                digits++;
            }
        } else {
            while (k < n && c_digit((unsigned char)s[k]) && digits < 7) {
                v = v * 10 + (unsigned long)(s[k] - '0');
                k++;
                digits++;
            }
        }
        if (!digits || k >= n || s[k] != ';') return 0;
        put_utf8(out, v);
        return k + 1;
    }
    while (k < n && c_alnum((unsigned char)s[k]) && k < 12) k++;
    if (k >= n || s[k] != ';') return 0;
    for (int e = 0; named[e].name; e++) {
        if (strlen(named[e].name) == k - 1 && memcmp(named[e].name, s + 1, k - 1) == 0) {
            put_utf8(out, named[e].cp);
            return k + 1;
        }
    }
    return 0;
}

static void push_delim(inl_t *I, md_node *node, char c, int count, int open, int close)
{
    if (I->nd == I->capd) {
        I->capd = I->capd ? I->capd * 2 : 16;
        I->d = (delim_t *)md_xrealloc(I->d, I->capd * sizeof *I->d);
    }
    delim_t *d = &I->d[I->nd++];
    d->node = node;
    d->c = c;
    d->count = d->orig = count;
    d->can_open = open;
    d->can_close = close;
    d->alive = 1;
}

static void push_bracket(inl_t *I, md_node *node, int image, size_t pos)
{
    if (I->nb == I->capb) {
        I->capb = I->capb ? I->capb * 2 : 8;
        I->b = (bracket_t *)md_xrealloc(I->b, I->capb * sizeof *I->b);
    }
    bracket_t *b = &I->b[I->nb++];
    b->node = node;
    b->image = image;
    b->active = 1;
    b->delim_bottom = I->nd;
    b->pos = pos;
}

static void process_emphasis(inl_t *I, size_t bottom)
{
    for (size_t ci = bottom; ci < I->nd; ci++) {
        delim_t *cl = &I->d[ci];
        if (!cl->alive || !cl->can_close) continue;
        for (;;) {
            size_t oi = 0;
            int found = 0;
            for (size_t k = ci; k > bottom; k--) {
                delim_t *op = &I->d[k - 1];
                if (!op->alive || op->c != cl->c || !op->can_open) continue;
                if (cl->c == '~') {
                    if (op->count != cl->count) continue;
                } else if ((op->can_close || cl->can_open) && (op->orig + cl->orig) % 3 == 0 &&
                           !(op->orig % 3 == 0 && cl->orig % 3 == 0)) {
                    continue;
                }
                oi = k - 1;
                found = 1;
                break;
            }
            if (!found) break;
            delim_t *op = &I->d[oi];
            int use = cl->c == '~' ? cl->count : (cl->count >= 2 && op->count >= 2 ? 2 : 1);
            md_type t = cl->c == '~' ? MD_STRIKE : (use == 2 ? MD_STRONG : MD_EMPH);

            op->node->len -= (size_t)use;
            memmove(cl->node->text, cl->node->text + use, cl->node->len - (size_t)use);
            cl->node->len -= (size_t)use;

            md_node *em = node_new(t);
            md_node *x = op->node->next;
            while (x && x != cl->node) {
                md_node *nx = x->next;
                node_unlink(x);
                node_append(em, x);
                x = nx;
            }
            node_insert_after(op->node, em);
            for (size_t k = oi + 1; k < ci; k++) I->d[k].alive = 0;

            op->count -= use;
            cl->count -= use;
            if (op->count == 0) {
                node_unlink(op->node);
                md_free(op->node);
                op->alive = 0;
            }
            if (cl->count == 0) {
                node_unlink(cl->node);
                md_free(cl->node);
                cl->alive = 0;
                break;
            }
        }
    }
}

/* Parses "(dest "title")" starting at s[j]=='('. */
static int parse_inline_link(const char *s, size_t n, size_t j, char **url, char **title, size_t *end)
{
    size_t k = j + 1;
    while (k < n && c_space((unsigned char)s[k])) k++;
    size_t ds, de;
    if (k < n && s[k] == '<') {
        ds = k + 1;
        de = ds;
        while (de < n && s[de] != '>' && s[de] != '\n' && s[de] != '<') {
            if (s[de] == '\\' && de + 1 < n) de++;
            de++;
        }
        if (de >= n || s[de] != '>') return 0;
        k = de + 1;
    } else {
        ds = k;
        int depth = 0;
        while (k < n) {
            char c = s[k];
            if (c == '\\' && k + 1 < n && c_punct((unsigned char)s[k + 1])) { k += 2; continue; }
            if (c_space((unsigned char)c) || (unsigned char)c < 0x20) break;
            if (c == '(') depth++;
            if (c == ')') {
                if (depth == 0) break;
                depth--;
            }
            k++;
        }
        de = k;
        if (depth != 0) return 0;
    }
    size_t before_ws = k;
    while (k < n && c_space((unsigned char)s[k])) k++;
    size_t ts = 0, te = 0;
    int has_title = 0;
    if (k < n && k > before_ws && (s[k] == '"' || s[k] == '\'' || s[k] == '(')) {
        char close = s[k] == '(' ? ')' : s[k];
        ts = k + 1;
        size_t m = ts;
        while (m < n && s[m] != close) {
            if (s[m] == '\\' && m + 1 < n) m++;
            m++;
        }
        if (m >= n) return 0;
        te = m;
        has_title = 1;
        k = m + 1;
        while (k < n && c_space((unsigned char)s[k])) k++;
    }
    if (k >= n || s[k] != ')') return 0;
    *url = unescape(s + ds, de - ds);
    *title = has_title ? unescape(s + ts, te - ts) : NULL;
    *end = k + 1;
    return 1;
}

static void close_bracket(inl_t *I)
{
    const char *s = I->s;
    size_t n = I->n, i = I->i;
    if (I->nb == 0) {
        mdbuf_putc(&I->text, ']');
        I->i++;
        return;
    }
    bracket_t br = I->b[I->nb - 1];
    if (!br.active) {
        I->nb--;
        mdbuf_putc(&I->text, ']');
        I->i++;
        return;
    }
    char *url = NULL, *title = NULL;
    size_t next = i + 1;
    int matched = 0;
    if (i + 1 < n && s[i + 1] == '(')
        matched = parse_inline_link(s, n, i + 1, &url, &title, &next);
    if (!matched) {
        const char *label = s + br.pos;
        size_t label_len = i - br.pos;
        next = i + 1;
        int full = 0;
        if (i + 1 < n && s[i + 1] == '[') {
            size_t k = i + 2;
            while (k < n && s[k] != ']' && s[k] != '[') {
                if (s[k] == '\\') k++;
                k++;
            }
            if (k < n && s[k] == ']') {
                if (k > i + 2) {
                    label = s + i + 2;
                    label_len = k - i - 2;
                    full = 1;
                }
                next = k + 1;
            }
        }
        const md_ref *r = ref_find(I->P, label, label_len);
        if (r) {
            matched = 1;
            url = md_xstrndup(r->url, strlen(r->url));
            title = r->title ? md_xstrndup(r->title, strlen(r->title)) : NULL;
        } else {
            (void)full;
        }
    }
    if (!matched) {
        I->nb--;
        mdbuf_putc(&I->text, ']');
        I->i++;
        return;
    }
    flush_text(I);
    md_node *link = node_new(br.image ? MD_IMAGE : MD_LINK);
    link->url = url;
    link->title = title;
    md_node *x = br.node->next;
    while (x) {
        md_node *nx = x->next;
        node_unlink(x);
        node_append(link, x);
        x = nx;
    }
    node_insert_after(br.node, link);
    node_unlink(br.node);
    md_free(br.node);
    process_emphasis(I, br.delim_bottom);
    I->nd = br.delim_bottom;
    I->nb--;
    if (!br.image)
        for (size_t k = 0; k < I->nb; k++)
            if (!I->b[k].image) I->b[k].active = 0;
    I->i = next;
}

static int is_scheme_autolink(const char *s, size_t n, size_t *end)
{
    /* s[0] == '<' */
    size_t k = 1;
    if (k >= n || !c_alpha((unsigned char)s[k])) return 0;
    size_t sc = 0;
    while (k < n && (c_alnum((unsigned char)s[k]) || s[k] == '+' || s[k] == '.' || s[k] == '-')) { k++; sc++; }
    if (sc < 2 || sc > 32 || k >= n || s[k] != ':') return 0;
    while (k < n && s[k] != '>') {
        unsigned char c = (unsigned char)s[k];
        if (c <= ' ' || c == '<') return 0;
        k++;
    }
    if (k >= n) return 0;
    *end = k;
    return 1;
}

static int is_email_autolink(const char *s, size_t n, size_t *end)
{
    size_t k = 1;
    size_t local = 0;
    while (k < n && (c_alnum((unsigned char)s[k]) || strchr(".!#$%&'*+/=?^_`{|}~-", s[k]))) { k++; local++; }
    if (!local || k >= n || s[k] != '@') return 0;
    k++;
    size_t dom = 0;
    while (k < n && (c_alnum((unsigned char)s[k]) || s[k] == '-' || s[k] == '.')) { k++; dom++; }
    if (!dom || k >= n || s[k] != '>') return 0;
    *end = k;
    return 1;
}

static int is_inline_html(const char *s, size_t n, size_t *end)
{
    if (n >= 7 && memcmp(s, "<!--", 4) == 0) {
        for (size_t k = 4; k + 2 < n; k++)
            if (s[k] == '-' && s[k + 1] == '-' && s[k + 2] == '>') { *end = k + 2; return 1; }
        return 0;
    }
    size_t k = 1;
    if (k < n && s[k] == '/') k++;
    if (k >= n || !c_alpha((unsigned char)s[k])) return 0;
    while (k < n && (c_alnum((unsigned char)s[k]) || s[k] == '-')) k++;
    if (k < n && s[k] != '>' && s[k] != ' ' && s[k] != '/' && s[k] != '\n') return 0;
    char quote = 0;
    while (k < n) {
        char c = s[k];
        if (quote) {
            if (c == quote) quote = 0;
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '>') {
            *end = k;
            return 1;
        } else if (c == '<') {
            return 0;
        }
        k++;
    }
    return 0;
}

/* GitHub-style bare URL autolinks: http://, https:// and www. */
static int try_bare_url(inl_t *I)
{
    const char *s = I->s;
    size_t n = I->n, i = I->i;
    if (I->nb) return 0;
    if (i > 0) {
        char p = s[i - 1];
        if (!c_space((unsigned char)p) && p != '*' && p != '_' && p != '~' && p != '(') return 0;
    }
    size_t pre;
    if (n - i > 8 && memcmp(s + i, "https://", 8) == 0) pre = 8;
    else if (n - i > 7 && memcmp(s + i, "http://", 7) == 0) pre = 7;
    else if (n - i > 4 && memcmp(s + i, "www.", 4) == 0) pre = 4;
    else return 0;
    size_t e = i + pre;
    while (e < n && !c_space((unsigned char)s[e]) && s[e] != '<') e++;
    for (;;) {
        if (e <= i + pre) return 0;
        char c = s[e - 1];
        if (strchr("?!.,:*_~'\"", c)) { e--; continue; }
        if (c == ')') {
            int open = 0, close = 0;
            for (size_t k = i; k < e; k++) {
                if (s[k] == '(') open++;
                else if (s[k] == ')') close++;
            }
            if (close > open) { e--; continue; }
        }
        break;
    }
    if (e <= i + pre) return 0;
    if (pre == 4 && !c_alnum((unsigned char)s[i + 4])) return 0;
    md_node *link = add_inline(I, MD_LINK, NULL, 0);
    mdbuf u;
    mdbuf_init(&u);
    if (pre == 4) mdbuf_puts(&u, "http://");
    mdbuf_put(&u, s + i, e - i);
    link->url = mdbuf_detach(&u);
    node_append(link, node_text(MD_TEXT, s + i, e - i));
    I->i = e;
    return 1;
}

static void parse_inline(md_parser *P, md_node *parent, const char *s, size_t n)
{
    inl_t I;
    memset(&I, 0, sizeof I);
    I.P = P;
    I.parent = parent;
    I.s = s;
    I.n = n;
    mdbuf_init(&I.text);

    while (I.i < n) {
        size_t i = I.i;
        char c = s[i];
        switch (c) {
        case '\\':
            if (i + 1 < n && s[i + 1] == '\n') {
                add_inline(&I, MD_LINEBREAK, NULL, 0);
                I.i += 2;
                while (I.i < n && s[I.i] == ' ') I.i++;
            } else if (i + 1 < n && c_punct((unsigned char)s[i + 1])) {
                mdbuf_putc(&I.text, s[i + 1]);
                I.i += 2;
            } else {
                mdbuf_putc(&I.text, '\\');
                I.i++;
            }
            break;

        case '`': {
            size_t k = 0;
            while (i + k < n && s[i + k] == '`') k++;
            size_t j = i + k, found = 0;
            while (j < n) {
                if (s[j] == '`') {
                    size_t m = 0;
                    while (j + m < n && s[j + m] == '`') m++;
                    if (m == k) { found = 1; break; }
                    j += m;
                } else {
                    j++;
                }
            }
            if (!found) {
                mdbuf_put(&I.text, s + i, k);
                I.i += k;
                break;
            }
            mdbuf code;
            mdbuf_init(&code);
            for (size_t m = i + k; m < j; m++) mdbuf_putc(&code, s[m] == '\n' ? ' ' : s[m]);
            const char *cp = code.data ? code.data : "";
            size_t cn = code.len;
            int all_space = 1;
            for (size_t m = 0; m < cn; m++)
                if (cp[m] != ' ') all_space = 0;
            if (cn >= 2 && cp[0] == ' ' && cp[cn - 1] == ' ' && !all_space) {
                cp++;
                cn -= 2;
            }
            add_inline(&I, MD_CODE, cp, cn);
            mdbuf_free(&code);
            I.i = j + k;
            break;
        }

        case '*':
        case '_':
        case '~': {
            size_t k = 0;
            while (i + k < n && s[i + k] == c) k++;
            if (c == '~' && k > 2) {
                mdbuf_put(&I.text, s + i, k);
                I.i += k;
                break;
            }
            unsigned char before = i == 0 ? ' ' : (unsigned char)s[i - 1];
            unsigned char after = i + k < n ? (unsigned char)s[i + k] : ' ';
            int ws_b = c_space(before), ws_a = c_space(after);
            int p_b = c_punct(before), p_a = c_punct(after);
            int left = !ws_a && (!p_a || ws_b || p_b);
            int right = !ws_b && (!p_b || ws_a || p_a);
            int open, close;
            if (c == '_') {
                open = left && (!right || p_b);
                close = right && (!left || p_a);
            } else {
                open = left;
                close = right;
            }
            md_node *t = add_inline(&I, MD_TEXT, s + i, k);
            push_delim(&I, t, c, (int)k, open, close);
            I.i += k;
            break;
        }

        case '!':
            if (i + 1 < n && s[i + 1] == '[') {
                md_node *t = add_inline(&I, MD_TEXT, "![", 2);
                push_bracket(&I, t, 1, i + 2);
                I.i += 2;
            } else {
                mdbuf_putc(&I.text, c);
                I.i++;
            }
            break;

        case '[': {
            md_node *t = add_inline(&I, MD_TEXT, "[", 1);
            push_bracket(&I, t, 0, i + 1);
            I.i++;
            break;
        }

        case ']':
            close_bracket(&I);
            break;

        case '<': {
            size_t end;
            if (is_scheme_autolink(s + i, n - i, &end)) {
                md_node *link = add_inline(&I, MD_LINK, NULL, 0);
                link->url = md_xstrndup(s + i + 1, end - 1);
                node_append(link, node_text(MD_TEXT, s + i + 1, end - 1));
                I.i += end + 1;
            } else if (is_email_autolink(s + i, n - i, &end)) {
                md_node *link = add_inline(&I, MD_LINK, NULL, 0);
                mdbuf u;
                mdbuf_init(&u);
                mdbuf_puts(&u, "mailto:");
                mdbuf_put(&u, s + i + 1, end - 1);
                link->url = mdbuf_detach(&u);
                node_append(link, node_text(MD_TEXT, s + i + 1, end - 1));
                I.i += end + 1;
            } else if (is_inline_html(s + i, n - i, &end)) {
                add_inline(&I, MD_HTML_INLINE, s + i, end + 1);
                I.i += end + 1;
            } else {
                mdbuf_putc(&I.text, '<');
                I.i++;
            }
            break;
        }

        case '&': {
            size_t used = parse_entity(s + i, n - i, &I.text);
            if (used) I.i += used;
            else {
                mdbuf_putc(&I.text, '&');
                I.i++;
            }
            break;
        }

        case '\n': {
            size_t sp = 0;
            while (I.text.len > sp && I.text.data[I.text.len - 1 - sp] == ' ') sp++;
            I.text.len -= sp;
            add_inline(&I, sp >= 2 ? MD_LINEBREAK : MD_SOFTBREAK, NULL, 0);
            I.i++;
            while (I.i < n && s[I.i] == ' ') I.i++;
            break;
        }

        case 'h':
        case 'w':
            if (try_bare_url(&I)) break;
            mdbuf_putc(&I.text, c);
            I.i++;
            break;

        default:
            mdbuf_putc(&I.text, c);
            I.i++;
            break;
        }
    }
    flush_text(&I);
    process_emphasis(&I, 0);

    /* Merge adjacent text nodes. */
    md_node *x = parent->first;
    while (x) {
        if (x->type == MD_TEXT && x->next && x->next->type == MD_TEXT) {
            md_node *y = x->next;
            char *t = (char *)md_xmalloc(x->len + y->len + 1);
            memcpy(t, x->text, x->len);
            memcpy(t + x->len, y->text, y->len);
            t[x->len + y->len] = '\0';
            free(x->text);
            x->text = t;
            x->len += y->len;
            node_unlink(y);
            md_free(y);
            continue;
        }
        x = x->next;
    }
    /* Inline parsing nested inside links/emphasis already merged above only at
     * the top level; merge one level down as well for tidier output. */
    for (md_node *c = parent->first; c; c = c->next) {
        md_node *y = c->first;
        while (y) {
            if (y->type == MD_TEXT && y->next && y->next->type == MD_TEXT) {
                md_node *z = y->next;
                char *t = (char *)md_xmalloc(y->len + z->len + 1);
                memcpy(t, y->text, y->len);
                memcpy(t + y->len, z->text, z->len);
                t[y->len + z->len] = '\0';
                free(y->text);
                y->text = t;
                y->len += z->len;
                node_unlink(z);
                md_free(z);
                continue;
            }
            y = y->next;
        }
    }

    mdbuf_free(&I.text);
    free(I.d);
    free(I.b);
}

static void parse_inlines_tree(md_parser *P, md_node *node)
{
    for (md_node *c = node->first; c; c = c->next) {
        switch (c->type) {
        case MD_PARAGRAPH:
        case MD_HEADING:
        case MD_TABLE_CELL:
            if (c->text) {
                char *t = c->text;
                size_t len = c->len;
                c->text = NULL;
                c->len = 0;
                parse_inline(P, c, t, len);
                free(t);
            }
            break;
        case MD_BLOCKQUOTE:
        case MD_LIST:
        case MD_ITEM:
        case MD_TABLE:
        case MD_TABLE_ROW:
            parse_inlines_tree(P, c);
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

md_node *md_parse(const char *src, size_t len)
{
    mdbuf norm;
    mdbuf_init(&norm);
    mdbuf_reserve(&norm, len + 16);
    size_t i = 0;
    if (len >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF)
        i = 3;
    size_t col = 0;
    for (; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\r') {
            mdbuf_putc(&norm, '\n');
            if (i + 1 < len && src[i + 1] == '\n') i++;
            col = 0;
        } else if (c == '\n') {
            mdbuf_putc(&norm, '\n');
            col = 0;
        } else if (c == '\t') {
            size_t sp = 4 - (col % 4);
            for (size_t k = 0; k < sp; k++) mdbuf_putc(&norm, ' ');
            col += sp;
        } else if (c == 0) {
            put_utf8(&norm, 0xFFFD);
            col++;
        } else {
            mdbuf_putc(&norm, (char)c);
            if ((c & 0xC0) != 0x80) col++;
        }
    }

    md_parser P;
    memset(&P, 0, sizeof P);
    md_node *doc = node_new(MD_DOCUMENT);
    parse_blocks(&P, doc, norm.data ? norm.data : "", norm.len, 0, 0);
    parse_inlines_tree(&P, doc);

    for (size_t r = 0; r < P.nrefs; r++) {
        free(P.refs[r].label);
        free(P.refs[r].url);
        free(P.refs[r].title);
    }
    free(P.refs);
    mdbuf_free(&norm);
    return doc;
}


static int is_eol(char c) { return c == '\n' || c == '\r'; }

/* Skips one line break at text[i] (CR, LF or CRLF); returns the new index. */
static size_t skip_eol(const char *text, size_t len, size_t i)
{
    if (text[i] == '\r' && i + 1 < len && text[i + 1] == '\n') return i + 2;
    return i + 1;
}

int md_line_count(const char *text, size_t len)
{
    int n = 1;
    size_t i = 0;
    while (i < len) {
        if (is_eol(text[i])) {
            i = skip_eol(text, len, i);
            n++;
        } else {
            i++;
        }
    }
    return n;
}

/* Documents edited in place are small enough for a linear scan per call. */
static int line_is_blank(const char *text, size_t len, int line)
{
    int cur = 0;
    size_t i = 0;
    while (i < len && cur < line) {
        if (is_eol(text[i])) {
            i = skip_eol(text, len, i);
            cur++;
        } else {
            i++;
        }
    }
    for (; i < len && !is_eol(text[i]); i++)
        if (text[i] != ' ' && text[i] != '\t') return 0;
    return 1;
}

int md_block_span(const md_node *doc, const char *text, size_t len, int *first, int *last, int *end)
{
    int lines = md_line_count(text, len);
    int f = *first, l = *last, e = lines, any = 0;
    for (const md_node *c = doc ? doc->first : NULL; c; c = c->next) {
        int cs = c->line, ce = c->next ? c->next->line : lines;
        int cl = ce - 1;
        while (cl > cs && line_is_blank(text, len, cl)) cl--;
        if (cl < f) continue;
        if (cs > l) {
            e = cs;
            break;
        }
        any = 1;
        if (cs < f) f = cs;
        if (cl > l) l = cl;
    }
    *first = f;
    *last = l;
    *end = e;
    return any;
}
