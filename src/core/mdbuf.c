/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "mdbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *md_xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) abort();
    return p;
}

void *md_xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (!p) abort();
    return p;
}

void *md_xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) abort();
    return q;
}

char *md_xstrndup(const char *s, size_t n)
{
    char *p = (char *)md_xmalloc(n + 1);
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

void mdbuf_init(mdbuf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

void mdbuf_free(mdbuf *b)
{
    free(b->data);
    mdbuf_init(b);
}

void mdbuf_reserve(mdbuf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < need) cap += cap / 2 + 64;
    b->data = (char *)md_xrealloc(b->data, cap);
    b->cap = cap;
}

void mdbuf_put(mdbuf *b, const char *s, size_t n)
{
    if (!n) return;
    mdbuf_reserve(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void mdbuf_puts(mdbuf *b, const char *s)
{
    mdbuf_put(b, s, strlen(s));
}

void mdbuf_putc(mdbuf *b, char c)
{
    mdbuf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
}

void mdbuf_printf(mdbuf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) {
        mdbuf_put(b, tmp, (size_t)n);
        return;
    }
    mdbuf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

char *mdbuf_detach(mdbuf *b)
{
    char *p = b->data;
    if (!p) p = md_xstrndup("", 0);
    mdbuf_init(b);
    return p;
}
