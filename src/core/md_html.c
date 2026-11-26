/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "md_html.h"
#include "mdbuf.h"

#include <stdlib.h>
#include <string.h>

/* ---- nodes ------------------------------------------------------------- */

static md_node *mk(md_type type, const md_node *at)
{
    md_node *n = (md_node *)md_xcalloc(1, sizeof *n);
    n->type = type;
    n->from_html = 1;
    n->line = at ? at->line : 0;
    n->end_line = at ? at->end_line : 0;
    return n;
}

static void append(md_node *parent, md_node *child)
{
    child->parent = parent;
    child->next = NULL;
    child->prev = parent->last;
    if (parent->last) parent->last->next = child;
    else parent->first = child;
    parent->last = child;
}

static void unlink_node(md_node *n)
{
    if (n->prev) n->prev->next = n->next;
    else if (n->parent) n->parent->first = n->next;
    if (n->next) n->next->prev = n->prev;
    else if (n->parent) n->parent->last = n->prev;
    n->prev = n->next = n->parent = NULL;
}

static void insert_after(md_node *ref, md_node *n)
{
    n->parent = ref->parent;
    n->prev = ref;
    n->next = ref->next;
    if (ref->next) ref->next->prev = n;
    else if (ref->parent) ref->parent->last = n;
    ref->next = n;
}

static md_node *mk_text(const char *s, size_t len, const md_node *at)
{
    md_node *n = mk(MD_TEXT, at);
    n->text = md_xstrndup(s, len);
    n->len = len;
    return n;
}

/* ---- tokens -------------------------------------------------------------- */

enum { TK_TEXT, TK_START, TK_END, TK_SKIP };

typedef struct tok {
    int kind;
    char name[16];
    const char *attrs, *text;
    size_t alen, tlen;
} tok;

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static size_t find(const char *s, size_t n, size_t i, const char *what)
{
    size_t w = strlen(what);
    for (; i + w <= n; i++)
        if (memcmp(s + i, what, w) == 0) return i;
    return n;
}

/* Reads one token at s[i]; returns the index after it. */
static size_t next_tok(const char *s, size_t n, size_t i, tok *t)
{
    memset(t, 0, sizeof *t);
    if (s[i] == '<') {
        if (i + 3 < n && memcmp(s + i, "<!--", 4) == 0) {
            size_t e = find(s, n, i + 4, "-->");
            t->kind = TK_SKIP;
            return e < n ? e + 3 : n;
        }
        if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
            size_t e = find(s, n, i, ">");
            t->kind = TK_SKIP;
            return e < n ? e + 1 : n;
        }
        size_t j = i + 1;
        int closing = j < n && s[j] == '/';
        if (closing) j++;
        if (j < n && is_alpha(s[j])) {
            size_t k = 0;
            while (j < n && (is_alpha(s[j]) || (s[j] >= '0' && s[j] <= '9') || s[j] == '-')) {
                if (k < sizeof t->name - 1) t->name[k++] = (char)(s[j] >= 'A' && s[j] <= 'Z' ? s[j] + 32 : s[j]);
                j++;
            }
            size_t a = j;
            char q = 0;
            while (j < n && (q || s[j] != '>')) {
                if (q && s[j] == q) q = 0;
                else if (!q && (s[j] == '"' || s[j] == '\'')) q = s[j];
                j++;
            }
            if (j < n) {
                t->kind = closing ? TK_END : TK_START;
                t->attrs = s + a;
                t->alen = j - a;
                return j + 1;
            }
        }
        t->kind = TK_TEXT; /* a lone "<" */
        t->text = s + i;
        t->tlen = 1;
        return i + 1;
    }
    size_t j = i;
    while (j < n && s[j] != '<') j++;
    t->kind = TK_TEXT;
    t->text = s + i;
    t->tlen = j - i;
    return j;
}

static void decode(const char *s, size_t n, mdbuf *out)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '&') {
            size_t k = md_decode_entity(s + i, n - i, out);
            if (k) {
                i += k - 1;
                continue;
            }
        }
        mdbuf_putc(out, s[i]);
    }
}

