/* suite_help.c - Teletext Navigator Help through The Teletext WinHelp.
 *
 * Copyright (c) 2026 Dimitri Papadopoulos and The Teletext.
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * What user32's WinHelp() did for a Windows 3.1 application, done here
 * because Windows 11's WinHelp() no longer reaches a viewer:
 *   1. find the running help program by its window class, MS_WINHELP;
 *   2. if there is none, start it hidden (TTWINHLP.EXE -x) and wait for it;
 *   3. send it the request as WM_COPYDATA: a WINHELP block (the layout and
 *      the magic number of Wine's user32 and winhlp32) naming the help file.
 * One viewer serves every request (the single-instance path), so F1 in
 * another tab moves the same Help window to that tab's topic.
 */
#include "suite_help.h"

#include <stdio.h>
#include <string.h>

#define WINHELP_MAGIC 0xA1DE505   /* COPYDATASTRUCT.dwData, Wine user32 */

typedef struct {          /* Wine's WINHELP block, 16 bytes, then the strings */
    WORD size;
    WORD command;
    LONG data;
    LONG reserved;
    WORD ofsFilename;
    WORD ofsData;
} nav_winhelp_t;

static void help_dir(char *dir, size_t cap)
{
    DWORD n = GetEnvironmentVariableA("NAV_HELP_DIR", dir, (DWORD)cap);
    if (n > 0 && n < cap) return;
    n = GetModuleFileNameA(NULL, dir, (DWORD)cap);
    if (n == 0 || n >= cap) { dir[0] = 0; return; }
    {
        char *slash = strrchr(dir, '\\');
        if (slash) *slash = 0;
    }
}

static void help_error(HWND owner, const char *what, const char *dir)
{
    char msg[MAX_PATH + 256];
    snprintf(msg, sizeof msg,
             "Help is not available: %s was not found in\n%s\n\n"
             "Install Teletext Navigator again to restore it.", what, dir);
    MessageBoxA(owner, msg, "Teletext Navigator", MB_OK | MB_ICONEXCLAMATION);
}

/* The running viewer, or NULL. */
static HWND find_viewer(void)
{
    return FindWindowA("MS_WINHELP", NULL);
}

/* Start TTWINHLP.EXE hidden and wait (at most 10 s) for its window. */
static HWND start_viewer(HWND owner, const char *dir)
{
    char exe[MAX_PATH], cmd[MAX_PATH + 16];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD t0;
    HWND w = NULL;

    snprintf(exe, sizeof exe, "%s\\%s", dir, NAV_HELP_VIEWER);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        help_error(owner, NAV_HELP_VIEWER, dir);
        return NULL;
    }
    snprintf(cmd, sizeof cmd, "\"%s\" -x", exe);
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        help_error(owner, NAV_HELP_VIEWER, dir);
        return NULL;
    }
    WaitForInputIdle(pi.hProcess, 10000);
    t0 = GetTickCount();
    while (!(w = find_viewer()) && GetTickCount() - t0 < 10000) {
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;   /* it exited */
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w;
}

static BOOL send_command(HWND owner, WORD command, LONG data, BOOL start)
{
    char dir[MAX_PATH], file[MAX_PATH];
    unsigned char buf[sizeof(nav_winhelp_t) + MAX_PATH + 2];
    nav_winhelp_t *wh = (nav_winhelp_t *)buf;
    COPYDATASTRUCT cds;
    size_t flen;
    HWND viewer;
    DWORD_PTR result = 0;
    DWORD pid = 0;

    help_dir(dir, sizeof dir);
    snprintf(file, sizeof file, "%s\\%s", dir, NAV_HELP_FILE);
    if (command != HELP_QUIT && GetFileAttributesA(file) == INVALID_FILE_ATTRIBUTES) {
        help_error(owner, NAV_HELP_FILE, dir);
        return FALSE;
    }
    viewer = find_viewer();
    if (!viewer) {
        if (!start) return TRUE;           /* HELP_QUIT with no viewer: done */
        viewer = start_viewer(owner, dir);
        if (!viewer) return FALSE;
    }

    flen = strlen(file) + 1;
    ZeroMemory(buf, sizeof buf);
    wh->size = (WORD)(sizeof *wh + flen + 1);
    wh->command = command;
    wh->data = data;
    wh->ofsFilename = (WORD)sizeof *wh;
    wh->ofsData = (command == HELP_PARTIALKEY) ? (WORD)(sizeof *wh + flen) : 0;  /* "" */
    memcpy(buf + sizeof *wh, file, flen);

    cds.dwData = WINHELP_MAGIC;
    cds.cbData = wh->size;
    cds.lpData = buf;
    /* let the viewer bring its window to the front */
    GetWindowThreadProcessId(viewer, &pid);
    if (pid) AllowSetForegroundWindow(pid);
    if (!SendMessageTimeoutA(viewer, WM_COPYDATA, (WPARAM)owner, (LPARAM)&cds,
                             SMTO_ABORTIFHUNG, 10000, &result))
        return FALSE;
    if (command != HELP_QUIT) {
        /* the topic's window: bring it forward */
        HWND w = find_viewer();
        if (w && IsWindowVisible(w)) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            SetForegroundWindow(w);
        }
    }
    return TRUE;
}

BOOL suite_help_context(HWND owner, DWORD context)
{
    return send_command(owner, HELP_CONTEXT, (LONG)context, TRUE);
}

BOOL suite_help_contents(HWND owner)
{
    return send_command(owner, HELP_CONTENTS, 0, TRUE);
}

BOOL suite_help_search(HWND owner)
{
    return send_command(owner, HELP_PARTIALKEY, 0, TRUE);
}

BOOL suite_help_on_help(HWND owner)
{
    return send_command(owner, HELP_HELPONHELP, 0, TRUE);
}

void suite_help_quit(HWND owner)
{
    send_command(owner, HELP_QUIT, 0, FALSE);
}
