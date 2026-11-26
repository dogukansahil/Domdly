/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "render_rtf.h"
#include "render_html.h"

#include <stdio.h>
#include <string.h>

/* Color table indices (see write_header). */
enum { CF_TEXT = 1, CF_HEADING, CF_LINK, CF_CODE_FG, CF_CODE_BG, CF_QUOTE, CF_RULE, CF_MUTED, CF_HEAD_BG, CF_PAGE_BG };

#define MAX_IMAGE_BYTES (16u * 1024u * 1024u)

typedef struct rtf_ctx {
    mdbuf *o;
    const md_rtf_options *opt;
    int li;          /* current left indent in twips */
    int quote;       /* block quote depth */
    int tight;       /* inside a tight list item */
    int list_depth;
    int first_block; /* suppress space before the very first block */
    size_t image_bytes;
    const md_node *html_marker; /* an HTML block whose marker is still due */
    int in_html;                /* inside the blocks made from HTML */
} rtf_ctx;

void md_rtf_default_options(md_rtf_options *opt, int dark)
{
    memset(opt, 0, sizeof *opt);
    if (dark) {
        opt->colors.text = 0xE6EDF3;
        opt->colors.heading = 0xF0F6FC;
        opt->colors.link = 0x4493F8;
        opt->colors.code_fg = 0xE6EDF3;
        opt->colors.code_bg = 0x262B33;
        opt->colors.quote = 0x9198A1;
        opt->colors.rule = 0x3D444D;
        opt->colors.muted = 0x9198A1;
        opt->colors.table_head_bg = 0x22272E;
        opt->colors.page_bg = 0x16181D;
    } else {
        opt->colors.text = 0x1F2328;
        opt->colors.heading = 0x1F2328;
        opt->colors.link = 0x0969DA;
        opt->colors.code_fg = 0x1F2328;
        opt->colors.code_bg = 0xF0F2F5;
        opt->colors.quote = 0x59636E;
        opt->colors.rule = 0xD1D9E0;
        opt->colors.muted = 0x6E7781;
        opt->colors.table_head_bg = 0xF6F8FA;
        opt->colors.page_bg = 0xFFFFFF;
    }
    opt->font = "Segoe UI";
    opt->mono_font = "Consolas";
    opt->symbol_font = "Segoe UI Symbol";
    opt->font_half_points = 22;
    opt->width_twips = 9360;
    opt->raw_line = -1;
}

/* ------------------------------------------------------------------------ */
/* Text output                                                              */
/* ------------------------------------------------------------------------ */

static unsigned long next_cp(const char *s, size_t n, size_t *i)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[*i];
    unsigned long cp;
    size_t extra;
    if (c < 0x80) { (*i)++; return c; }
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { (*i)++; return 0xFFFD; }
    for (size_t k = 1; k <= extra; k++) {
        if (*i + k >= n || (u[*i + k] & 0xC0) != 0x80) { (*i)++; return 0xFFFD; }
        cp = (cp << 6) | (u[*i + k] & 0x3F);
    }
    *i += extra + 1;
    return cp;
}

static void put_u16(mdbuf *o, unsigned int unit)
{
    mdbuf_printf(o, "\\u%d?", (int)(short)(unsigned short)unit);
}

static void rtf_text(mdbuf *o, const char *s, size_t n, int keep_newlines)
{
    size_t i = 0;
    while (i < n) {
        unsigned long cp = next_cp(s, n, &i);
        if (cp == '\\' || cp == '{' || cp == '}') {
            mdbuf_putc(o, '\\');
            mdbuf_putc(o, (char)cp);
        } else if (cp == '\n') {
            mdbuf_puts(o, keep_newlines ? "\\line " : " ");
        } else if (cp == '\t') {
            mdbuf_puts(o, "\\tab ");
        } else if (cp < 0x20) {
            /* drop other control characters */
        } else if (cp >= MD_RTF_IMAGE_PLACEHOLDER && cp <= MD_RTF_RAW_END) {
            put_u16(o, 0xFFFD); /* reserved for image placeholders */
        } else if (cp < 0x80) {
            mdbuf_putc(o, (char)cp);
        } else if (cp < 0x10000) {
            put_u16(o, (unsigned int)cp);
        } else {
            cp -= 0x10000;
            put_u16(o, (unsigned int)(0xD800 + (cp >> 10)));
            put_u16(o, (unsigned int)(0xDC00 + (cp & 0x3FF)));
        }
    }
}

