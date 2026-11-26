/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * HTML inside Markdown (portable C99). Markdown files often contain bits of
 * HTML: a centred logo, <b>, <kbd>, <br>, a <details> box, a small table.
 * md_expand_html() turns that HTML into ordinary document nodes so every
 * viewer can show it, without a browser engine and without running or
 * loading anything:
 *
 *  - formatting, links, local pictures, line breaks, headings, paragraphs,
 *    alignment, lists, block quotes, <pre>, tables and <details> are shown;
 *  - <script>, <style>, <iframe>, <object>, <embed>, forms and comments are
 *    dropped together with their content;
 *  - any other tag is ignored and only its text is kept.
 *
 * The raw HTML stays in the MD_HTML_BLOCK / MD_HTML_INLINE text that the
 * HTML export writes out unchanged; the viewers draw the new nodes instead.
 */
#ifndef DOMDLY_MD_HTML_H
#define DOMDLY_MD_HTML_H

#include "markdown.h"

/* HTML blocks get child blocks; inline HTML in paragraphs, headings and
 * table cells is replaced by emphasis, links, code and so on. All new
 * nodes have from_html set. Call once, right after md_parse(). */
void md_expand_html(md_node *doc);

#endif