/* The value of attribute `name` (entities decoded), or 0 when absent. */
static int attr(const tok *t, const char *name, mdbuf *out)
{
    const char *s = t->attrs;
    size_t n = t->alen, i = 0, nl = strlen(name);
    while (i < n) {
        while (i < n && (is_space(s[i]) || s[i] == '/')) i++;
        size_t a = i;
        while (i < n && !is_space(s[i]) && s[i] != '=' && s[i] != '/') i++;
        size_t an = i - a;
        while (i < n && is_space(s[i])) i++;
        const char *v = NULL;
        size_t vn = 0;
        if (i < n && s[i] == '=') {
            i++;
            while (i < n && is_space(s[i])) i++;
            if (i < n && (s[i] == '"' || s[i] == '\'')) {
                char q = s[i++];
                v = s + i;
                while (i < n && s[i] != q) i++;
                vn = (size_t)(s + i - v);
                if (i < n) i++;
            } else {
                v = s + i;
                while (i < n && !is_space(s[i])) i++;
                vn = (size_t)(s + i - v);
            }
        }
        int same = an == nl;
        for (size_t k = 0; same && k < nl; k++) {
            char c = s[a + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != name[k]) same = 0;
        }
        if (same && an) {
            out->len = 0;
            if (v) decode(v, vn, out);
            if (!out->data) mdbuf_puts(out, "");
            return 1;
        }
        if (an == 0 && i < n) i++;
    }
    return 0;
}

static md_align align_of(const tok *t)
{
    mdbuf v;
    mdbuf_init(&v);
    md_align a = MD_ALIGN_NONE;
    if (attr(t, "align", &v) && v.data) {
        if (strcmp(v.data, "center") == 0 || strcmp(v.data, "middle") == 0) a = MD_ALIGN_CENTER;
        else if (strcmp(v.data, "right") == 0) a = MD_ALIGN_RIGHT;
        else if (strcmp(v.data, "left") == 0) a = MD_ALIGN_LEFT;
    }
    mdbuf_free(&v);
    return a;
}

static int in_list(const char *name, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(name, *list) == 0) return 1;
    return 0;
}

/* Elements dropped together with everything inside them. */
static const char *const dropped[] = {"script", "style", "iframe", "object", "embed", "noscript", "template",
                                      "svg", "math", "head", "title", "textarea", "select", "form", "frameset",
                                      "frame", "applet", "audio", "video", "canvas", NULL};
/* Elements without content or end tag that show nothing. */
static const char *const voids[] = {"input", "meta", "link", "wbr", "source", "col", "area", "base", "param", "track",
                                    "keygen", NULL};
static const char *const containers[] = {"p", "div", "center", "section", "article", "header", "footer", "nav", "main",
                                         "aside", "figure", "figcaption", "address", "dl", "dd", "dt", "details",
                                         "body", "html", "thead", "tbody", "tfoot", NULL};

static md_type inline_type(const char *name)
{
    if (!strcmp(name, "b") || !strcmp(name, "strong")) return MD_STRONG;
    if (!strcmp(name, "i") || !strcmp(name, "em") || !strcmp(name, "cite") || !strcmp(name, "var") ||
        !strcmp(name, "dfn"))
        return MD_EMPH;
    if (!strcmp(name, "s") || !strcmp(name, "del") || !strcmp(name, "strike")) return MD_STRIKE;
    if (!strcmp(name, "code") || !strcmp(name, "kbd") || !strcmp(name, "tt") || !strcmp(name, "samp")) return MD_CODE;
    if (!strcmp(name, "a")) return MD_LINK;
    return MD_TEXT; /* anything else: keep the content only */
}

/* ---- HTML blocks ----------------------------------------------------------- */

enum { K_INLINE = 1, K_BLOCK = 2 };

typedef struct entry {
    char name[16];
    int kind;
    md_node *node, *container, *para, *inl;
    md_align align;
} entry;

typedef struct builder {
    md_node *root;
    md_node *container, *para, *inl;
    md_align align;
    int last_space;
    md_node *pre, *code;
    mdbuf buf;
    char skip[16];
    entry st[64];
    int depth;
} builder;

static void close_para(builder *b)
{
    b->para = b->inl = NULL;
}

