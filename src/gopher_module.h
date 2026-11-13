/*
 * gopher_module.h - F3 Gopher client module (plain RFC 1436 over TCP).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * The module follows the suite_shell.h activate/deactivate/resize
 * contract used by every other tab. Public symbols beyond that are
 * confined to the shutdown hook called from WinMain's exit path.
 */

#ifndef GOPHER_MODULE_H
#define GOPHER_MODULE_H

#include "suite_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called once from WinMain after suite_fonts_init / audio_service_init.
 * No-op-safe on repeat calls. */
void gopher_module_init(void);

/* Called once from WinMain's exit path. Frees any cached resources
 * (history strings, in-flight worker handles, image-viewer windows). */
void gopher_module_shutdown(void);

/* Open a gopher:// URL handed over from another tab (2026-11-11: the Web
 * tab's gopher links), exactly as if it were typed in the Selector field
 * and Go pressed. The caller switches to the Gopher tab first. */
void gopher_module_open_url(const char *url);

#ifdef __cplusplus
}
#endif

#endif /* GOPHER_MODULE_H */
