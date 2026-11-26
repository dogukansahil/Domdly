/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * HTML renderer (portable C99). Used for "Export as HTML" and for tests.
 */
#ifndef DOMDLY_RENDER_HTML_H
#define DOMDLY_RENDER_HTML_H

#include "markdown.h"
#include "mdbuf.h"

/* Renders the document body (no <html>/<head>). */
void md_render_html(const md_node *doc, mdbuf *out);

/* Renders a complete, self-contained HTML5 page with a built-in style sheet
 * that follows the reader's light/dark preference. title is UTF-8. */
void md_render_html_page(const md_node *doc, const char *title, mdbuf *out);

/* Returns 1 when a link destination is considered safe to follow or emit
 * (rejects javascript:, vbscript:, data: other than images, etc.). */
int md_url_is_safe(const char *url, int is_image);

#endif
