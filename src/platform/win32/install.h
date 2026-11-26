/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Self-installer. Domdly.exe installs itself: it copies itself to
 * %ProgramFiles%\Domdly, adds a Start menu shortcut, registers in
 * "Installed apps" and offers itself in the "Open with" list for .md files.
 * No external installer technology is used.
 */
#ifndef DOMDLY_WIN32_INSTALL_H
#define DOMDLY_WIN32_INSTALL_H

#include <windows.h>
#include "settings.h"

void inst_install_dir(WCHAR *out, size_t cap);
void inst_installed_exe(WCHAR *out, size_t cap);
int  inst_running_installed(void);
/* Returns 1 when an installation is registered; copies its version. */
int  inst_installed_version(WCHAR *version, size_t cap);
int  inst_is_elevated(void);

/* Runs this executable elevated with args and waits. Returns 1 when the
 * elevated process ran (its exit code is stored), 0 if the user declined. */
int  inst_run_elevated(HWND owner, const WCHAR *args, DWORD *exit_code);

/* Both require administrator rights. Return 0 on success. */
int  inst_install(int quiet);
int  inst_uninstall(int quiet);

/* Handles --install / --uninstall on the command line. Returns 1 when the
 * command line was handled and the process should exit with *exit_code. */
int  inst_handle_command_line(int argc, WCHAR **argv, int *exit_code);

/* Offers to install when running a copy outside Program Files. Returns 1 if
 * the installed copy was started and this process should exit. */
int  inst_startup_offer(app_settings *s, const WCHAR *file_to_open);

/* Interactive install from the Help menu. Returns 1 if the installed copy
 * was started (the caller should close this window). */
int  inst_interactive_install(HWND owner);
void inst_interactive_uninstall(HWND owner);

#endif