static int ensure_para(builder *b)
{
    if (b->para) return 1;
    md_type ct = b->container->type;
    if (ct == MD_LIST || ct == MD_TABLE || ct == MD_TABLE_ROW) return 0;
    md_node *p = mk(MD_PARAGRAPH, b->root);
    p->align = b->align;
    append(b->container, p);
    b->para = b->inl = p;
    b->last_space = 1;
    return 1;
}

static void push(builder *b, const char *name, int kind, md_node *node)
{
    if (b->depth >= 64) return;
    entry *e = &b->st[b->depth++];
    strncpy(e->name, name, sizeof e->name - 1);
    e->name[sizeof e->name - 1] = 0;
    e->kind = kind;
    e->node = node;
    e->container = b->container;
    e->para = b->para;
    e->inl = b->inl;
    e->align = b->align;
}

static void finish_text_node(builder *b, md_node *n)
{
    n->text = md_xstrndup(b->buf.data ? b->buf.data : "", b->buf.len);
    n->len = b->buf.len;
    b->buf.len = 0;
}

static void pop_entry(builder *b)
{
    entry *e = &b->st[--b->depth];
    if (e->node && e->node == b->pre) {
        size_t len = b->buf.len;
        while (len && b->buf.data[len - 1] == '\n') len--;
        b->buf.len = len;
        mdbuf_putc(&b->buf, '\n');
        finish_text_node(b, b->pre);
        b->pre = NULL;
    }
    if (e->node && e->node == b->code) {
        finish_text_node(b, b->code);
        b->code = NULL;
    }
    if (e->kind == K_INLINE) {
        b->inl = e->inl;
    } else {
        b->container = e->container;
        b->para = e->para;
        b->inl = e->inl;
        b->align = e->align;
    }
}

static void pop_to(builder *b, const char *name)
{
    int k = b->depth - 1;
    while (k >= 0 && strcmp(b->st[k].name, name) != 0) k--;
    if (k < 0) return; /* an end tag nobody opened */
    while (b->depth > k) pop_entry(b);
}

static void add_text(builder *b, const char *s, size_t n)
{
    mdbuf d;
    mdbuf_init(&d);
    decode(s, n, &d);
    if (b->pre) {
        mdbuf_put(&b->buf, d.data ? d.data : "", d.len);
    } else if (b->code) {
        for (size_t i = 0; i < d.len; i++) mdbuf_putc(&b->buf, is_space(d.data[i]) ? ' ' : d.data[i]);
    } else {
        /* HTML whitespace: runs collapse to one space */
        mdbuf t;
        mdbuf_init(&t);
        for (size_t i = 0; i < d.len; i++) {
            if (is_space(d.data[i])) {
                if (!b->last_space) mdbuf_putc(&t, ' ');
                b->last_space = 1;
            } else {
                mdbuf_putc(&t, d.data[i]);
                b->last_space = 0;
            }
        }
        if (t.len && !(t.len == 1 && t.data[0] == ' ' && !b->para) && ensure_para(b)) {
            const char *p = t.data;
            size_t len = t.len;
            if (!b->para->first && b->inl == b->para && *p == ' ') {
                p++;
                len--;
            }
            if (len) append(b->inl, mk_text(p, len, b->root));
        }
        mdbuf_free(&t);
    }
    mdbuf_free(&d);
}

