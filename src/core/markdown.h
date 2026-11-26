/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Markdown parser. Turns UTF-8 Markdown text into a document tree.
 * Supports CommonMark-style blocks and inlines plus the common GitHub
 * extensions (tables, task lists, strikethrough, bare URL autolinks).
 *
 * Portable C99: this module must never include platform headers so it can
 * be shared by the Windows, Linux and macOS front ends.
 */
#ifndef DOMDLY_MARKDOWN_H
#define DOMDLY_MARKDOWN_H

#include <stddef.h>

typedef enum md_type {
    /* blocks */
    MD_DOCUMENT,
    MD_PARAGRAPH,
    MD_HEADING,
    MD_BLOCKQUOTE,
    MD_LIST,
    MD_ITEM,
    MD_CODE_BLOCK,
    MD_HTML_BLOCK,
    MD_THEMATIC_BREAK,
    MD_TABLE,
    MD_TABLE_ROW,
    MD_TABLE_CELL,
    /* inlines */
    MD_TEXT,
    MD_SOFTBREAK,
    MD_LINEBREAK,
    MD_CODE,
    MD_EMPH,
    MD_STRONG,
    MD_STRIKE,
    MD_LINK,
    MD_IMAGE,
    MD_HTML_INLINE
} md_type;

typedef enum md_align {
    MD_ALIGN_NONE,
    MD_ALIGN_LEFT,
    MD_ALIGN_CENTER,
    MD_ALIGN_RIGHT
} md_align;

enum { MD_TASK_NONE = 0, MD_TASK_OPEN = 1, MD_TASK_DONE = 2 };

typedef struct md_node {
    md_type type;
    struct md_node *parent, *first, *last, *prev, *next;

    char  *text;   /* TEXT, CODE, CODE_BLOCK, HTML_*: literal content (not NUL-safe, use len) */
    size_t len;
    char  *url;    /* LINK, IMAGE */
    char  *title;  /* LINK, IMAGE (may be NULL) */
    char  *info;   /* CODE_BLOCK: fence info string (may be NULL) */

    int level;     /* HEADING: 1..6 */
    int ordered;   /* LIST */
    int start;     /* LIST: first number of an ordered list */
    int tight;     /* LIST */
    char marker;   /* LIST: '-', '+', '*', '.' or ')' */
    int task;      /* ITEM: MD_TASK_* */
    md_align align;/* TABLE_CELL; PARAGRAPH and HEADING made from HTML */
    int header;    /* TABLE_ROW / TABLE_CELL: part of the header row */
    int columns;   /* TABLE */
    int line;      /* blocks: 0-based source line where the block starts */
    int end_line;  /* leaf blocks and table rows: last source line of the block */
    int from_html; /* made from HTML by md_expand_html() */
    int width;     /* IMAGE from HTML: width="..." in pixels, 0 = natural */
} md_node;

/* Parses len bytes of UTF-8 text. Never returns NULL. */
md_node *md_parse(const char *src, size_t len);

/* Frees a node and all of its descendants (siblings are not touched). */
void md_free(md_node *node);

/* Appends the plain text of all descendants of node to out (used for image
 * alt text, heading anchors and table sizing). Returns the new length. */
struct mdbuf;
void md_plain_text(const md_node *node, struct mdbuf *out);

/* Writes a GitHub-style anchor slug for a heading into out. */
void md_heading_slug(const md_node *heading, struct mdbuf *out);

/* Widens the source line range [*first, *last] of doc (parsed from text) to
 * whole top-level blocks: every block with content on those lines is
 * covered from its first to its last non-blank line. *end becomes the line
 * where the next top-level block starts (or the line count), so the blocks
 * starting on lines *first .. *end - 1 are exactly the ones in the range.
 * Returns 0 when no block touches the range. */
int md_block_span(const md_node *doc, const char *text, size_t len, int *first, int *last, int *end);

/* Decodes the HTML entity at s[0] == '&' into out. Returns the bytes
 * consumed, or 0 when s does not start with a known entity. */
size_t md_decode_entity(const char *s, size_t n, struct mdbuf *out);

/* Number of source lines in text ('
', '
' or '
' separated). */
int md_line_count(const char *text, size_t len);

#endif
