/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * mkicon: draws the Domdly application icon procedurally and writes a
 * multi-resolution .ico file. The artwork is pure geometry defined in
 * icon_art.h (a rounded square with a white "D"), so the icon is
 * original to the project and carries no third-party rights.
 *
 * Usage: mkicon <output.ico>   or   mkicon --png <size> <output.png>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icon_art.h"

static void render(int size, unsigned char *bgra)
{
    const int ss = 8; /* supersampling per axis */
    double margin = size <= 16 ? 0.0 : size <= 32 ? 0.03 : 0.06;
    double radius = icon_radius;
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            double bg = 0, fg = 0;
            for (int sy = 0; sy < ss; sy++) {
                for (int sx = 0; sx < ss; sx++) {
                    double x = (px + (sx + 0.5) / ss) / size;
                    double y = (py + (sy + 0.5) / ss) / size;
                    double d = sd_round_rect(x, y, 0.5, 0.5, 0.5 - margin, 0.5 - margin, radius);
                    if (d <= 0) {
                        bg += 1;
                        if (in_mark(x, y)) fg += 1;
                    }
                }
            }
            bg /= ss * ss;
            fg /= ss * ss;
            double t = (double)py / (size - 1 > 0 ? size - 1 : 1);
            double r = icon_top[0] + (icon_bottom[0] - icon_top[0]) * t;
            double g = icon_top[1] + (icon_bottom[1] - icon_top[1]) * t;
            double b = icon_top[2] + (icon_bottom[2] - icon_top[2]) * t;
            /* white glyph over the gradient (fg is a subset of bg) */
            double k = bg > 0 ? fg / bg : 0;
            r = r * (1 - k) + 1.0 * k;
            g = g * (1 - k) + 1.0 * k;
            b = b * (1 - k) + 1.0 * k;
            /* ICO rows are stored bottom-up */
            unsigned char *p = bgra + ((size_t)(size - 1 - py) * size + px) * 4;
            p[0] = (unsigned char)(b * 255 + 0.5);
            p[1] = (unsigned char)(g * 255 + 0.5);
            p[2] = (unsigned char)(r * 255 + 0.5);
            p[3] = (unsigned char)(bg * 255 + 0.5);
        }
    }
}

static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }
static void put32(FILE *f, unsigned long v) { put16(f, (unsigned)(v & 0xFFFF)); put16(f, (unsigned)(v >> 16)); }

/* ---- PNG output for Linux desktops (no zlib: stored deflate blocks) ---- */

static unsigned long crc_table[256];

