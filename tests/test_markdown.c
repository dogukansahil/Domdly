/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Parser regression tests. Each case renders Markdown to HTML and compares
 * the result with the expected output. Portable C99, runs on every OS.
 */
#include <stdio.h>
#include <string.h>

#include "core/markdown.h"
#include "core/mdbuf.h"
#include "core/render_html.h"
#include "core/render_rtf.h"
#include "core/md_write.h"
#include "core/md_html.h"

typedef struct test_case {
    const char *name;
    const char *md;
    const char *html;
} test_case;

static const test_case cases[] = {
    {"paragraph", "Hello world", "<p>Hello world</p>\n"},
    {"two paragraphs", "one\n\ntwo", "<p>one</p>\n<p>two</p>\n"},
    {"soft break", "a\nb", "<p>a\nb</p>\n"},
    {"hard break spaces", "a  \nb", "<p>a<br />\nb</p>\n"},
    {"hard break backslash", "a\\\nb", "<p>a<br />\nb</p>\n"},
    {"atx headings", "# One\n### Three ###", "<h1 id=\"one\">One</h1>\n<h3 id=\"three\">Three</h3>\n"},
    {"atx needs space", "#nope", "<p>#nope</p>\n"},
    {"setext", "Title\n=====\nSub\n---", "<h1 id=\"title\">Title</h1>\n<h2 id=\"sub\">Sub</h2>\n"},
    {"emphasis", "*a* _b_ **c** __d__", "<p><em>a</em> <em>b</em> <strong>c</strong> <strong>d</strong></p>\n"},
    {"nested emphasis", "***both***", "<p><em><strong>both</strong></em></p>\n"},
    {"intraword underscore", "snake_case_name", "<p>snake_case_name</p>\n"},
    {"intraword star", "a*b*c", "<p>a<em>b</em>c</p>\n"},
    {"unmatched", "*open", "<p>*open</p>\n"},
    {"strike", "~~gone~~", "<p><del>gone</del></p>\n"},
    {"code span", "use `x < y` here", "<p>use <code>x &lt; y</code> here</p>\n"},
    {"code span backticks", "`` a ` b ``", "<p><code>a ` b</code></p>\n"},
    {"escapes", "\\*not\\* \\# \\[x]", "<p>*not* # [x]</p>\n"},
    {"entities", "&copy; &amp; &#65; &#x42; &bogus;", "<p>\xC2\xA9 &amp; A B &amp;bogus;</p>\n"},
    {"inline link", "[Domdly](https://example.com \"Home\")",
     "<p><a href=\"https://example.com\" title=\"Home\">Domdly</a></p>\n"},
    {"link with emphasis", "[*em* text](/x)", "<p><a href=\"/x\"><em>em</em> text</a></p>\n"},
    {"reference link", "[a][ref] and [ref]\n\n[ref]: https://r.example \"T\"",
     "<p><a href=\"https://r.example\" title=\"T\">a</a> and <a href=\"https://r.example\" title=\"T\">ref</a></p>\n"},
    {"image", "![alt *x*](img.png)", "<p><img src=\"img.png\" alt=\"alt x\" /></p>\n"},
    {"autolink", "<https://a.example/b>", "<p><a href=\"https://a.example/b\">https://a.example/b</a></p>\n"},
    {"email autolink", "<me@example.com>", "<p><a href=\"mailto:me@example.com\">me@example.com</a></p>\n"},
    {"bare url", "see https://example.com/x.", "<p>see <a href=\"https://example.com/x\">https://example.com/x</a>.</p>\n"},
    {"www url", "www.example.com", "<p><a href=\"http://www.example.com\">www.example.com</a></p>\n"},
    {"unsafe link", "[x](javascript:alert(1))", "<p><a href=\"\">x</a></p>\n"},
    {"blockquote", "> quote\n> more", "<blockquote>\n<p>quote\nmore</p>\n</blockquote>\n"},
    {"lazy blockquote", "> quote\nlazy", "<blockquote>\n<p>quote\nlazy</p>\n</blockquote>\n"},
    {"nested blockquote", "> a\n>> b", "<blockquote>\n<p>a</p>\n<blockquote>\n<p>b</p>\n</blockquote>\n</blockquote>\n"},
    {"bullet list", "- a\n- b", "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n"},
    {"ordered list", "3. a\n4. b", "<ol start=\"3\">\n<li>a</li>\n<li>b</li>\n</ol>\n"},
    {"loose list", "- a\n\n- b", "<ul>\n<li>\n<p>a</p>\n</li>\n<li>\n<p>b</p>\n</li>\n</ul>\n"},
    {"nested list", "- a\n  - b\n- c", "<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n<li>c</li>\n</ul>\n"},
    {"list marker change", "- a\n* b", "<ul>\n<li>a</li>\n</ul>\n<ul>\n<li>b</li>\n</ul>\n"},
    {"list interrupts paragraph", "text\n- item", "<p>text</p>\n<ul>\n<li>item</li>\n</ul>\n"},
    {"number does not interrupt", "text\n2. no", "<p>text\n2. no</p>\n"},
    {"task list", "- [ ] todo\n- [x] done",
     "<ul class=\"task-list\">\n<li class=\"task-list-item\"><input type=\"checkbox\" disabled /> todo</li>\n"
     "<li class=\"task-list-item\"><input type=\"checkbox\" disabled checked /> done</li>\n</ul>\n"},
    {"list with code", "- item\n\n  ```\n  code\n  ```",
     "<ul>\n<li>\n<p>item</p>\n<pre><code>code\n</code></pre>\n</li>\n</ul>\n"},
    {"fenced code", "```c\nint x;\n```", "<pre><code class=\"language-c\">int x;\n</code></pre>\n"},
    {"tilde fence", "~~~\na\n\nb\n~~~", "<pre><code>a\n\nb\n</code></pre>\n"},
    {"unclosed fence", "```\nopen", "<pre><code>open\n</code></pre>\n"},
    {"indented code", "    code\n    more", "<pre><code>code\nmore\n</code></pre>\n"},
    {"tab expansion", "\tcode", "<pre><code>code\n</code></pre>\n"},
    {"thematic breaks", "***\n- - -\n___", "<hr />\n<hr />\n<hr />\n"},
    {"table", "| a | b |\n|:--|--:|\n| 1 | 2 |\n| 3 |",
     "<table>\n<thead>\n<tr>\n<th style=\"text-align:left\">a</th>\n<th style=\"text-align:right\">b</th>\n</tr>\n"
     "</thead>\n<tbody>\n<tr>\n<td style=\"text-align:left\">1</td>\n<td style=\"text-align:right\">2</td>\n</tr>\n"
     "<tr>\n<td style=\"text-align:left\">3</td>\n<td style=\"text-align:right\"></td>\n</tr>\n</tbody>\n</table>\n"},
    {"table escaped pipe", "a | b\n--- | ---\n`x\\|y` | *z*",
     "<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td><code>x|y</code></td>\n"
     "<td><em>z</em></td>\n</tr>\n</tbody>\n</table>\n"},
    {"html block", "<div align=\"center\">\n<b>hi</b>\n</div>", "<div align=\"center\">\n<b>hi</b>\n</div>\n"},
    {"inline html", "a <kbd>Ctrl</kbd> b", "<p>a <kbd>Ctrl</kbd> b</p>\n"},
    {"html escaping", "a < b & c > d", "<p>a &lt; b &amp; c &gt; d</p>\n"},
    {"utf8 heading slug", "# Merhaba Dünya", "<h1 id=\"merhaba-dünya\">Merhaba Dünya</h1>\n"},
    {"crlf input", "a\r\n\r\nb\r\n", "<p>a</p>\n<p>b</p>\n"},
    {"deep nesting", ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>x", NULL},
    {"empty", "", ""},
};

static int run_case(const test_case *t)
{
    md_node *doc = md_parse(t->md, strlen(t->md));
    mdbuf out;
    mdbuf_init(&out);
    md_render_html(doc, &out);

    /* The RTF renderer must handle every case without crashing either. */
    mdbuf rtf;
    mdbuf_init(&rtf);
    md_rtf_options opt;
    md_rtf_default_options(&opt, 0);
    md_render_rtf(doc, &opt, &rtf);
    int rtf_ok = rtf.len > 10 && memcmp(rtf.data, "{\\rtf1", 6) == 0 && rtf.data[rtf.len - 1] == '}';
    mdbuf_free(&rtf);

    md_free(doc);
    const char *got = out.data ? out.data : "";
    int ok = rtf_ok && (t->html == NULL || strcmp(got, t->html) == 0);
    if (!ok) {
        printf("FAIL: %s\n--- input ---\n%s\n--- expected ---\n%s--- got ---\n%s---\n", t->name, t->md,
               t->html ? t->html : "(anything)", got);
        if (!rtf_ok) printf("(RTF output malformed)\n");
    }
    mdbuf_free(&out);
    return ok;
}

typedef struct marker_log {
    int lines[16];
    int count;
} marker_log;

static void log_marker(void *user, int line)
{
    marker_log *m = (marker_log *)user;
    if (m->count < 16) m->lines[m->count] = line;
    m->count++;
}

/* Block markers let the editor and the reading view scroll to the same
 * block; they must arrive in order with correct source lines. */
static int run_marker_test(void)
{
    const char *md = "# A\n\npara\n\n- x\n- y\n\n> q\n\n| a |\n|---|\n| b |";
    static const int expect[] = {0, 2, 4, 5, 7, 9, 11};
    md_node *doc = md_parse(md, strlen(md));
    md_rtf_options opt;
    md_rtf_default_options(&opt, 0);
    marker_log log;
    memset(&log, 0, sizeof log);
    opt.block_marker = log_marker;
    opt.user = &log;
    mdbuf rtf;
    mdbuf_init(&rtf);
    md_render_rtf(doc, &opt, &rtf);
    int hidden = 0;
    for (const char *p = rtf.data; (p = strstr(p, "{\\v\\u-8191?}")) != NULL; p++) hidden++;
    mdbuf_free(&rtf);
    md_free(doc);
    int ok = log.count == 7 && hidden == 7;
    for (int i = 0; ok && i < 7; i++) ok = log.lines[i] == expect[i];
    if (!ok) {
        printf("FAIL: block markers (%d callbacks, %d in RTF):", log.count, hidden);
        for (int i = 0; i < log.count && i < 16; i++) printf(" %d", log.lines[i]);
        printf("\n");
    }
    return ok;
}

/* ---- Markdown writer: Markdown -> styled runs -> Markdown keeps the meaning */

typedef struct run_list {
    md_run v[128];
    size_t n;
} run_list;

static void collect_runs(const md_node *parent, unsigned flags, const char *url, run_list *out)
{
    for (const md_node *c = parent->first; c && out->n < 128; c = c->next) {
        md_run *r = &out->v[out->n];
        switch (c->type) {
        case MD_TEXT:
            r->text = c->text;
            r->len = c->len;
            r->flags = flags;
            r->url = url;
            out->n++;
            break;
        case MD_SOFTBREAK:
            r->text = " ";
            r->len = 1;
            r->flags = flags;
            r->url = url;
            out->n++;
            break;
        case MD_LINEBREAK:
            r->text = "";
            r->len = 0;
            r->flags = MD_RUN_BREAK;
            r->url = NULL;
            out->n++;
            break;
        case MD_CODE:
            r->text = c->text;
            r->len = c->len;
            r->flags = flags | MD_RUN_CODE;
            r->url = url;
            out->n++;
            break;
        case MD_EMPH: collect_runs(c, flags | MD_RUN_ITALIC, url, out); break;
        case MD_STRONG: collect_runs(c, flags | MD_RUN_BOLD, url, out); break;
        case MD_STRIKE: collect_runs(c, flags | MD_RUN_STRIKE, url, out); break;
        case MD_LINK: collect_runs(c, flags | MD_RUN_LINK, c->url, out); break;
        default: collect_runs(c, flags, url, out); break;
        }
    }
}

static void html_of(const char *md, mdbuf *out)
{
    md_node *doc = md_parse(md, strlen(md));
    md_render_html(doc, out);
    md_free(doc);
}

static int run_writer_tests(void)
{
    static const char *inputs[] = {
        "Plain **bold** *it* ~~st~~ `code` [link](https://x.y) and <https://a.b> end",
        "a*b*c **x _y_ z** under_score 2*3 = 6",
        "Literal \\*stars\\* and \\[brackets\\] and \\<tag> &amp;copy; &copy;",
        "\\# not a heading",
        "1\\. not a list",
        "\\- dash and \\+ plus",
        "`` a`b `` and ` `` `",
        "[**bold link**](http://e.com/a_(b)) after",
        "line one\\\nline two",
        "Tom & Jerry ~ tilde_ and_ _under",
        "***both*** and **bold *nested* end**",
        "**bold **then plain",
        "[a link with `code`](u) and [x](<a b>)",
    };
    int ok = 1;
    for (size_t t = 0; t < sizeof inputs / sizeof inputs[0]; t++) {
        md_node *doc = md_parse(inputs[t], strlen(inputs[t]));
        run_list runs;
        runs.n = 0;
        if (doc->first) collect_runs(doc->first, 0, NULL, &runs);
        mdbuf md, a, b;
        mdbuf_init(&md);
        mdbuf_init(&a);
        mdbuf_init(&b);
        md_write_inline(runs.v, runs.n, 0, NULL, 1, &md);
        html_of(inputs[t], &a);
        html_of(md.data ? md.data : "", &b);
        if (!a.data || !b.data || strcmp(a.data, b.data) != 0) {
            printf("FAIL: writer round trip %u\n  in:   %s\n  out:  %s\n  html: %s  got:  %s", (unsigned)t, inputs[t],
                   md.data ? md.data : "", a.data ? a.data : "", b.data ? b.data : "");
            ok = 0;
        }
        mdbuf_free(&md);
        mdbuf_free(&a);
        mdbuf_free(&b);
        md_free(doc);
    }
    /* table cells escape pipes, breaks carry the container prefix */
    {
        md_run r[3] = {{"a|b", 3, 0, NULL}, {"", 0, MD_RUN_BREAK, NULL}, {"c", 1, MD_RUN_BOLD, NULL}};
        mdbuf o;
        mdbuf_init(&o);
        md_write_inline(r, 3, 1, "> ", 1, &o);
        if (!o.data || strcmp(o.data, "a\\|b\\\n> **c**") != 0) {
            printf("FAIL: writer cell/break: %s\n", o.data ? o.data : "");
            ok = 0;
        }
        mdbuf_free(&o);
    }
    return ok;
}

/* ---- HTML inside Markdown: md_expand_html() --------------------------------- */

static void dump(const md_node *n, mdbuf *o)
{
    static const char *names[] = {"DOC",  "P",    "H",    "QUOTE", "LIST", "LI",   "CODEBLOCK", "HTML", "HR",
                                  "TABLE", "TR",  "TD",   "T",     "SB",   "BR",   "CODE",      "EM",   "STRONG",
                                  "S",     "LINK", "IMG", "HTMLI"};
    mdbuf_puts(o, names[n->type]);
    if (n->type == MD_HEADING) mdbuf_printf(o, "%d", n->level);
    if (n->align == MD_ALIGN_CENTER) mdbuf_puts(o, "^");
    if (n->type == MD_TABLE) mdbuf_printf(o, "%d", n->columns);
    if (n->url) mdbuf_printf(o, "(%s)", n->url);
    if ((n->type == MD_TEXT || n->type == MD_CODE) && n->text) {
        mdbuf_putc(o, '"');
        mdbuf_put(o, n->text, n->len);
        mdbuf_putc(o, '"');
    }
    if (n->first) {
        mdbuf_putc(o, '{');
        for (const md_node *c = n->first; c; c = c->next) {
            dump(c, o);
            if (c->next) mdbuf_putc(o, ',');
        }
        mdbuf_putc(o, '}');
    }
}

static int run_html_tests(void)
{
    static const char *cases[][2] = {
        {"<p align=\"center\">\n  <img src=\"logo.png\" alt=\"Logo\">\n</p>\n",
         "DOC{HTML{P^{IMG(logo.png){T\"Logo\"}}}}"},
        {"<div><b>Bold</b> and <a href=\"https://x.y\">link</a><br>next</div>\n",
         "DOC{HTML{P{STRONG{T\"Bold\"},T\" and \",LINK(https://x.y){T\"link\"},BR,T\"next\"}}}"},
        {"<script>alert(1)</script>\n", "DOC{HTML}"},
        {"Press <kbd>Ctrl</kbd>+<kbd>S</kbd> to <b>save</b><!-- c --> <span>now</span>.",
         "DOC{P{T\"Press \",CODE\"Ctrl\",T\"+\",CODE\"S\",T\" to \",STRONG{T\"save\"},T\" \",T\"now\",T\".\"}}"},
        {"<table><tr><th>A</th><th>B</th></tr><tr><td>1</td></tr></table>\n",
         "DOC{HTML{TABLE2{TR{TD{T\"A\"},TD{T\"B\"}},TR{TD{T\"1\"},TD}}}}"},
        {"<ul><li>one</li><li><i>two</i></li></ul>\n", "DOC{HTML{LIST{LI{P{T\"one\"}},LI{P{EM{T\"two\"}}}}}}"},
        {"a <script>x</script> b", "DOC{P{T\"a \",T\" b\"}}"},
        {"<h2 align=\"center\">Title &amp; more</h2>\n", "DOC{HTML{H2^{T\"Title & more\"}}}"},
    };
    int ok = 1;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        md_node *doc = md_parse(cases[i][0], strlen(cases[i][0]));
        md_expand_html(doc);
        mdbuf o;
        mdbuf_init(&o);
        dump(doc, &o);
        if (!o.data || strcmp(o.data, cases[i][1]) != 0) {
            printf("FAIL: html %u\n  want: %s\n  got:  %s\n", (unsigned)i, cases[i][1], o.data ? o.data : "");
            ok = 0;
        }
        mdbuf_free(&o);
        md_free(doc);
    }
    return ok;
}

