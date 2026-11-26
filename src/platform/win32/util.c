/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

WCHAR *util_utf8_to_wide(const char *s, int len)
{
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    WCHAR *w = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    w[n] = 0;
    return w;
}

char *util_wide_to_utf8(const WCHAR *s, int len, int *out_len)
{
    if (!s) s = L"";
    int n = WideCharToMultiByte(CP_UTF8, 0, s, len, NULL, 0, NULL, NULL);
    char *u = (char *)malloc((size_t)n + 1);
    if (!u) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, s, len, u, n, NULL, NULL);
    u[n] = 0;
    if (len == -1 && n > 0) n--; /* exclude terminator */
    if (out_len) *out_len = n;
    return u;
}

int util_read_file(const WCHAR *path, char **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > 256LL * 1024 * 1024) {
        CloseHandle(h);
        return 0;
    }
    size_t n = (size_t)size.QuadPart;
    char *buf = (char *)malloc(n + 1);
    if (!buf) {
        CloseHandle(h);
        return 0;
    }
    size_t got = 0;
    while (got < n) {
        DWORD chunk = (DWORD)((n - got) > 0x10000000 ? 0x10000000 : (n - got)), r = 0;
        if (!ReadFile(h, buf + got, chunk, &r, NULL) || r == 0) break;
        got += r;
    }
    CloseHandle(h);
    buf[got] = 0;
    *data = buf;
    *len = got;
    return 1;
}

int util_write_file_atomic(const WCHAR *path, const void *data, size_t len)
{
    size_t pl = wcslen(path);
    WCHAR *tmp = (WCHAR *)malloc((pl + 16) * sizeof(WCHAR));
    if (!tmp) return 0;
    wcscpy(tmp, path);
    wcscat(tmp, L".domdly-tmp");
    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(tmp);
        return 0;
    }
    const char *p = (const char *)data;
    size_t done = 0;
    int ok = 1;
    while (done < len) {
        DWORD chunk = (DWORD)((len - done) > 0x10000000 ? 0x10000000 : (len - done)), w = 0;
        if (!WriteFile(h, p + done, chunk, &w, NULL) || w == 0) {
            ok = 0;
            break;
        }
        done += w;
    }
    if (ok) ok = FlushFileBuffers(h) != 0;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp);
    free(tmp);
    return ok;
}

WCHAR *util_decode_text(const char *data, size_t len, text_encoding *enc, int *crlf)
{
    const unsigned char *u = (const unsigned char *)data;
    WCHAR *w = NULL;
    *crlf = 0;
    if (len >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF))) {
        int be = u[0] == 0xFE;
        size_t n = (len - 2) / 2;
        w = (WCHAR *)malloc((n + 1) * sizeof(WCHAR));
        if (!w) return NULL;
        for (size_t i = 0; i < n; i++) {
            unsigned char a = u[2 + i * 2], b = u[3 + i * 2];
            w[i] = be ? (WCHAR)((a << 8) | b) : (WCHAR)((b << 8) | a);
        }
        w[n] = 0;
        *enc = be ? ENC_UTF16BE : ENC_UTF16LE;
    } else {
        size_t skip = 0;
        *enc = ENC_UTF8;
        if (len >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) {
            skip = 3;
            *enc = ENC_UTF8_BOM;
        }
        int n = len - skip > 0 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data + skip, (int)(len - skip), NULL, 0) : 0;
        UINT cp = CP_UTF8;
        DWORD flags = 0;
        if (n == 0 && len - skip > 0) {
            cp = CP_ACP;
            *enc = ENC_ANSI;
            n = MultiByteToWideChar(cp, flags, data + skip, (int)(len - skip), NULL, 0);
        }
        w = (WCHAR *)malloc(((size_t)n + 1) * sizeof(WCHAR));
        if (!w) return NULL;
        if (n) MultiByteToWideChar(cp, flags, data + skip, (int)(len - skip), w, n);
        w[n] = 0;
    }
    for (WCHAR *p = w; *p; p++) {
        if (p[0] == '\r' && p[1] == '\n') {
            *crlf = 1;
            break;
        }
        if (p[0] == '\n') break;
    }
    return w;
}

const WCHAR *util_encoding_name(text_encoding enc)
{
    switch (enc) {
    case ENC_UTF8_BOM: return L"UTF-8 with BOM";
    case ENC_UTF16LE: return L"UTF-16 LE";
    case ENC_UTF16BE: return L"UTF-16 BE";
    case ENC_ANSI: return L"ANSI";
    default: return L"UTF-8";
    }
}

void util_path_dir(const WCHAR *path, WCHAR *out, size_t cap)
{
    if (!cap) return;
    out[0] = 0;
    if (!path) return;
    const WCHAR *slash = NULL;
    for (const WCHAR *p = path; *p; p++)
        if (*p == '\\' || *p == '/') slash = p;
    if (!slash) return;
    size_t n = (size_t)(slash - path);
    if (n >= cap) n = cap - 1;
    wmemcpy(out, path, n);
    out[n] = 0;
    if (n == 2 && out[1] == ':') { /* "C:" -> "C:\" */
        if (cap > 3) {
            out[2] = '\\';
            out[3] = 0;
        }
    }
}

const WCHAR *util_path_name(const WCHAR *path)
{
    const WCHAR *name = path;
    for (const WCHAR *p = path; *p; p++)
        if (*p == '\\' || *p == '/') name = p + 1;
    return name;
}

int util_path_has_ext(const WCHAR *path, const WCHAR *ext)
{
    size_t pl = wcslen(path), el = wcslen(ext);
    if (pl < el) return 0;
    return CompareStringOrdinal(path + pl - el, (int)el, ext, (int)el, TRUE) == CSTR_EQUAL;
}

int util_file_exists(const WCHAR *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

const char *util_resource(int id, size_t *len)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!r) return NULL;
    HGLOBAL g = LoadResource(NULL, r);
    if (!g) return NULL;
    *len = SizeofResource(NULL, r);
    return (const char *)LockResource(g);
}

int util_scale(int px, int dpi)
{
    return MulDiv(px, dpi, 96);
}
