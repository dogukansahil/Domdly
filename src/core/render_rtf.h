/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * RTF renderer (portable C99). The Windows front end displays the preview
 * with the RichEdit control that ships with the operating system; other
 * front ends may use it for clipboard export. Images are embedded as PNG or
 * JPEG pictures when the host supplies a loader callback.
 */
#ifndef DOMDLY_RENDER_RTF_H
#define DOMDLY_RENDER_RTF_H

#include "markdown.h"
#include "mdbuf.h"

/* Private-use character emitted for images in placeholder mode. Literal
 * occurrences in the document are replaced with U+FFFD. */
#define MD_RTF_IMAGE_PLACEHOLDER 0xE000
/* Private-use character written as hidden text at block starts. */
#define MD_RTF_BLOCK_MARKER 0xE001
/* Hidden characters that open and close the raw source region (see
 * md_rtf_options.raw_line). Each sits alone in a tiny paragraph of its own. */
#define MD_RTF_RAW_START 0xE002
#define MD_RTF_RAW_END 0xE003

/* Colors are 0xRRGGBB. */
typedef struct md_rtf_colors {
    unsigned long text;
    unsigned long heading;
    unsigned long link;
    unsigned long code_fg;
    unsigned long code_bg;
    unsigned long quote;
    unsigned long rule;
    unsigned long muted;
    unsigned long table_head_bg;
    unsigned long page_bg;
} md_rtf_colors;

/* Returns 1 and sets *data and *len (memory owned by the host) when the image
 * at url could be loaded; 0 otherwise. */
typedef int (*md_image_loader)(void *user, const char *url, const unsigned char **data, size_t *len);

typedef struct md_rtf_options {
    md_rtf_colors colors;
    const char *font;        /* proportional font family name (ASCII) */
    const char *mono_font;   /* monospaced font family name (ASCII) */
    const char *symbol_font; /* font used for check boxes (ASCII) */
    int font_half_points;    /* base size, e.g. 22 = 11 pt */
    int width_twips;         /* usable line width; 0 = 9360 (6.5 in) */
    /* Optional: embed PNG/JPEG data as RTF \pict groups (for RTF consumers
     * that understand them, e.g. word processors). */
    md_image_loader load_image;
    /* Optional, preferred over load_image: called once per image with its
     * alt text and the maximum display width in twips. Returning 1 makes the
     * renderer emit one MD_RTF_IMAGE_PLACEHOLDER character in place of the
     * image; the host replaces those characters, in document order, with
     * real pictures after loading the RTF. */
    int (*image_placeholder)(void *user, const char *url, const char *alt, int max_width_twips);
    /* Optional: called in document order for every block that starts a new
     * visual position, with its 0-based source line. The renderer then writes
     * one hidden MD_RTF_BLOCK_MARKER character, so the host can map positions
     * in the rendered text back to source lines (and the other way round). */
    void (*block_marker)(void *user, int line);
    /* Optional, called just before block_marker for blocks with a node
     * (everything but the raw region): lets the host record what kind of
     * block each marker starts, and which source lines it spans. */
    void (*block_node)(void *user, const md_node *node);
    /* Optional: show one stretch of the document as its Markdown source, so
     * the host can let the user edit it in place. Top-level blocks starting
     * on source lines raw_line .. raw_end - 1 are replaced by raw_text (lines
     * separated by '\n', no trailing newline), written in the monospaced
     * font between a MD_RTF_RAW_START and a MD_RTF_RAW_END paragraph. The
     * start paragraph also carries a block marker for raw_line. A negative
     * raw_line (the default) turns this off. */
    int raw_line, raw_end;
    const char *raw_text;
    size_t raw_len;
    void *user;
} md_rtf_options;

void md_rtf_default_options(md_rtf_options *opt, int dark);
void md_render_rtf(const md_node *doc, const md_rtf_options *opt, mdbuf *out);

/* Reads the pixel size of PNG, JPEG, GIF or BMP data. Returns 1 on success. */
int md_image_size(const unsigned char *data, size_t len, long *width, long *height);

#endif
