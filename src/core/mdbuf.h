/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mdbuf: a small growable byte buffer used by the parser and renderers.
 * Portable C99, no platform dependencies.
 */
#ifndef DOMDLY_MDBUF_H
#define DOMDLY_MDBUF_H

#include <stddef.h>

typedef struct mdbuf {
    char  *data;
    size_t len;
    size_t cap;
} mdbuf;

void  mdbuf_init(mdbuf *b);
void  mdbuf_free(mdbuf *b);
void  mdbuf_reserve(mdbuf *b, size_t extra);
void  mdbuf_put(mdbuf *b, const char *s, size_t n);
void  mdbuf_puts(mdbuf *b, const char *s);
void  mdbuf_putc(mdbuf *b, char c);
void  mdbuf_printf(mdbuf *b, const char *fmt, ...);
/* Returns a NUL-terminated heap string and resets the buffer. */
char *mdbuf_detach(mdbuf *b);

/* Allocation helpers that abort on out-of-memory. */
void *md_xmalloc(size_t n);
void *md_xcalloc(size_t count, size_t size);
void *md_xrealloc(void *p, size_t n);
char *md_xstrndup(const char *s, size_t n);

#endif