static void rtf_str(mdbuf *o, const char *s)
{
    rtf_text(o, s, strlen(s), 0);
}

static void rtf_url(mdbuf *o, const char *url)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        unsigned char c = *p;
        if (c == '\\' || c == '{' || c == '}') {
            mdbuf_putc(o, '\\');
            mdbuf_putc(o, (char)c);
        } else if (c <= ' ' || c == '"' || c >= 0x7F) {
            mdbuf_putc(o, '%');
            mdbuf_putc(o, hex[c >> 4]);
            mdbuf_putc(o, hex[c & 15]);
        } else {
            mdbuf_putc(o, (char)c);
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Images                                                                   */
/* ------------------------------------------------------------------------ */

static unsigned long be32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | p[3];
}

static unsigned int be16(const unsigned char *p)
{
    return ((unsigned int)p[0] << 8) | p[1];
}

/* kind: 1 = PNG, 2 = JPEG */
static int image_info(const unsigned char *d, size_t n, long *w, long *h, int *kind)
{
    if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0 && memcmp(d + 12, "IHDR", 4) == 0) {
        *w = (long)be32(d + 16);
        *h = (long)be32(d + 20);
        *kind = 1;
        return *w > 0 && *h > 0;
    }
    if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xFF) { i++; continue; }
            unsigned char m = d[i + 1];
            if (m == 0xFF) { i++; continue; }
            if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
            unsigned int len = be16(d + i + 2);
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                *h = (long)be16(d + i + 5);
                *w = (long)be16(d + i + 7);
                *kind = 2;
                return *w > 0 && *h > 0;
            }
            if (len < 2) return 0;
            i += 2 + len;
        }
    }
    return 0;
}

static unsigned long le32(const unsigned char *p)
{
    return ((unsigned long)p[3] << 24) | ((unsigned long)p[2] << 16) | ((unsigned long)p[1] << 8) | p[0];
}

int md_image_size(const unsigned char *d, size_t n, long *w, long *h)
{
    int kind;
    if (image_info(d, n, w, h, &kind)) return 1;
    if (n >= 10 && (memcmp(d, "GIF87a", 6) == 0 || memcmp(d, "GIF89a", 6) == 0)) {
        *w = (long)(d[6] | (d[7] << 8));
        *h = (long)(d[8] | (d[9] << 8));
        return *w > 0 && *h > 0;
    }
    if (n >= 26 && d[0] == 'B' && d[1] == 'M') {
        long bw = (long)le32(d + 18), bh = (long)le32(d + 22);
        if (bh < 0) bh = -bh;
        *w = bw;
        *h = bh;
        return bw > 0 && bh > 0;
    }
    return 0;
}

static int avail_width(const rtf_ctx *C)
{
    int w = (C->opt->width_twips > 0 ? C->opt->width_twips : 9360) - C->li;
    return w < 1440 ? 1440 : w;
}

