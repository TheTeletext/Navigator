/*
 * suite_config.h - Settings > Configuration and the configuration file.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Navigator 1.7 (docs/2026-11-15_NAVIGATOR_17_BBS_TAB.md). One setting so
 * far, the Downloads directory: where every download lands whose
 * destination the program decides itself, with no Save dialog (the BBS and
 * RIPscrip Zmodem receives, and the FTP tab's starting local folder).
 *
 * THE FILE. NAVIGATR.INI in %LOCALAPPDATA%\Teletext Navigator\, beside the
 * updater's folder, written with the profile API the way a Windows 3.1
 * program kept its settings:
 *
 *     [Navigator]
 *     DownloadsDir=C:\Users\...\Downloads\Teletext Navigator
 *
 * It is created as UTF-16 (a byte-order mark first), so the profile API
 * keeps a path with Greek or other non-ANSI letters intact. A missing file
 * or key means the default, Downloads\Teletext Navigator under the user's
 * Downloads known folder, which is written to the file the first time it
 * is asked for. The directory itself is created when it is first needed.
 *
 * NAV_CONFIG_INI, when set, names another file. The test gates use it so
 * no gate ever reads or changes the operator's own configuration.
 */

#ifndef SUITE_CONFIG_H
#define SUITE_CONFIG_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The configured Downloads directory, wide. Never empty: falls back to the
 * default (and records it in the file) when nothing is configured. Does
 * NOT create the directory. Returns out. */
wchar_t *suite_config_downloads_dir_w(wchar_t *out, size_t cap);

/* The same, UTF-8, for the char-based callers (the Zmodem receiver takes
 * UTF-8). Returns out. */
char *suite_config_downloads_dir_utf8(char *out, size_t cap);

/* The same in the ANSI code page, for the FTP tab's char paths. Returns 0
 * (and leaves out empty) if the path has a letter the code page cannot
 * hold; the caller then keeps its own default. */
int suite_config_downloads_dir_ansi(char *out, size_t cap);

/* Create the directory, parents included. Returns 1 if it exists after. */
int suite_config_ensure_dir_w(const wchar_t *dir);

/* Store a new Downloads directory. Returns 1 on success. */
int suite_config_set_downloads_dir_w(const wchar_t *dir);

/* The configuration file's full path (for reports and the gates). */
wchar_t *suite_config_file_w(wchar_t *out, size_t cap);

/* Settings > Configuration: the modal dialog. */
void suite_config_dialog(HWND owner);

#ifdef __cplusplus
}
#endif

#endif /* SUITE_CONFIG_H */
