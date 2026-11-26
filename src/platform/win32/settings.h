/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Per-user settings stored under HKEY_CURRENT_USER\Software\Domdly.
 */
#ifndef DOMDLY_WIN32_SETTINGS_H
#define DOMDLY_WIN32_SETTINGS_H

#include <windows.h>
#include "util.h"

#define SETTINGS_KEY  L"Software\\Domdly"
#define RECENT_MAX    10
#define SESSION_MAX   64

enum { VIEW_SPLIT = 0, VIEW_EDITOR = 1, VIEW_PREVIEW = 2 };
enum { THEME_SYSTEM = 0, THEME_LIGHT = 1, THEME_DARK = 2, THEME_NIGHT = 3 };

typedef struct app_settings {
    int view;
    int theme;
    int word_wrap;
    int sync_scroll;
    int status_bar;
    int zoom;            /* percent */
    int split_permille;  /* editor share of the window width */
    int no_install_prompt;
    int has_placement;
    WINDOWPLACEMENT placement;
    int recent_count;
    WCHAR recent[RECENT_MAX][DOMDLY_PATH_MAX];
    int tree_open;       /* the files sidebar is shown */
    int fmt_bar;         /* the formatting toolbar is shown */
    int tree_width;      /* its width at 96 DPI */
    WCHAR tree_root[DOMDLY_PATH_MAX]; /* pinned folder; empty = follow the open file */
    int session_count;   /* files open in tabs when Domdly last closed */
    int session_active;
    WCHAR session[SESSION_MAX][DOMDLY_PATH_MAX];
} app_settings;

void settings_load(app_settings *s);
void settings_save(const app_settings *s);
void settings_add_recent(app_settings *s, const WCHAR *path);
void settings_remove_recent(app_settings *s, const WCHAR *path);

#endif