static void crc_init(void)
{
    for (unsigned long n = 0; n < 256; n++) {
        unsigned long c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned long crc_update(unsigned long c, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
    return c;
}

static void put_be32(FILE *f, unsigned long v)
{
    fputc((int)((v >> 24) & 255), f);
    fputc((int)((v >> 16) & 255), f);
    fputc((int)((v >> 8) & 255), f);
    fputc((int)(v & 255), f);
}

static void chunk(FILE *f, const char *type, const unsigned char *data, size_t n)
{
    put_be32(f, (unsigned long)n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    unsigned long c = crc_update(0xFFFFFFFFUL, (const unsigned char *)type, 4);
    c = crc_update(c, data, n);
    put_be32(f, c ^ 0xFFFFFFFFUL);
}

static int write_png(const char *path, int s)
{
    unsigned char *bgra = (unsigned char *)calloc((size_t)s * s, 4);
    size_t raw_len = (size_t)s * ((size_t)s * 4 + 1);
    unsigned char *raw = (unsigned char *)malloc(raw_len);
    size_t blocks = raw_len / 65535 + 1;
    unsigned char *z = (unsigned char *)malloc(raw_len + blocks * 5 + 6);
    if (!bgra || !raw || !z) return 1;
    render(s, bgra);
    for (int y = 0; y < s; y++) { /* rows top-down, RGBA, filter 0 */
        unsigned char *row = raw + (size_t)y * ((size_t)s * 4 + 1);
        const unsigned char *src = bgra + (size_t)(s - 1 - y) * s * 4;
        row[0] = 0;
        for (int x = 0; x < s; x++) {
            row[1 + x * 4] = src[x * 4 + 2];
            row[2 + x * 4] = src[x * 4 + 1];
            row[3 + x * 4] = src[x * 4 + 0];
            row[4 + x * 4] = src[x * 4 + 3];
        }
    }
    size_t zn = 0;
    z[zn++] = 0x78;
    z[zn++] = 0x01;
    unsigned long a = 1, b = 0;
    for (size_t pos = 0; pos < raw_len;) {
        size_t n = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        z[zn++] = pos + n == raw_len ? 1 : 0;
        z[zn++] = (unsigned char)(n & 255);
        z[zn++] = (unsigned char)(n >> 8);
        z[zn++] = (unsigned char)(~n & 255);
        z[zn++] = (unsigned char)((~n >> 8) & 255);
        memcpy(z + zn, raw + pos, n);
        for (size_t i = 0; i < n; i++) {
            a = (a + raw[pos + i]) % 65521;
            b = (b + a) % 65521;
        }
        zn += n;
        pos += n;
    }
    unsigned long adler = (b << 16) | a;
    z[zn++] = (unsigned char)(adler >> 24);
    z[zn++] = (unsigned char)(adler >> 16);
    z[zn++] = (unsigned char)(adler >> 8);
    z[zn++] = (unsigned char)adler;

    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 1;
    }
    static const unsigned char sig[8] = {137, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    unsigned char ihdr[13] = {0};
    ihdr[0] = (unsigned char)(s >> 24);
    ihdr[1] = (unsigned char)(s >> 16);
    ihdr[2] = (unsigned char)(s >> 8);
    ihdr[3] = (unsigned char)s;
    memcpy(ihdr + 4, ihdr, 4);
    ihdr[8] = 8; /* bit depth */
    ihdr[9] = 6; /* RGBA */
    crc_init();
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, zn);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(bgra);
    free(raw);
    free(z);
    return 0;
}

int main(int argc, char **argv)
{
    static const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    const int count = (int)(sizeof sizes / sizeof sizes[0]);
    if (argc == 4 && strcmp(argv[1], "--png") == 0) {
        int s = atoi(argv[2]);
        if (s < 16 || s > 1024) return 2;
        return write_png(argv[3], s);
    }
    if (argc != 2) {
        fprintf(stderr, "usage: mkicon <output.ico> | mkicon --png <size> <output.png>\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "wb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    unsigned long offset = 6 + 16UL * count;
    unsigned long bytes[16];
    for (int i = 0; i < count; i++) {
        int s = sizes[i];
        unsigned long mask_row = (unsigned long)((s + 31) / 32) * 4;
        bytes[i] = 40 + (unsigned long)s * s * 4 + mask_row * s;
    }
    put16(f, 0);
    put16(f, 1);
    put16(f, (unsigned)count);
    for (int i = 0; i < count; i++) {
        int s = sizes[i];
        fputc(s >= 256 ? 0 : s, f);
        fputc(s >= 256 ? 0 : s, f);
        fputc(0, f);
        fputc(0, f);
        put16(f, 1);
        put16(f, 32);
        put32(f, bytes[i]);
        put32(f, offset);
        offset += bytes[i];
    }
    for (int i = 0; i < count; i++) {
        int s = sizes[i];
        unsigned char *px = (unsigned char *)calloc((size_t)s * s, 4);
        if (!px) return 1;
        render(s, px);
        put32(f, 40);
        put32(f, (unsigned long)s);
        put32(f, (unsigned long)s * 2);
        put16(f, 1);
        put16(f, 32);
        put32(f, 0);
        put32(f, (unsigned long)s * s * 4);
        put32(f, 0);
        put32(f, 0);
        put32(f, 0);
        put32(f, 0);
        fwrite(px, 4, (size_t)s * s, f);
        unsigned long mask_row = (unsigned long)((s + 31) / 32) * 4;
        for (unsigned long k = 0; k < mask_row * (unsigned long)s; k++) fputc(0, f);
        free(px);
    }
    fclose(f);
    return 0;
}
