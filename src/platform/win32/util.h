/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Small Win32 helpers: string conversion, file I/O and text decoding.
 */
#ifndef DOMDLY_WIN32_UTIL_H
#define DOMDLY_WIN32_UTIL_H

#include <windows.h>
#include <stddef.h>

#define DOMDLY_PATH_MAX 1024

/* Heap-allocated conversions; free() the result. Length -1 = NUL-terminated. */
WCHAR *util_utf8_to_wide(const char *s, int len);
char  *util_wide_to_utf8(const WCHAR *s, int len, int *out_len);

/* Reads a whole file (max 256 MiB). free() *data. Returns 1 on success. */
int util_read_file(const WCHAR *path, char **data, size_t *len);

/* Writes through a temporary file and renames it over the target. */
int util_write_file_atomic(const WCHAR *path, const void *data, size_t len);

typedef enum text_encoding {
    ENC_UTF8,
    ENC_UTF8_BOM,
    ENC_UTF16LE,
    ENC_UTF16BE,
    ENC_ANSI
} text_encoding;

/* Decodes file bytes to UTF-16. Detects BOMs, validates UTF-8 and falls back
 * to the ANSI code page. *crlf is set when the text uses CRLF line breaks. */
WCHAR *util_decode_text(const char *data, size_t len, text_encoding *enc, int *crlf);

const WCHAR *util_encoding_name(text_encoding enc);

/* Directory part of path (without trailing slash). */
void util_path_dir(const WCHAR *path, WCHAR *out, size_t cap);
/* File name part of path. */
const WCHAR *util_path_name(const WCHAR *path);
int util_path_has_ext(const WCHAR *path, const WCHAR *ext);
int util_file_exists(const WCHAR *path);

/* Loads a resource (RCDATA) as a pointer into the module image. */
const char *util_resource(int id, size_t *len);

int util_scale(int px, int dpi);

#endif
