/*
 * Domdly - a lightweight Markdown reader and editor
 * Copyright (C) 2026 Dogukan Sahil and Domdly contributors
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "install.h"
#include "resource.h"
#include "util.h"
#include "version.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Domdly"
#define APPPATH_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Domdly.exe"
#define PROGID        L"Domdly.Markdown"
#define PROGID_KEY    L"Software\\Classes\\Domdly.Markdown"
#define APP_KEY       L"Software\\Classes\\Applications\\Domdly.exe"

static const WCHAR *const md_exts[] = {L".md", L".markdown", L".mdown", L".mkd", NULL};

/* ------------------------------------------------------------------------ */
/* Paths                                                                    */
/* ------------------------------------------------------------------------ */

void inst_install_dir(WCHAR *out, size_t cap)
{
    PWSTR pf = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_ProgramFiles, 0, NULL, &pf)) && pf) {
        swprintf(out, cap, L"%s\\Domdly", pf);
        CoTaskMemFree(pf);
    } else {
        swprintf(out, cap, L"C:\\Program Files\\Domdly");
    }
}

void inst_installed_exe(WCHAR *out, size_t cap)
{
    WCHAR dir[MAX_PATH];
    inst_install_dir(dir, MAX_PATH);
    swprintf(out, cap, L"%s\\Domdly.exe", dir);
}

static void self_path(WCHAR *out, DWORD cap)
{
    DWORD n = GetModuleFileNameW(NULL, out, cap);
    if (n == 0 || n >= cap) out[0] = 0;
}

static int same_path(const WCHAR *a, const WCHAR *b)
{
    WCHAR fa[DOMDLY_PATH_MAX], fb[DOMDLY_PATH_MAX];
    if (!GetFullPathNameW(a, DOMDLY_PATH_MAX, fa, NULL)) return 0;
    if (!GetFullPathNameW(b, DOMDLY_PATH_MAX, fb, NULL)) return 0;
    return CompareStringOrdinal(fa, -1, fb, -1, TRUE) == CSTR_EQUAL;
}

int inst_running_installed(void)
{
    WCHAR self[DOMDLY_PATH_MAX], target[DOMDLY_PATH_MAX];
    self_path(self, DOMDLY_PATH_MAX);
    inst_installed_exe(target, DOMDLY_PATH_MAX);
    return self[0] && same_path(self, target);
}

int inst_installed_version(WCHAR *version, size_t cap)
{
    HKEY k;
    if (cap) version[0] = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, bytes = (DWORD)((cap - 1) * sizeof(WCHAR));
    if (RegQueryValueExW(k, L"DisplayVersion", NULL, &type, (BYTE *)version, &bytes) == ERROR_SUCCESS && type == REG_SZ)
        version[bytes / sizeof(WCHAR)] = 0;
    else if (cap)
        version[0] = 0;
    RegCloseKey(k);
    WCHAR exe[DOMDLY_PATH_MAX];
    inst_installed_exe(exe, DOMDLY_PATH_MAX);
    return util_file_exists(exe);
}

int inst_is_elevated(void)
{
    HANDLE token;
    TOKEN_ELEVATION e;
    DWORD size = 0;
    int elevated = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &e, sizeof e, &size)) elevated = e.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

