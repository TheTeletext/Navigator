/*
 * suite_res.h - Numeric resource IDs used by suite.rc and the
 *               AddFontMemResourceEx callers in suite_fonts.c.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#ifndef SUITE_RES_H
#define SUITE_RES_H

/* Application icon -- referenced in suite.rc as `1 ICON ...`. The
 * numeric literal there stays for back-compat with the existing
 * RegisterClassExA wiring; this constant documents intent. */
#define IDR_ICON_APP                1

/* Embedded fonts (RCDATA). Loaded at startup via AddFontMemResourceEx
 * so they become process-private without touching the system font
 * directory. Used today by the F3 Gopher render area; the F6 Telnet
 * render area uses the OS Cascadia Mono (no embedded font needed). */
#define IDR_UBUNTU_MONO_REGULAR    100
#define IDR_UBUNTU_MONO_BOLD       101

/* Logo resource IDs not embedded by Navigator's suite.rc; defined so
 * every source that names them still compiles. */
#define IDR_LOGO_RADIO             200
#define IDR_LOGO_TV                201

/* The Teletext mark (RCDATA, 256 px PNG with alpha). Drawn in grayscale
 * on the CU-SeeMe Start screen. Source: assets/brand/. */
#define IDR_LOGO_TELETEXT          210

/* Channel-bar icon resource IDs: 32x32 PNGs decoded at runtime by
 * suite_logo.c. Not embedded by Navigator's suite.rc.
 * activedesktop_module.c maps each CDF channel item to one of these by
 * title and blits it into the 32x32 icon placeholder, falling back to
 * the gray placeholder square when a resource is missing or fails to
 * decode. Source PNGs live under icons/ at the repo root. */
#define IDR_ICON_NEWS              300
#define IDR_ICON_WEATHER           301
#define IDR_ICON_KAIROS            302
#define IDR_ICON_EISISEIS          303
#define IDR_ICON_HOMEPAGE          304
#define IDR_ICON_NEWSTICKER        305
#define IDR_ICON_LIVERADIO         306

/* GW-BASIC tab (RCDATA, 2026-10-20). The interpreter itself, built from
 * source by firmware/gwbasic/mgt_build.sh and byte-identical to upstream
 * release 20261005, and the two Gazette listings that seed the reader's
 * Documents\MGT BASIC folder on first use (never overwriting). */
#define IDR_GWB_EXE                400
#define IDR_GWB_RETRO649           401
#define IDR_GWB_DIALUP             402

#endif /* SUITE_RES_H */