/* Block ranges and the raw (edit in place) region of the RTF renderer. */
static int run_raw_test(void)
{
    const char *md = "# Title\n\nfirst para\nsecond line\n\n\n- a\n\n- b\n\nlast";
    md_node *doc = md_parse(md, strlen(md));
    int ok = 1, f = -1, l = -1, n = -1;
    ok &= md_line_count(md, strlen(md)) == 11;
    f = l = 3;
    ok &= md_block_span(doc, md, strlen(md), &f, &l, &n) && f == 2 && l == 3 && n == 6;
    f = l = 7;
    ok &= md_block_span(doc, md, strlen(md), &f, &l, &n) && f == 6 && l == 8 && n == 10;
    f = l = 10;
    ok &= md_block_span(doc, md, strlen(md), &f, &l, &n) && f == 10 && l == 10 && n == 11;
    f = 4;
    l = 5; /* blank lines only */
    ok &= !md_block_span(doc, md, strlen(md), &f, &l, &n) && f == 4 && l == 5 && n == 6;
    {
        /* a paragraph that grew into the line above is taken in whole */
        const char *m2 = "a\nb\n\nc";
        md_node *d2 = md_parse(m2, strlen(m2));
        int g = 1, h = 1, e = 0;
        ok &= md_block_span(d2, m2, strlen(m2), &g, &h, &e) && g == 0 && h == 1 && e == 3;
        md_free(d2);
    }
    if (!ok) printf("FAIL: md_block_span (%d %d %d)\n", f, l, n);

    md_rtf_options opt;
    md_rtf_default_options(&opt, 0);
    marker_log log;
    memset(&log, 0, sizeof log);
    opt.block_marker = log_marker;
    opt.user = &log;
    opt.raw_line = 2;
    opt.raw_end = 6;
    opt.raw_text = "first *para*\nsecond {x}";
    opt.raw_len = strlen(opt.raw_text);
    mdbuf rtf;
    mdbuf_init(&rtf);
    md_render_rtf(doc, &opt, &rtf);
    const char *a = strstr(rtf.data, "\\u-8190?");
    const char *t = strstr(rtf.data, "first *para*\\par");
    const char *t2 = strstr(rtf.data, "second \\{x\\}\\par");
    const char *b = strstr(rtf.data, "\\u-8189?");
    int raw_ok = a && t && t2 && b && a < t && t < t2 && t2 < b && strstr(rtf.data, "Title") &&
                 strstr(rtf.data, "last") && !strstr(rtf.data, "{\\i para}");
    raw_ok &= log.count == 5 && log.lines[0] == 0 && log.lines[1] == 2 && log.lines[2] == 6;
    if (!raw_ok) printf("FAIL: raw region (%d markers)\n", log.count);
    mdbuf_free(&rtf);
    md_free(doc);
    return ok && raw_ok;
}

int main(void)
{
    size_t total = sizeof cases / sizeof cases[0] + 4, passed = 0;
    for (size_t i = 0; i + 4 < total; i++) passed += (size_t)run_case(&cases[i]);
    passed += (size_t)run_html_tests();
    passed += (size_t)run_writer_tests();
    passed += (size_t)run_marker_test();
    passed += (size_t)run_raw_test();
    printf("%zu/%zu tests passed\n", passed, total);
    return passed == total ? 0 : 1;
}
