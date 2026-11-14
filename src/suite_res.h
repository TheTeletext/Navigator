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

/* Navigator 1.5 Windows 3.1 icon set, 32x32 16-colour BITMAPs from
 * tools/make_w31_icons.py (assets/w31icons). */
#define IDB_W31_HOME               500
#define IDB_W31_RIPSCRIP           501
#define IDB_W31_NABU               502
#define IDB_W31_VIDEOTEX           503
#define IDB_W31_WEB                504
#define IDB_W31_GOPHER             505
#define IDB_W31_ARPANET            506
#define IDB_W31_WAIS               507
#define IDB_W31_ARCHIE             508
#define IDB_W31_IRC                509
#define IDB_W31_CUSEEME            510
#define IDB_W31_TERMINAL           511
#define IDB_W31_GWBASIC            512
#define IDB_W31_FTP                513   /* 2026-11-12 */
#define IDB_W31_NEWSGROUPS         514   /* 2026-11-13 */

/* CU-SeeMe idle test pattern (RCA Indian Head), 1024x768 4 bpp gray,
 * from tools/make_cusm_icons.py (assets/cusmicons/testpattern.bmp). */
#define IDB_CUSM_TESTPATTERN       520

/* Gopher item-type icons, 16x16 16-colour BITMAPs from
 * tools/make_w31_icons.py (assets/w31icons/gt_*.bmp), 2026-11-11. */
#define IDB_W31_GT_MENU              530
#define IDB_W31_GT_TEXT              531
#define IDB_W31_GT_SEARCH            532
#define IDB_W31_GT_TELNET            533
#define IDB_W31_GT_IMAGE             534
#define IDB_W31_GT_VIDEO             535
#define IDB_W31_GT_BINARY            536
#define IDB_W31_GT_SOUND             537
#define IDB_W31_GT_UNKNOWN           538
#define IDB_W31_GT_LINK              539

#endif /* SUITE_RES_H */