int inst_run_elevated(HWND owner, const WCHAR *args, DWORD *exit_code)
{
    WCHAR self[DOMDLY_PATH_MAX];
    self_path(self, DOMDLY_PATH_MAX);
    SHELLEXECUTEINFOW sei;
    memset(&sei, 0, sizeof sei);
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return 0;
    /* Keep the UI responsive while the elevated copy works. */
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (exit_code) *exit_code = code;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* Registry helpers                                                         */
/* ------------------------------------------------------------------------ */

static int reg_sz(HKEY root, const WCHAR *key, const WCHAR *name, const WCHAR *value)
{
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return 0;
    LONG r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static int reg_dword(HKEY root, const WCHAR *key, const WCHAR *name, DWORD value)
{
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return 0;
    LONG r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&value, sizeof value);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static int reg_none(HKEY root, const WCHAR *key, const WCHAR *name)
{
    HKEY k;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return 0;
    LONG r = RegSetValueExW(k, name, 0, REG_NONE, NULL, 0);
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static int reg_get_default(HKEY root, const WCHAR *key, WCHAR *out, DWORD cap)
{
    HKEY k;
    out[0] = 0;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    DWORD type = 0, bytes = (cap - 1) * sizeof(WCHAR);
    LONG r = RegQueryValueExW(k, NULL, NULL, &type, (BYTE *)out, &bytes);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS || type != REG_SZ) {
        out[0] = 0;
        return 0;
    }
    out[bytes / sizeof(WCHAR)] = 0;
    return 1;
}

static void reg_delete_value(HKEY root, const WCHAR *key, const WCHAR *name)
{
    HKEY k;
    if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, name);
    RegCloseKey(k);
}

/* ------------------------------------------------------------------------ */
/* Install / uninstall                                                      */
/* ------------------------------------------------------------------------ */

static int shortcut_path(WCHAR *out, size_t cap)
{
    PWSTR dir = NULL;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_CommonPrograms, 0, NULL, &dir)) || !dir) return 0;
    swprintf(out, cap, L"%s\\Domdly.lnk", dir);
    CoTaskMemFree(dir);
    return 1;
}

static int create_shortcut(const WCHAR *lnk, const WCHAR *target, const WCHAR *workdir)
{
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    int ok = 0;
    HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) {
        sl->lpVtbl->SetPath(sl, target);
        sl->lpVtbl->SetWorkingDirectory(sl, workdir);
        sl->lpVtbl->SetDescription(sl, L"Read and edit Markdown documents");
        sl->lpVtbl->SetIconLocation(sl, target, 0);
        if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
            ok = SUCCEEDED(pf->lpVtbl->Save(pf, lnk, TRUE));
            pf->lpVtbl->Release(pf);
        }
        sl->lpVtbl->Release(sl);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

static void write_license(const WCHAR *dir)
{
    size_t len = 0;
    const char *text = util_resource(IDR_LICENSE, &len);
    if (!text) return;
    WCHAR path[DOMDLY_PATH_MAX];
    swprintf(path, DOMDLY_PATH_MAX, L"%s\\LICENSE.txt", dir);
    util_write_file_atomic(path, text, len);
}

static void fail(int quiet, const WCHAR *what)
{
    if (quiet) return;
    WCHAR msg[512];
    DWORD err = GetLastError();
    swprintf(msg, 512, L"%s\n\nWindows error code: %lu", what, err);
    MessageBoxW(NULL, msg, L"Domdly Setup", MB_ICONERROR | MB_OK);
}

