/*
 * telnet_module.h - F6 Telnet client public entry points.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Mirrors gopher_module's lifecycle: init/shutdown bracket the
 * per-process allocations; activate/deactivate/resize/on_command and
 * has_unsaved are the suite_module_t callbacks. The declarations of
 * the suite_module_t callbacks themselves live in suite_shell.h to
 * keep the table-init readable.
 */

#ifndef TELNET_MODULE_H
#define TELNET_MODULE_H

#include "suite_shell.h"

void telnet_module_init(void);
void telnet_module_shutdown(void);

#endif /* TELNET_MODULE_H */
