/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Markdown writer (portable C99): turns styled text, as a rich text editor
 * holds it, back into Markdown. This is what lets the Read view be edited
 * like a word processor: the host reads the formatted runs of a changed
 * block and writes the Markdown for them into the source.
 */
#ifndef DOMDLY_MD_WRITE_H
#define DOMDLY_MD_WRITE_H

#include <stddef.h>

#include "mdbuf.h"

enum {
    MD_RUN_BOLD = 1,
    MD_RUN_ITALIC = 2,
    MD_RUN_STRIKE = 4,
    MD_RUN_CODE = 8,   /* inline code; other flags are ignored inside it */
    MD_RUN_LINK = 16,  /* link text; url holds the target */
    MD_RUN_BREAK = 32  /* a hard line break; text is ignored */
};

typedef struct md_run {
    const char *text; /* UTF-8, not NUL terminated */
    size_t len;
    unsigned flags;
    const char *url; /* MD_RUN_LINK only, NUL terminated */
} md_run;

/* Appends the Markdown for a sequence of runs to out.
 *   table_cell   escape "|" as well
 *   break_prefix written after every hard line break (the container prefix
 *                of continuation lines, for example "> " or "  "); may be NULL
 *   close        0 leaves emphasis and links that are still open unclosed,
 *                which gives the Markdown length up to a caret position
 * Plain text is escaped so that it stays plain text. */
void md_write_inline(const md_run *runs, size_t n, int table_cell, const char *break_prefix, int close, mdbuf *out);

#endif
