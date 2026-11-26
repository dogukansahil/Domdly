/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * md2html: tiny command line front end for the portable core.
 * Converts a Markdown file to a standalone HTML page. It builds on every
 * platform with a C99 compiler and doubles as a portability check.
 *
 * Usage: md2html [--body] <input.md> [output.html]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/markdown.h"
#include "core/mdbuf.h"
#include "core/render_html.h"

static int read_all(FILE *f, mdbuf *out)
{
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) mdbuf_put(out, chunk, n);
    return !ferror(f);
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

int main(int argc, char **argv)
{
    int body_only = 0, arg = 1;
    if (arg < argc && strcmp(argv[arg], "--body") == 0) {
        body_only = 1;
        arg++;
    }
    if (argc - arg < 1 || argc - arg > 2) {
        fprintf(stderr, "usage: md2html [--body] <input.md> [output.html]\n");
        return 2;
    }
    const char *in_path = argv[arg];
    FILE *in = strcmp(in_path, "-") == 0 ? stdin : fopen(in_path, "rb");
    if (!in) {
        perror(in_path);
        return 1;
    }
    mdbuf src;
    mdbuf_init(&src);
    int ok = read_all(in, &src);
    if (in != stdin) fclose(in);
    if (!ok) {
        fprintf(stderr, "md2html: read error\n");
        return 1;
    }

    md_node *doc = md_parse(src.data ? src.data : "", src.len);
    mdbuf html;
    mdbuf_init(&html);
    if (body_only) {
        md_render_html(doc, &html);
    } else {
        char title[256];
        snprintf(title, sizeof title, "%s", base_name(in_path));
        char *dot = strrchr(title, '.');
        if (dot) *dot = '\0';
        md_render_html_page(doc, title, &html);
    }
    md_free(doc);

    FILE *out = (argc - arg == 2) ? fopen(argv[arg + 1], "wb") : stdout;
    if (!out) {
        perror(argv[arg + 1]);
        return 1;
    }
    fwrite(html.data ? html.data : "", 1, html.len, out);
    if (out != stdout) fclose(out);
    mdbuf_free(&html);
    mdbuf_free(&src);
    return 0;
}