static void start_tag(builder *b, const tok *t)
{
    const char *nm = t->name;
    mdbuf v;
    mdbuf_init(&v);
    if (in_list(nm, dropped)) {
        strcpy(b->skip, nm);
    } else if (b->pre) {
        /* tags inside <pre> are ignored, the text stays */
    } else if (!strcmp(nm, "br")) {
        if (ensure_para(b)) append(b->inl, mk(MD_LINEBREAK, b->root));
        b->last_space = 1;
    } else if (!strcmp(nm, "img")) {
        if (attr(t, "src", &v) && v.len && ensure_para(b)) {
            md_node *img = mk(MD_IMAGE, b->root);
            img->url = md_xstrndup(v.data, v.len);
            if (attr(t, "width", &v) && v.len) img->width = atoi(v.data);
            if (attr(t, "alt", &v) && v.len) append(img, mk_text(v.data, v.len, b->root));
            append(b->inl, img);
            b->last_space = 0;
        }
    } else if (!strcmp(nm, "hr")) {
        close_para(b);
        md_type ct = b->container->type;
        if (ct != MD_LIST && ct != MD_TABLE && ct != MD_TABLE_ROW) append(b->container, mk(MD_THEMATIC_BREAK, b->root));
    } else if (in_list(nm, voids)) {
        /* nothing to show */
    } else if (nm[0] == 'h' && nm[1] >= '1' && nm[1] <= '6' && !nm[2]) {
        close_para(b);
        md_node *h = mk(MD_HEADING, b->root);
        h->level = nm[1] - '0';
        h->align = align_of(t) ? align_of(t) : b->align;
        push(b, nm, K_BLOCK, h);
        if (b->container->type == MD_LIST || b->container->type == MD_TABLE || b->container->type == MD_TABLE_ROW) {
            md_free(h);
            b->st[b->depth - 1].node = NULL;
        } else {
            append(b->container, h);
            b->para = b->inl = h;
            b->last_space = 1;
        }
    } else if (!strcmp(nm, "ul") || !strcmp(nm, "ol")) {
        close_para(b);
        md_node *l = mk(MD_LIST, b->root);
        l->ordered = nm[0] == 'o';
        l->start = 1;
        if (l->ordered && attr(t, "start", &v) && v.len) l->start = atoi(v.data);
        l->marker = l->ordered ? '.' : '-';
        l->tight = 1;
        push(b, nm, K_BLOCK, l);
        if (b->container->type == MD_TABLE || b->container->type == MD_TABLE_ROW) {
            md_free(l);
            b->st[b->depth - 1].node = NULL;
        } else {
            append(b->container, l);
            b->container = l;
        }
    } else if (!strcmp(nm, "li")) {
        close_para(b);
        push(b, nm, K_BLOCK, NULL);
        if (b->container->type == MD_LIST) {
            md_node *it = mk(MD_ITEM, b->root);
            append(b->container, it);
            b->container = it;
            b->st[b->depth - 1].node = it;
        }
    } else if (!strcmp(nm, "blockquote")) {
        close_para(b);
        push(b, nm, K_BLOCK, NULL);
        if (b->container->type != MD_LIST && b->container->type != MD_TABLE && b->container->type != MD_TABLE_ROW) {
            md_node *q = mk(MD_BLOCKQUOTE, b->root);
            append(b->container, q);
            b->container = q;
        }
    } else if (!strcmp(nm, "pre")) {
        close_para(b);
        push(b, nm, K_BLOCK, NULL);
        if (b->container->type != MD_LIST && b->container->type != MD_TABLE && b->container->type != MD_TABLE_ROW) {
            md_node *c = mk(MD_CODE_BLOCK, b->root);
            append(b->container, c);
            b->pre = c;
            b->st[b->depth - 1].node = c;
            b->buf.len = 0;
        }
    } else if (!strcmp(nm, "table")) {
        close_para(b);
        push(b, nm, K_BLOCK, NULL);
        md_node *tb = mk(MD_TABLE, b->root);
        append(b->container->type == MD_TABLE_ROW || b->container->type == MD_TABLE ? b->root : b->container, tb);
        b->container = tb;
    } else if (!strcmp(nm, "tr")) {
        close_para(b);
        push(b, nm, K_BLOCK, NULL);
        if (b->container->type == MD_TABLE) {
            md_node *r = mk(MD_TABLE_ROW, b->root);
            append(b->container, r);
            b->container = r;
        }
    } else if (!strcmp(nm, "td") || !strcmp(nm, "th")) {
        close_para(b);
        push(b, nm, K_BLOCK, NULL);
        if (b->container->type == MD_TABLE_ROW) {
            md_node *c = mk(MD_TABLE_CELL, b->root);
            c->header = nm[1] == 'h';
            c->align = align_of(t);
            append(b->container, c);
            b->para = b->inl = c;
            b->last_space = 1;
        }
    } else if (!strcmp(nm, "summary")) {
        close_para(b);
        push(b, nm, K_BLOCK, NULL);
        if (ensure_para(b)) {
            md_node *s = mk(MD_STRONG, b->root);
            append(b->para, s);
            b->inl = s;
        }
    } else if (in_list(nm, containers)) {
        if (b->container->type != MD_TABLE_CELL && b->para && b->para->type == MD_TABLE_CELL) {
            push(b, nm, K_INLINE, NULL); /* block tags inside a cell: just their text */
        } else {
            close_para(b);
            push(b, nm, K_BLOCK, NULL);
            md_align a = align_of(t);
            if (!strcmp(nm, "center")) a = MD_ALIGN_CENTER;
            if (a) b->align = a;
        }
    } else {
        md_type ty = inline_type(nm);
        md_node *node = NULL;
        if (ty != MD_TEXT && ensure_para(b)) {
            if (ty == MD_LINK) {
                if (attr(t, "href", &v) && v.len) {
                    node = mk(MD_LINK, b->root);
                    node->url = md_xstrndup(v.data, v.len);
                }
            } else {
                node = mk(ty, b->root);
            }
            if (node) {
                append(b->inl, node);
                if (ty == MD_CODE && !b->code) {
                    b->code = node;
                    b->buf.len = 0;
                }
            }
        }
        push(b, nm, K_INLINE, node);
        if (node && ty != MD_CODE) b->inl = node;
    }
    mdbuf_free(&v);
}

