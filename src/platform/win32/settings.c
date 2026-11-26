/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int get_dword(HKEY k, const WCHAR *name, int def)
{
    DWORD v = 0, size = sizeof v, type = 0;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &size) == ERROR_SUCCESS && type == REG_DWORD)
        return (int)v;
    return def;
}

static void set_dword(HKEY k, const WCHAR *name, int v)
{
    DWORD d = (DWORD)v;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&d, sizeof d);
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void settings_load(app_settings *s)
{
    memset(s, 0, sizeof *s);
    s->view = VIEW_EDITOR;
    s->theme = THEME_SYSTEM;
    s->word_wrap = 1;
    s->sync_scroll = 1;
    s->status_bar = 1;
    s->zoom = 100;
    s->split_permille = 500;
    s->tree_width = 240;

    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        /* first start under the new name: take over the settings that the
         * test builds stored under the old one ("doMD") */
        HKEY old, created;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\doMD", 0, KEY_READ, &old) != ERROR_SUCCESS) return;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &created, NULL) ==
            ERROR_SUCCESS) {
            RegCopyTreeW(old, NULL, created);
            RegCloseKey(created);
        }
        RegCloseKey(old);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    }
    s->view = clamp(get_dword(k, L"View", s->view), VIEW_EDITOR, VIEW_PREVIEW); /* 0 was the old split view */
    s->theme = clamp(get_dword(k, L"Theme", s->theme), 0, 3);
    s->word_wrap = get_dword(k, L"WordWrap", s->word_wrap) != 0;
    s->sync_scroll = get_dword(k, L"SyncScroll", s->sync_scroll) != 0;
    s->status_bar = get_dword(k, L"StatusBar", s->status_bar) != 0;
    s->zoom = clamp(get_dword(k, L"Zoom", s->zoom), 50, 300);
    s->split_permille = clamp(get_dword(k, L"Split", s->split_permille), 150, 850);
    s->no_install_prompt = get_dword(k, L"NoInstallPrompt", 0) != 0;
    s->tree_open = get_dword(k, L"TreeOpen", 0) != 0;
    s->tree_width = clamp(get_dword(k, L"TreeWidth", s->tree_width), 120, 2000);
    {
        DWORD size = sizeof s->tree_root - sizeof(WCHAR), type = 0;
        if (RegQueryValueExW(k, L"TreeRoot", NULL, &type, (BYTE *)s->tree_root, &size) != ERROR_SUCCESS ||
            type != REG_SZ)
            size = 0;
        s->tree_root[size / sizeof(WCHAR)] = 0;
    }

    DWORD size = sizeof s->placement, type = 0;
    if (RegQueryValueExW(k, L"Placement", NULL, &type, (BYTE *)&s->placement, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size == sizeof s->placement && s->placement.length == sizeof s->placement)
        s->has_placement = 1;

    for (int i = 0; i < RECENT_MAX; i++) {
        WCHAR name[16];
        swprintf(name, 16, L"Recent%d", i);
        DWORD bytes = sizeof s->recent[0] - sizeof(WCHAR);
        WCHAR *dst = s->recent[s->recent_count];
        if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)dst, &bytes) == ERROR_SUCCESS && type == REG_SZ) {
            dst[bytes / sizeof(WCHAR)] = 0;
            if (dst[0]) s->recent_count++;
        }
    }
    for (int i = 0; i < SESSION_MAX; i++) {
        WCHAR name[16];
        swprintf(name, 16, L"Tab%d", i);
        DWORD bytes = sizeof s->session[0] - sizeof(WCHAR);
        WCHAR *dst = s->session[s->session_count];
        if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)dst, &bytes) != ERROR_SUCCESS || type != REG_SZ) break;
        dst[bytes / sizeof(WCHAR)] = 0;
        if (dst[0]) s->session_count++;
    }
    s->session_active = clamp(get_dword(k, L"ActiveTab", 0), 0, SESSION_MAX - 1);
    RegCloseKey(k);
}

void settings_save(const app_settings *s)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    set_dword(k, L"View", s->view);
    set_dword(k, L"Theme", s->theme);
    set_dword(k, L"WordWrap", s->word_wrap);
    set_dword(k, L"SyncScroll", s->sync_scroll);
    set_dword(k, L"StatusBar", s->status_bar);
    set_dword(k, L"Zoom", s->zoom);
    set_dword(k, L"Split", s->split_permille);
    set_dword(k, L"NoInstallPrompt", s->no_install_prompt);
    set_dword(k, L"TreeOpen", s->tree_open);
    set_dword(k, L"TreeWidth", s->tree_width);
    if (s->tree_root[0])
        RegSetValueExW(k, L"TreeRoot", 0, REG_SZ, (const BYTE *)s->tree_root,
                       (DWORD)((wcslen(s->tree_root) + 1) * sizeof(WCHAR)));
    else
        RegDeleteValueW(k, L"TreeRoot");
    if (s->has_placement)
        RegSetValueExW(k, L"Placement", 0, REG_BINARY, (const BYTE *)&s->placement, sizeof s->placement);
    for (int i = 0; i < RECENT_MAX; i++) {
        WCHAR name[16];
        swprintf(name, 16, L"Recent%d", i);
        if (i < s->recent_count)
            RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)s->recent[i],
                           (DWORD)((wcslen(s->recent[i]) + 1) * sizeof(WCHAR)));
        else
            RegDeleteValueW(k, name);
    }
    for (int i = 0; i < SESSION_MAX; i++) {
        WCHAR name[16];
        swprintf(name, 16, L"Tab%d", i);
        if (i < s->session_count)
            RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)s->session[i],
                           (DWORD)((wcslen(s->session[i]) + 1) * sizeof(WCHAR)));
        else
            RegDeleteValueW(k, name);
    }
    set_dword(k, L"ActiveTab", s->session_active);
    RegCloseKey(k);
}

void settings_remove_recent(app_settings *s, const WCHAR *path)
{
    for (int i = 0; i < s->recent_count; i++) {
        if (CompareStringOrdinal(s->recent[i], -1, path, -1, TRUE) == CSTR_EQUAL) {
            for (int j = i; j + 1 < s->recent_count; j++) wcscpy(s->recent[j], s->recent[j + 1]);
            s->recent_count--;
            i--;
        }
    }
}

void settings_add_recent(app_settings *s, const WCHAR *path)
{
    if (!path || !*path || wcslen(path) >= DOMDLY_PATH_MAX) return;
    settings_remove_recent(s, path);
    if (s->recent_count == RECENT_MAX) s->recent_count--;
    for (int j = s->recent_count; j > 0; j--) wcscpy(s->recent[j], s->recent[j - 1]);
    wcscpy(s->recent[0], path);
    s->recent_count++;
}