int inst_install(int quiet)
{
    WCHAR dir[MAX_PATH], self[DOMDLY_PATH_MAX], target[DOMDLY_PATH_MAX], old[DOMDLY_PATH_MAX];
    inst_install_dir(dir, MAX_PATH);
    inst_installed_exe(target, DOMDLY_PATH_MAX);
    self_path(self, DOMDLY_PATH_MAX);
    if (!self[0]) return 1;

    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        fail(quiet, L"Could not create the installation folder.");
        return 2;
    }
    if (!same_path(self, target)) {
        swprintf(old, DOMDLY_PATH_MAX, L"%s\\Domdly.old.exe", dir);
        DeleteFileW(old);
        if (util_file_exists(target)) {
            /* A running executable can be renamed but not overwritten. */
            if (!MoveFileExW(target, old, MOVEFILE_REPLACE_EXISTING)) {
                fail(quiet, L"Could not replace the installed Domdly.exe. Close all Domdly windows and try again.");
                return 3;
            }
        }
        if (!CopyFileW(self, target, FALSE)) {
            fail(quiet, L"Could not copy Domdly.exe to Program Files.");
            if (util_file_exists(old)) MoveFileExW(old, target, MOVEFILE_REPLACE_EXISTING);
            return 4;
        }
        if (util_file_exists(old) && !DeleteFileW(old)) MoveFileExW(old, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    }
    write_license(dir);

    WCHAR lnk[DOMDLY_PATH_MAX];
    if (shortcut_path(lnk, DOMDLY_PATH_MAX)) create_shortcut(lnk, target, dir);

    /* "Installed apps" entry */
    WCHAR quoted[DOMDLY_PATH_MAX + 4], value[DOMDLY_PATH_MAX + 64];
    swprintf(quoted, DOMDLY_PATH_MAX + 4, L"\"%s\"", target);
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"DisplayName", L"Domdly");
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"DisplayVersion", DOMDLY_VERSION_WSTR);
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"Publisher", L"Domdly contributors");
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"DisplayIcon", target);
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"InstallLocation", dir);
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"URLInfoAbout", DOMDLY_URL_WSTR);
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"HelpLink", DOMDLY_URL_WSTR);
    swprintf(value, DOMDLY_PATH_MAX + 64, L"%s --uninstall", quoted);
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"UninstallString", value);
    swprintf(value, DOMDLY_PATH_MAX + 64, L"%s --uninstall --quiet", quoted);
    reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"QuietUninstallString", value);
    reg_dword(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"NoModify", 1);
    reg_dword(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"NoRepair", 1);
    {
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(target, GetFileExInfoStandard, &fa))
            reg_dword(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"EstimatedSize", (fa.nFileSizeLow + 1023) / 1024 + 40);
        SYSTEMTIME st;
        GetLocalTime(&st);
        WCHAR date[16];
        swprintf(date, 16, L"%04u%02u%02u", st.wYear, st.wMonth, st.wDay);
        reg_sz(HKEY_LOCAL_MACHINE, UNINSTALL_KEY, L"InstallDate", date);
    }

    /* Run dialog / "start Domdly" */
    reg_sz(HKEY_LOCAL_MACHINE, APPPATH_KEY, NULL, target);
    reg_sz(HKEY_LOCAL_MACHINE, APPPATH_KEY, L"Path", dir);

    /* File type registration: offered in "Open with", never forced. */
    swprintf(value, DOMDLY_PATH_MAX + 64, L"%s \"%%1\"", quoted);
    reg_sz(HKEY_LOCAL_MACHINE, PROGID_KEY, NULL, L"Markdown Document");
    swprintf(old, DOMDLY_PATH_MAX, L"%s,0", quoted);
    reg_sz(HKEY_LOCAL_MACHINE, PROGID_KEY L"\\DefaultIcon", NULL, old);
    reg_sz(HKEY_LOCAL_MACHINE, PROGID_KEY L"\\shell\\open\\command", NULL, value);
    reg_sz(HKEY_LOCAL_MACHINE, APP_KEY, L"FriendlyAppName", L"Domdly");
    reg_sz(HKEY_LOCAL_MACHINE, APP_KEY L"\\shell\\open\\command", NULL, value);
    for (int i = 0; md_exts[i]; i++) {
        WCHAR key[128], current[128];
        reg_none(HKEY_LOCAL_MACHINE, APP_KEY L"\\SupportedTypes", md_exts[i]);
        swprintf(key, 128, L"Software\\Classes\\%s\\OpenWithProgids", md_exts[i]);
        reg_none(HKEY_LOCAL_MACHINE, key, PROGID);
        /* Only claim the extension when nothing else handles it. */
        swprintf(key, 128, L"%s", md_exts[i]);
        HKEY hk;
        int has_handler = 0;
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            DWORD type = 0, bytes = sizeof current - sizeof(WCHAR);
            if (RegQueryValueExW(hk, NULL, NULL, &type, (BYTE *)current, &bytes) == ERROR_SUCCESS && type == REG_SZ &&
                bytes > sizeof(WCHAR))
                has_handler = 1;
            RegCloseKey(hk);
        }
        swprintf(key, 128, L"Software\\Classes\\%s", md_exts[i]);
        if (!has_handler) reg_sz(HKEY_LOCAL_MACHINE, key, NULL, PROGID);
        reg_sz(HKEY_LOCAL_MACHINE, key, L"Content Type", L"text/markdown");
        reg_sz(HKEY_LOCAL_MACHINE, key, L"PerceivedType", L"text");
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
    return 0;
}