/* Empty paragraphs (from tags around nothing) are removed and tables get
 * their column count. */
static void tidy(md_node *n)
{
    md_node *c = n->first;
    while (c) {
        md_node *next = c->next;
        if (c->from_html && (c->type == MD_PARAGRAPH || c->type == MD_HEADING || c->type == MD_TABLE_CELL ||
                             c->type == MD_STRONG)) {
            /* the space before a closing tag is not content */
            while (c->last && c->last->type == MD_TEXT) {
                md_node *t = c->last;
                while (t->len && t->text[t->len - 1] == ' ') t->text[--t->len] = 0;
                if (t->len) break;
                unlink_node(t);
                md_free(t);
            }
        }
        if (c->type == MD_PARAGRAPH && c->from_html && !c->first) {
            unlink_node(c);
            md_free(c);
        } else {
            tidy(c);
            if (c->type == MD_TABLE) {
                int cols = 0;
                for (md_node *r = c->first; r; r = r->next) {
                    int k = 0;
                    for (md_node *x = r->first; x; x = x->next) k++;
                    if (k > cols) cols = k;
                    for (md_node *x = r->first; x; x = x->next)
                        if (x->header) r->header = 1;
                }
                for (md_node *r = c->first; r; r = r->next) {
                    int k = 0;
                    for (md_node *x = r->first; x; x = x->next) k++;
                    for (; k < cols; k++) {
                        md_node *e = mk(MD_TABLE_CELL, c);
                        e->header = r->header;
                        append(r, e);
                    }
                    for (md_node *x = r->first; x; x = x->next) x->header = r->header;
                }
                c->columns = cols;
                if (!cols) {
                    unlink_node(c);
                    md_free(c);
                }
            }
        }
        c = next;
    }
}

static void build_block(md_node *root)
{
    builder b;
    memset(&b, 0, sizeof b);
    root->from_html = 1; /* expanded: viewers draw the children */
    b.root = root;
    b.container = root;
    b.last_space = 1;
    mdbuf_init(&b.buf);
    const char *s = root->text ? root->text : "";
    size_t n = root->len, i = 0;
    while (i < n) {
        tok t;
        i = next_tok(s, n, i, &t);
        if (b.skip[0]) {
            if (t.kind == TK_END && !strcmp(t.name, b.skip)) b.skip[0] = 0;
            continue;
        }
        if (t.kind == TK_TEXT) add_text(&b, t.text, t.tlen);
        else if (t.kind == TK_START) start_tag(&b, &t);
        else if (t.kind == TK_END) {
            if (!b.pre || !strcmp(t.name, "pre")) pop_to(&b, t.name);
        }
    }
    while (b.depth) pop_entry(&b);
    if (b.pre) finish_text_node(&b, b.pre);
    if (b.code) finish_text_node(&b, b.code);
    mdbuf_free(&b.buf);
    tidy(root);
}