static int emit_picture(rtf_ctx *C, const md_node *img)
{
    if (!img->url || !md_url_is_safe(img->url, 1)) return 0;
    if (C->opt->image_placeholder) {
        mdbuf alt;
        mdbuf_init(&alt);
        md_plain_text(img, &alt);
        int maxw = avail_width(C) - 60;
        if (img->width > 0 && img->width * 15 < maxw) maxw = img->width * 15; /* width="..." from HTML */
        int ok = C->opt->image_placeholder(C->opt->user, img->url, alt.data ? alt.data : "", maxw);
        mdbuf_free(&alt);
        if (!ok) return 0;
        put_u16(C->o, MD_RTF_IMAGE_PLACEHOLDER);
        return 1;
    }
    if (!C->opt->load_image) return 0;
    const unsigned char *data = NULL;
    size_t len = 0;
    if (!C->opt->load_image(C->opt->user, img->url, &data, &len) || !data || !len) return 0;
    if (len > MAX_IMAGE_BYTES || C->image_bytes + len > 4 * MAX_IMAGE_BYTES) return 0;
    long w, h;
    int kind;
    if (!image_info(data, len, &w, &h, &kind)) return 0;
    C->image_bytes += len;

    /* 96 DPI source pixels -> twips, scaled down to fit the line. */
    double gw = (double)w * 15.0, gh = (double)h * 15.0;
    double maxw = (double)avail_width(C) - 60.0;
    if (gw > maxw) {
        gh = gh * maxw / gw;
        gw = maxw;
    }
    mdbuf *o = C->o;
    mdbuf_printf(o, "{\\pict%s\\picw%ld\\pich%ld\\picwgoal%ld\\pichgoal%ld\n",
                 kind == 1 ? "\\pngblip" : "\\jpegblip", w, h, (long)gw, (long)gh);
    static const char hex[] = "0123456789abcdef";
    mdbuf_reserve(o, len * 2 + len / 64 + 4);
    for (size_t i = 0; i < len; i++) {
        o->data[o->len++] = hex[data[i] >> 4];
        o->data[o->len++] = hex[data[i] & 15];
        if ((i & 63) == 63) o->data[o->len++] = '\n';
    }
    o->data[o->len] = '\0';
    mdbuf_puts(o, "}");
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Inlines                                                                  */
/* ------------------------------------------------------------------------ */

static void inlines(rtf_ctx *C, const md_node *parent);

static void inline_node(rtf_ctx *C, const md_node *n)
{
    mdbuf *o = C->o;
    switch (n->type) {
    case MD_TEXT:
        rtf_text(o, n->text, n->len, 0);
        break;
    case MD_SOFTBREAK:
        mdbuf_putc(o, ' ');
        break;
    case MD_LINEBREAK:
        mdbuf_puts(o, "\\line ");
        break;
    case MD_CODE:
        mdbuf_printf(o, "{\\f1\\fs%d\\highlight%d\\cf%d \\u160?", C->opt->font_half_points - 2, CF_CODE_BG, CF_CODE_FG);
        rtf_text(o, n->text, n->len, 0);
        mdbuf_puts(o, "\\u160?}");
        break;
    case MD_EMPH:
        mdbuf_puts(o, "{\\i ");
        inlines(C, n);
        mdbuf_putc(o, '}');
        break;
    case MD_STRONG:
        mdbuf_puts(o, "{\\b ");
        inlines(C, n);
        mdbuf_putc(o, '}');
        break;
    case MD_STRIKE:
        mdbuf_puts(o, "{\\strike ");
        inlines(C, n);
        mdbuf_putc(o, '}');
        break;
    case MD_LINK:
        if (n->url && *n->url && md_url_is_safe(n->url, 0)) {
            /* RichEdit turns a link whose text equals its URL into a plain
             * auto-URL and drops our colour. A trailing space inside the
             * instruction keeps it a "friendly" link; the host trims it. */
            int same = n->first && n->first == n->last && n->first->type == MD_TEXT &&
                       strlen(n->url) == n->first->len && memcmp(n->url, n->first->text, n->first->len) == 0;
            mdbuf_puts(o, "{\\field{\\*\\fldinst{HYPERLINK \"");
            rtf_url(o, n->url);
            mdbuf_printf(o, "%s\"}}{\\fldrslt{\\ul\\cf%d ", same ? " " : "", CF_LINK);
            inlines(C, n);
            mdbuf_puts(o, "}}}");
        } else {
            inlines(C, n);
        }
        break;
    case MD_IMAGE:
        if (!emit_picture(C, n)) {
            mdbuf alt;
            mdbuf_init(&alt);
            md_plain_text(n, &alt);
            int linked = n->url && *n->url && md_url_is_safe(n->url, 1);
            if (linked) {
                mdbuf_puts(o, "{\\field{\\*\\fldinst{HYPERLINK \"");
                rtf_url(o, n->url);
                mdbuf_puts(o, "\"}}{\\fldrslt{");
            }
            mdbuf_printf(o, "{\\cf%d [", CF_MUTED);
            rtf_str(o, "Image");
            if (alt.len) {
                mdbuf_puts(o, ": ");
                rtf_text(o, alt.data, alt.len, 0);
            }
            mdbuf_puts(o, "]}");
            if (linked) mdbuf_puts(o, "}}}");
            mdbuf_free(&alt);
        }
        break;
    case MD_HTML_INLINE:
        if (n->len >= 3 && (n->text[1] == 'b' || n->text[1] == 'B') && (n->text[2] == 'r' || n->text[2] == 'R') &&
            (n->len == 4 || n->text[3] == ' ' || n->text[3] == '/'))
            mdbuf_puts(o, "\\line ");
        break;
    default:
        inlines(C, n);
        break;
    }
}

static void inlines(rtf_ctx *C, const md_node *parent)
{
    for (const md_node *c = parent->first; c; c = c->next) inline_node(C, c);
}

/* ------------------------------------------------------------------------ */
/* Blocks                                                                   */
/* ------------------------------------------------------------------------ */

static void para_open(rtf_ctx *C, int sb, int sa, int li, int fi, int color, const char *extra)
{
    if (C->first_block) {
        sb = 0;
        C->first_block = 0;
    }
    mdbuf_printf(C->o, "\\pard\\plain\\sb%d\\sa%d\\li%d\\fi%d\\sl276\\slmult1%s\\f0\\fs%d\\cf%d ", sb, sa, li, fi,
                 extra ? extra : "", C->opt->font_half_points, color);
}

/* Hidden source-position marker at the start of a block. */
static void marker(rtf_ctx *C, const md_node *n)
{
    if (!C->opt->block_marker) return;
    /* blocks made from an HTML block share one marker: the HTML block's */
    if (C->html_marker) {
        n = C->html_marker;
        C->html_marker = NULL;
    } else if (C->in_html) {
        return;
    }
    if (C->opt->block_node) C->opt->block_node(C->opt->user, n);
    C->opt->block_marker(C->opt->user, n->line);
    mdbuf_puts(C->o, "{\\v\\u-8191?}");
}

static int text_color(const rtf_ctx *C)
{
    return C->quote ? CF_QUOTE : CF_TEXT;
}

static void spacer(rtf_ctx *C, int twips)
{
    mdbuf_printf(C->o, "\\pard\\plain\\sb0\\sa0\\sl-%d\\slmult0\\fs2\\par\n", twips < 20 ? 20 : twips);
    C->first_block = 0;
}

/* A one-cell table: used for code blocks and horizontal rules so that the
 * background / border spans the whole line. */
static void box_open(rtf_ctx *C, const char *cell_props)
{
    int left = C->li;
    int right = left + avail_width(C);
    mdbuf_printf(C->o, "\\trowd\\trgaph180\\trleft%d%s\\cellx%d\n", left, cell_props, right);
    C->first_block = 0;
}

static void block_any(rtf_ctx *C, const md_node *n);

#define BORDER(side, width, color) "\\clbrdr" side "\\brdrs\\brdrw" width "\\brdrcf" color

/* Thin horizontal line across the available width. */
static void rule_line(rtf_ctx *C, int space_after)
{
    box_open(C, BORDER("t", "10", "10") BORDER("l", "10", "10") BORDER("r", "10", "10") BORDER("b", "15", "7"));
    mdbuf_puts(C->o, "\\pard\\intbl\\plain\\fs2\\cell\\row\n");
    spacer(C, space_after);
}

static int has_image(const md_node *n)
{
    for (const md_node *c = n->first; c; c = c->next)
        if (c->type == MD_IMAGE || (c->first && has_image(c))) return 1;
    return 0;
}

static void block_paragraph(rtf_ctx *C, const md_node *n)
{
    /* Proportional line spacing would also stretch picture lines; HTML
     * (<p align="center">, <center>) may centre or right-align. */
    char extra[16] = "";
    if (has_image(n)) strcat(extra, "\\sl0");
    if (n->align == MD_ALIGN_CENTER) strcat(extra, "\\qc");
    else if (n->align == MD_ALIGN_RIGHT) strcat(extra, "\\qr");
    para_open(C, 0, C->tight ? 40 : 200, C->li, 0, text_color(C), extra[0] ? extra : NULL);
    marker(C, n);
    inlines(C, n);
    mdbuf_puts(C->o, "\\par\n");
}

static void block_heading(rtf_ctx *C, const md_node *n)
{
    static const int sizes[6] = {44, 36, 30, 26, 23, 22};
    int lvl = n->level < 1 ? 1 : n->level > 6 ? 6 : n->level;
    int sb = lvl <= 2 ? 360 : 280;
    int color = lvl == 6 ? CF_MUTED : CF_HEADING;
    if (C->first_block) {
        sb = 0;
        C->first_block = 0;
    }
    mdbuf_printf(C->o, "\\pard\\plain\\sb%d\\sa%d\\li%d\\keepn%s\\f0\\fs%d\\b\\cf%d ", sb, lvl <= 2 ? 80 : 120, C->li,
                 n->align == MD_ALIGN_CENTER ? "\\qc" : n->align == MD_ALIGN_RIGHT ? "\\qr" : "", sizes[lvl - 1], color);
    marker(C, n);
    inlines(C, n);
    mdbuf_puts(C->o, "\\par\n");
    if (lvl <= 2) rule_line(C, 160);
}

static void block_code(rtf_ctx *C, const md_node *n)
{
    size_t len = n->len;
    if (len && n->text[len - 1] == '\n') len--;
    mdbuf_printf(C->o, "\\pard\\plain\\sb0\\sa0\\fs4\\par\n");
    box_open(C, "\\clcbpat5" BORDER("t", "10", "5") BORDER("l", "10", "5") BORDER("b", "10", "5") BORDER("r", "10", "5"));
    mdbuf_printf(C->o, "\\pard\\intbl\\plain\\sb100\\sa100\\f1\\fs%d\\cf%d ", C->opt->font_half_points - 2, CF_CODE_FG);
    marker(C, n);
    rtf_text(C->o, n->text ? n->text : "", len, 1);
    mdbuf_puts(C->o, "\\cell\\row\n");
    spacer(C, 200);
}

static void block_rule(rtf_ctx *C)
{
    spacer(C, 120);
    rule_line(C, 240);
}

/* A run of quoted paragraphs [first, end) drawn as a cell with a thick left
 * bar. RichEdit does not paint paragraph borders, but it does paint table
 * cell borders. */
static void quote_bar(rtf_ctx *C, const md_node *first, const md_node *end)
{
    mdbuf_printf(C->o, "\\pard\\plain\\sb0\\sa0\\fs4\\par\n");
    box_open(C, BORDER("l", "45", "7") BORDER("t", "10", "10") BORDER("b", "10", "10") BORDER("r", "10", "10"));
    for (const md_node *p = first; p != end; p = p->next) {
        int last = p->next == end;
        mdbuf_printf(C->o, "\\pard\\intbl\\plain\\sb%d\\sa%d\\sl276\\slmult1\\f0\\fs%d\\cf%d ", p == first ? 40 : 0,
                     last ? 40 : 160, C->opt->font_half_points, CF_QUOTE);
        marker(C, p);
        inlines(C, p);
        mdbuf_puts(C->o, last ? "\\cell\\row\n" : "\\par\n");
    }
    spacer(C, 160);
}

static void block_quote(rtf_ctx *C, const md_node *q)
{
    int saved_li = C->li, saved_tight = C->tight;
    C->quote++;
    C->tight = 0;
    const md_node *c = q->first;
    while (c) {
        if (c->type == MD_PARAGRAPH) {
            const md_node *end = c;
            while (end && end->type == MD_PARAGRAPH) end = end->next;
            quote_bar(C, c, end);
            c = end;
        } else {
            C->li = saved_li + 400;
            block_any(C, c);
            C->li = saved_li;
            c = c->next;
        }
    }
    C->quote--;
    C->tight = saved_tight;
}

static size_t cell_chars(const md_node *cell)
{
    mdbuf t;
    mdbuf_init(&t);
    md_plain_text(cell, &t);
    size_t count = 0;
    for (size_t i = 0; i < t.len; i++)
        if (((unsigned char)t.data[i] & 0xC0) != 0x80) count++;
    mdbuf_free(&t);
    return count;
}

static void block_table(rtf_ctx *C, const md_node *n)
{
    int cols = n->columns;
    if (cols <= 0) return;
    if (cols > 64) cols = 64;
    long widths[64];
    long natural[64];
    for (int c = 0; c < cols; c++) natural[c] = 3;
    for (const md_node *row = n->first; row; row = row->next) {
        int c = 0;
        for (const md_node *cell = row->first; cell && c < cols; cell = cell->next, c++) {
            long k = (long)cell_chars(cell);
            if (k > 60) k = 60;
            if (k > natural[c]) natural[c] = k;
        }
    }
    long char_tw = C->opt->font_half_points * 5L;
    long total = 0;
    for (int c = 0; c < cols; c++) {
        widths[c] = natural[c] * char_tw + 400;
        total += widths[c];
    }
    long avail = avail_width(C);
    if (total > avail) {
        long sum = 0;
        for (int c = 0; c < cols; c++) {
            widths[c] = widths[c] * avail / total;
            if (widths[c] < 500) widths[c] = 500;
            sum += widths[c];
        }
        total = sum;
    }

    mdbuf *o = C->o;
    mdbuf_printf(o, "\\pard\\plain\\sb0\\sa0\\fs4\\par\n");
    C->first_block = 0;
    for (const md_node *row = n->first; row; row = row->next) {
        mdbuf_printf(o, "\\trowd\\trgaph108\\trleft%d", C->li);
        long pos = C->li;
        for (int c = 0; c < cols; c++) {
            pos += widths[c];
            mdbuf_puts(o, "\\clbrdrt\\brdrs\\brdrw10\\brdrcf7\\clbrdrl\\brdrs\\brdrw10\\brdrcf7"
                          "\\clbrdrb\\brdrs\\brdrw10\\brdrcf7\\clbrdrr\\brdrs\\brdrw10\\brdrcf7");
            if (row->header) mdbuf_printf(o, "\\clcbpat%d", CF_HEAD_BG);
            mdbuf_printf(o, "\\cellx%ld", pos);
        }
        mdbuf_putc(o, '\n');
        int c = 0;
        for (const md_node *cell = row->first; cell && c < cols; cell = cell->next, c++) {
            const char *al = cell->align == MD_ALIGN_CENTER ? "\\qc" : cell->align == MD_ALIGN_RIGHT ? "\\qr" : "\\ql";
            mdbuf_printf(o, "\\pard\\intbl\\plain\\sb60\\sa60%s\\f0\\fs%d\\cf%d%s ", al, C->opt->font_half_points,
                         text_color(C), row->header ? "\\b" : "");
            if (c == 0) marker(C, row);
            inlines(C, cell);
            mdbuf_puts(o, "\\cell");
        }
        for (; c < cols; c++) mdbuf_puts(o, "\\pard\\intbl\\plain\\cell");
        mdbuf_puts(o, "\\row\n");
    }
    spacer(C, 200);
}

static void block_list(rtf_ctx *C, const md_node *list)
{
    static const unsigned int bullets[3] = {0x2022, 0x25E6, 0x25AA};
    int saved_li = C->li, saved_tight = C->tight;
    int number = list->ordered ? list->start : 0;
    int digits = 1;
    for (int v = list->start + 64; v >= 10; v /= 10) digits++;
    int hang = list->ordered ? 200 + digits * 130 : 300;
    int L = saved_li + 220 + hang;

    C->list_depth++;
    for (const md_node *item = list->first; item; item = item->next, number++) {
        mdbuf bullet;
        mdbuf_init(&bullet);
        if (item->task) {
            mdbuf_puts(&bullet, "{\\f2 ");
            put_u16(&bullet, item->task == MD_TASK_DONE ? 0x2611 : 0x2610);
            mdbuf_puts(&bullet, "}");
        } else if (list->ordered) {
            mdbuf_printf(&bullet, "%d%c", number, list->marker == ')' ? ')' : '.');
        } else {
            put_u16(&bullet, bullets[(C->list_depth - 1) % 3]);
        }
        C->tight = list->tight;
        const md_node *child = item->first;
        char tabstop[32];
        sprintf(tabstop, "\\tx%d", L);
        int sa = list->tight ? 40 : 200;
        if (child && child->type == MD_PARAGRAPH) {
            para_open(C, 0, sa, L, -hang, text_color(C), tabstop);
            marker(C, item);
            mdbuf_put(C->o, bullet.data, bullet.len);
            mdbuf_puts(C->o, "\\tab ");
            inlines(C, child);
            mdbuf_puts(C->o, "\\par\n");
            child = child->next;
        } else {
            para_open(C, 0, sa, L, -hang, text_color(C), tabstop);
            marker(C, item);
            mdbuf_put(C->o, bullet.data, bullet.len);
            mdbuf_puts(C->o, "\\tab\\par\n"); /* the tab keeps empty items like the others */
        }
        mdbuf_free(&bullet);
        C->li = L;
        for (; child; child = child->next) block_any(C, child);
        C->li = saved_li;
    }
    C->list_depth--;
    C->li = saved_li;
    C->tight = saved_tight;
    if (C->list_depth == 0 && list->tight) spacer(C, 160);
}


static void block_html(rtf_ctx *C, const md_node *n)
{
    if (n->from_html) { /* shown through the nodes md_expand_html() made */
        C->html_marker = n;
        C->in_html++;
        for (const md_node *c = n->first; c; c = c->next) block_any(C, c);
        C->in_html--;
        C->html_marker = NULL;
        return;
    }
    size_t len = n->len;
    while (len && n->text[len - 1] == '\n') len--;
    para_open(C, 0, 200, C->li, 0, CF_MUTED, NULL);
    mdbuf_printf(C->o, "\\f1\\fs%d ", C->opt->font_half_points - 2);
    marker(C, n);
    rtf_text(C->o, n->text, len, 1);
    mdbuf_puts(C->o, "\\par\n");
}

static void block_any(rtf_ctx *C, const md_node *n)
{
    switch (n->type) {
    case MD_PARAGRAPH: block_paragraph(C, n); break;
    case MD_HEADING: block_heading(C, n); break;
    case MD_CODE_BLOCK: block_code(C, n); break;
    case MD_THEMATIC_BREAK: block_rule(C); break;
    case MD_TABLE: block_table(C, n); break;
    case MD_LIST: block_list(C, n); break;
    case MD_HTML_BLOCK: block_html(C, n); break;
    case MD_BLOCKQUOTE: block_quote(C, n); break;
    default:
        break;
    }
}

/* The source being edited in place: one paragraph per source line between
 * two hidden sentinel paragraphs. The sentinels live in paragraphs of their
 * own so that text typed at either edge never inherits hidden formatting. */
static void raw_region(rtf_ctx *C)
{
    const md_rtf_options *opt = C->opt;
    int sb = C->first_block ? 0 : 120;
    C->first_block = 0;
    mdbuf_printf(C->o, "\\pard\\plain\\sb%d\\sa0\\sl-20\\slmult0\\li%d\\fs2 {\\v", sb, C->li);
    if (opt->block_marker) {
        opt->block_marker(opt->user, opt->raw_line);
        put_u16(C->o, MD_RTF_BLOCK_MARKER);
    }
    put_u16(C->o, MD_RTF_RAW_START);
    mdbuf_puts(C->o, "}\\par\n");
    mdbuf_printf(C->o, "\\pard\\plain\\sb0\\sa0\\sl276\\slmult1\\li%d\\f1\\fs%d\\cf%d ", C->li,
                 opt->font_half_points - 1, CF_CODE_FG);
    const char *s = opt->raw_text ? opt->raw_text : "";
    size_t n = opt->raw_text ? opt->raw_len : 0, i = 0;
    for (;;) {
        size_t j = i;
        while (j < n && s[j] != '\n') j++;
        size_t e = j;
        if (e > i && s[e - 1] == '\r') e--;
        rtf_text(C->o, s + i, e - i, 0);
        mdbuf_puts(C->o, "\\par\n");
        if (j >= n) break;
        i = j + 1;
    }
    mdbuf_printf(C->o, "\\pard\\plain\\sb0\\sa160\\sl-20\\slmult0\\li%d\\fs2 {\\v", C->li);
    put_u16(C->o, MD_RTF_RAW_END);
    mdbuf_puts(C->o, "}\\par\n");
}

/* Top level: like blocks(), with the raw region (if any) in place of the
 * blocks it covers. */
static void top_blocks(rtf_ctx *C, const md_node *doc)
{
    const md_rtf_options *opt = C->opt;
    int raw = opt->raw_line >= 0, done = 0;
    for (const md_node *c = doc->first; c; c = c->next) {
        if (raw && !done && c->line >= opt->raw_line) {
            raw_region(C);
            done = 1;
        }
        if (raw && c->line >= opt->raw_line && c->line < opt->raw_end) continue;
        block_any(C, c);
    }
    if (raw && !done) raw_region(C);
}

static void color_entry(mdbuf *o, unsigned long rgb)
{
    mdbuf_printf(o, "\\red%lu\\green%lu\\blue%lu;", (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

void md_render_rtf(const md_node *doc, const md_rtf_options *opt, mdbuf *out)
{
    rtf_ctx C;
    memset(&C, 0, sizeof C);
    C.o = out;
    C.opt = opt;
    C.first_block = 1;

    mdbuf_printf(out, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1\\deflang1033\n{\\fonttbl{\\f0\\fswiss\\fcharset0 %s;}"
                      "{\\f1\\fmodern\\fcharset0 %s;}{\\f2\\fnil\\fcharset0 %s;}}\n{\\colortbl;",
                 opt->font, opt->mono_font, opt->symbol_font);
    const md_rtf_colors *k = &opt->colors;
    color_entry(out, k->text);
    color_entry(out, k->heading);
    color_entry(out, k->link);
    color_entry(out, k->code_fg);
    color_entry(out, k->code_bg);
    color_entry(out, k->quote);
    color_entry(out, k->rule);
    color_entry(out, k->muted);
    color_entry(out, k->table_head_bg);
    color_entry(out, k->page_bg);
    mdbuf_puts(out, "}\n\\viewkind4\n");

    top_blocks(&C, doc);
    mdbuf_puts(out, "\\pard\\plain\\fs2\\par\n}");
}
