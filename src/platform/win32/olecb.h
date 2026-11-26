/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Minimal IRichEditOleCallback. RichEdit only creates picture objects from
 * RTF (\pict) when its host provides storage through this interface.
 */
#ifndef DOMDLY_WIN32_OLECB_H
#define DOMDLY_WIN32_OLECB_H

#include <windows.h>

/* Installs the callback on a RichEdit control. Returns 1 on success. */
int olecb_attach(HWND richedit);

#endif