/* ---- inline HTML in paragraphs, headings and cells ------------------------ */

static void remove_node(md_node *n)
{
    unlink_node(n);
    md_free(n);
}

static int tag_of(const md_node *n, tok *t)
{
    if (!n || n->type != MD_HTML_INLINE || !n->text) return 0;
    next_tok(n->text, n->len, 0, t);
    return t->kind == TK_START || t->kind == TK_END;
}

static void fold(md_node *parent);

/* The matching end tag among the following siblings, or NULL. */
static md_node *matching_end(md_node *start, const char *name)
{
    int depth = 0;
    for (md_node *n = start->next; n; n = n->next) {
        tok t;
        if (!tag_of(n, &t) || strcmp(t.name, name) != 0) continue;
        if (t.kind == TK_START) depth++;
        else if (depth-- == 0) return n;
    }
    return NULL;
}

static void fold(md_node *parent)
{
    md_node *c = parent->first;
    while (c) {
        md_node *next = c->next;
        if (c->type != MD_HTML_INLINE) {
            if (c->first) fold(c);
            c = next;
            continue;
        }
        tok t;
        if (!tag_of(c, &t) || t.kind == TK_END) { /* comment, stray end tag */
            remove_node(c);
            c = next;
            continue;
        }
        mdbuf v;
        mdbuf_init(&v);
        if (!strcmp(t.name, "br")) {
            insert_after(c, mk(MD_LINEBREAK, c));
            remove_node(c);
        } else if (!strcmp(t.name, "img")) {
            if (attr(&t, "src", &v) && v.len) {
                md_node *img = mk(MD_IMAGE, c);
                img->url = md_xstrndup(v.data, v.len);
                if (attr(&t, "width", &v) && v.len) img->width = atoi(v.data);
                if (attr(&t, "alt", &v) && v.len) append(img, mk_text(v.data, v.len, c));
                insert_after(c, img);
            }
            remove_node(c);
        } else {
            md_node *end = matching_end(c, t.name);
            int drop = in_list(t.name, dropped);
            md_type ty = inline_type(t.name);
            md_node *w = NULL;
            if (end && !drop && ty != MD_TEXT) {
                if (ty == MD_LINK) {
                    if (attr(&t, "href", &v) && v.len) {
                        w = mk(MD_LINK, c);
                        w->url = md_xstrndup(v.data, v.len);
                    }
                } else {
                    w = mk(ty, c);
                }
            }
            md_node *first = c->next; /* the content between the two tags */
            if (!end) {
                next = c->next; /* unmatched start tag: just drop it */
            } else if (drop) {
                for (md_node *m = first; m && m != end;) {
                    md_node *mn = m->next;
                    remove_node(m);
                    m = mn;
                }
                next = end->next;
            } else if (w) {
                insert_after(c, w);
                for (md_node *m = first; m && m != end;) {
                    md_node *mn = m->next;
                    unlink_node(m);
                    append(w, m);
                    m = mn;
                }
                next = end->next;
            } else {
                next = first == end ? end->next : first; /* tag only changes looks: keep content */
            }
            if (end) remove_node(end);
            remove_node(c);
            if (w && ty == MD_CODE) { /* code keeps text only */
                mdbuf txt;
                mdbuf_init(&txt);
                md_plain_text(w, &txt);
                for (md_node *x = w->first; x;) {
                    md_node *xn = x->next;
                    remove_node(x);
                    x = xn;
                }
                w->text = md_xstrndup(txt.data ? txt.data : "", txt.len);
                w->len = txt.len;
                mdbuf_free(&txt);
            } else if (w) {
                fold(w);
            }
        }
        mdbuf_free(&v);
        c = next;
    }
}

static void walk(md_node *n)
{
    for (md_node *c = n->first; c; c = c->next) {
        if (c->type == MD_HTML_BLOCK) {
            if (!c->first) build_block(c);
        } else if (c->type == MD_PARAGRAPH || c->type == MD_HEADING || c->type == MD_TABLE_CELL) {
            fold(c);
        } else {
            walk(c);
        }
    }
}

void md_expand_html(md_node *doc)
{
    if (doc) walk(doc);
}