int inst_uninstall(int quiet)
{
    WCHAR dir[MAX_PATH], target[DOMDLY_PATH_MAX], path[DOMDLY_PATH_MAX];
    inst_install_dir(dir, MAX_PATH);
    inst_installed_exe(target, DOMDLY_PATH_MAX);

    if (!quiet && MessageBoxW(NULL, L"Remove Domdly from this computer?\n\nYour Markdown files are not touched.",
                              L"Uninstall Domdly", MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES)
        return 10;

    if (shortcut_path(path, DOMDLY_PATH_MAX)) DeleteFileW(path);

    RegDeleteTreeW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, UNINSTALL_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, APPPATH_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, APPPATH_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, PROGID_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, PROGID_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, APP_KEY);
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, APP_KEY);
    for (int i = 0; md_exts[i]; i++) {
        WCHAR key[128], current[128];
        swprintf(key, 128, L"Software\\Classes\\%s\\OpenWithProgids", md_exts[i]);
        reg_delete_value(HKEY_LOCAL_MACHINE, key, PROGID);
        swprintf(key, 128, L"Software\\Classes\\%s", md_exts[i]);
        if (reg_get_default(HKEY_LOCAL_MACHINE, key, current, 128) && wcscmp(current, PROGID) == 0)
            reg_delete_value(HKEY_LOCAL_MACHINE, key, NULL);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, SETTINGS_KEY);
    RegDeleteKeyW(HKEY_CURRENT_USER, SETTINGS_KEY);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);

    WCHAR self[DOMDLY_PATH_MAX];
    self_path(self, DOMDLY_PATH_MAX);
    swprintf(path, DOMDLY_PATH_MAX, L"%s\\LICENSE.txt", dir);
    DeleteFileW(path);
    swprintf(path, DOMDLY_PATH_MAX, L"%s\\Domdly.old.exe", dir);
    DeleteFileW(path);
    if (same_path(self, target)) {
        /* We cannot delete our own executable while it runs: let a hidden
         * command interpreter remove the folder once this process exits. */
        WCHAR sys[MAX_PATH], cmd[DOMDLY_PATH_MAX * 2];
        GetSystemDirectoryW(sys, MAX_PATH);
        swprintf(cmd, DOMDLY_PATH_MAX * 2,
                 L"\"%s\\cmd.exe\" /d /c \"ping -n 4 127.0.0.1 >nul & del /f /q \"%s\" & rmdir \"%s\"\"", sys, target,
                 dir);
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sys, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            MoveFileExW(target, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            MoveFileExW(dir, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
    } else {
        DeleteFileW(target);
        RemoveDirectoryW(dir);
    }
    if (!quiet) MessageBoxW(NULL, L"Domdly has been removed from this computer.", L"Uninstall Domdly", MB_ICONINFORMATION | MB_OK);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Command line and prompts                                                 */
/* ------------------------------------------------------------------------ */

static int has_arg(int argc, WCHAR **argv, const WCHAR *flag)
{
    for (int i = 1; i < argc; i++)
        if (CompareStringOrdinal(argv[i], -1, flag, -1, TRUE) == CSTR_EQUAL) return 1;
    return 0;
}

int inst_handle_command_line(int argc, WCHAR **argv, int *exit_code)
{
    int install = has_arg(argc, argv, L"--install");
    int uninstall = has_arg(argc, argv, L"--uninstall");
    if (!install && !uninstall) return 0;
    int quiet = has_arg(argc, argv, L"--quiet");
    if (!inst_is_elevated()) {
        WCHAR args[64];
        swprintf(args, 64, L"%s%s", install ? L"--install" : L"--uninstall", quiet ? L" --quiet" : L"");
        DWORD code = 1;
        if (!inst_run_elevated(NULL, args, &code)) code = 1223; /* ERROR_CANCELLED */
        *exit_code = (int)code;
        return 1;
    }
    if (install) {
        int r = inst_install(quiet);
        if (r == 0 && !quiet)
            MessageBoxW(NULL, L"Domdly is installed. You can start it from the Start menu.", L"Domdly Setup",
                        MB_ICONINFORMATION | MB_OK);
        *exit_code = r;
    } else {
        *exit_code = inst_uninstall(quiet);
    }
    return 1;
}

static HRESULT CALLBACK offer_cb(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR data)
{
    (void)wp;
    (void)lp;
    (void)data;
    if (msg == TDN_CREATED) SendMessageW(hwnd, TDM_SET_BUTTON_ELEVATION_REQUIRED_STATE, 100, TRUE);
    return S_OK;
}

static int launch_installed(const WCHAR *file_to_open)
{
    WCHAR exe[DOMDLY_PATH_MAX], dir[MAX_PATH], cmd[DOMDLY_PATH_MAX * 2 + 8];
    inst_installed_exe(exe, DOMDLY_PATH_MAX);
    inst_install_dir(dir, MAX_PATH);
    if (file_to_open && *file_to_open) swprintf(cmd, DOMDLY_PATH_MAX * 2 + 8, L"\"%s\" \"%s\"", exe, file_to_open);
    else swprintf(cmd, DOMDLY_PATH_MAX * 2 + 8, L"\"%s\"", exe);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* Returns 1 if the installed copy is now running. */
static int do_elevated_install(HWND owner, const WCHAR *file_to_open)
{
    DWORD code = 1;
    if (!inst_run_elevated(owner, L"--install --quiet", &code)) return 0;
    if (code != 0) {
        MessageBoxW(owner, L"Domdly could not be installed. You can keep using this copy.", L"Domdly Setup",
                    MB_ICONWARNING | MB_OK);
        return 0;
    }
    return launch_installed(file_to_open);
}

int inst_startup_offer(app_settings *s, const WCHAR *file_to_open)
{
    if (s->no_install_prompt || inst_running_installed()) return 0;
    WCHAR ver[64];
    int installed = inst_installed_version(ver, 64);
    if (installed && wcscmp(ver, DOMDLY_VERSION_WSTR) == 0) return 0;

    TASKDIALOG_BUTTON buttons[2];
    buttons[0].nButtonID = 100;
    buttons[0].pszButtonText = installed ? L"Update the installed copy\nReplaces the version in Program Files with this one."
                                         : L"Install Domdly\nCopies Domdly to Program Files and adds it to the Start menu "
                                           L"and the \"Open with\" list for Markdown files.";
    buttons[1].nButtonID = 101;
    buttons[1].pszButtonText = L"Run without installing\nUse this copy as a portable app.";

    TASKDIALOGCONFIG tc;
    memset(&tc, 0, sizeof tc);
    tc.cbSize = sizeof tc;
    tc.hInstance = GetModuleHandleW(NULL);
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"Domdly";
    tc.pszMainIcon = MAKEINTRESOURCEW(IDI_APP);
    tc.pszMainInstruction = installed ? L"Update Domdly on this PC?" : L"Install Domdly on this PC?";
    tc.pszContent = L"Domdly is a single program file. Installing it takes a second and needs administrator "
                    L"approval. You can uninstall it any time from Settings \x2192 Apps.";
    tc.cButtons = 2;
    tc.pButtons = buttons;
    tc.nDefaultButton = 100;
    tc.pszVerificationText = L"Don't ask me again";
    tc.pfCallback = offer_cb;

    int button = 0;
    BOOL never = FALSE;
    if (FAILED(TaskDialogIndirect(&tc, &button, NULL, &never))) return 0;
    if (never) {
        s->no_install_prompt = 1;
        settings_save(s);
    }
    if (button != 100) return 0;
    return do_elevated_install(NULL, file_to_open);
}

int inst_interactive_install(HWND owner)
{
    if (inst_running_installed()) {
        MessageBoxW(owner, L"This copy of Domdly is already the installed one.", L"Domdly", MB_ICONINFORMATION | MB_OK);
        return 0;
    }
    if (do_elevated_install(owner, NULL)) {
        MessageBoxW(owner, L"Domdly is installed and has been started from Program Files.", L"Domdly Setup",
                    MB_ICONINFORMATION | MB_OK);
        return 1;
    }
    return 0;
}

void inst_interactive_uninstall(HWND owner)
{
    WCHAR ver[64];
    if (!inst_installed_version(ver, 64)) {
        MessageBoxW(owner, L"Domdly is not installed on this computer. A portable copy can simply be deleted.",
                    L"Domdly", MB_ICONINFORMATION | MB_OK);
        return;
    }
    DWORD code = 1;
    inst_run_elevated(owner, L"--uninstall", &code);
}
