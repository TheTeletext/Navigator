/*
 * suite_version.h - THE version and THE identity of Teletext Navigator.
 * One place.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * ------------------------------------------------------------------
 * TO RELEASE A NEW VERSION, EDIT THE THREE NUMBERS AND THE SUFFIX
 * BELOW AND NOTHING ELSE.
 * ------------------------------------------------------------------
 *
 * The version lives here and nowhere else. Everything derives from the
 * three integers below, so its spellings cannot disagree.
 *
 * WHO CONSUMES THIS
 *
 *   src/suite.rc          FILEVERSION / PRODUCTVERSION (numeric) and
 *                         the FileVersion / ProductVersion strings.
 *                         windres runs the C preprocessor, and it
 *                         concatenates adjacent string literals, so the
 *                         composed macros below reach the resource as
 *                         one string. Verified 2026-08-02.
 *   src/suite_shell.h     SUITE_VERSION_STRING, which the title bar and
 *                         the About box already used.
 *   src/suite_update.c    the version the updater compares against the
 *                         manifest, parsed by the same semantic-version
 *                         parser that reads the manifest's string, so
 *                         our own version and theirs take identical
 *                         code paths.
 *   installer/build_navigator_msi.bat
 *                         reads SUITE_VERSION_MSI back out through the
 *                         C preprocessor (gcc -E) and hands it to WiX
 *                         as the MSI ProductVersion. Nothing about the
 *                         version is retyped in the WiX authoring.
 *
 * THE TWO FORMS, AND WHY BOTH EXIST
 *
 *   SUITE_VERSION_MSI  "1.2.0"       numeric triple, no suffix.
 *                                    Windows Installer's ProductVersion
 *                                    is strictly major.minor.build, the
 *                                    fields cap at 255.255.65535, and
 *                                    only the first three are compared
 *                                    for upgrade decisions. It cannot
 *                                    carry a pre-release tag.
 *   SUITE_VERSION_STR  "1.2.0-rc.1"  what a human, the update manifest
 *                                    and the version comparison use.
 *
 * A CONSEQUENCE WORTH KNOWING. Because MSI ignores the suffix, two
 * builds that differ only in their pre-release tag (1.2.0-rc.1 and
 * 1.2.0) carry the SAME ProductVersion and Windows Installer will not
 * treat one as an upgrade of the other. Bump the patch number when a
 * pre-release becomes a release, or ship the release as 1.2.1.
 */

#ifndef SUITE_VERSION_H
#define SUITE_VERSION_H

/* ---- edit these, and only these ---------------------------------- */

#define SUITE_VERSION_MAJOR   1
#define SUITE_VERSION_MINOR   0
#define SUITE_VERSION_PATCH   0

/* Pre-release suffix, including its leading hyphen. Set to "" for a
 * final release. Ordering rule, which the updater's comparator
 * implements: a pre-release sorts BEFORE the release it belongs to, so
 * 1.2.0-rc.1 is older than 1.2.0. */
#define SUITE_VERSION_SUFFIX  ""

/* Build number: the 4th field of the exe's FILEVERSION, and nothing else
 * (the title, the MSI ProductVersion and the updater see only the triple
 * above). Bump it for every rebuild of the SAME version that must reach an
 * installed copy: the MSI upgrades in place (AllowSameVersionUpgrades),
 * but Windows Installer keeps an installed file whose version is not
 * lower, so an unbumped exe is silently KEPT (observed 2026-10-29,
 * docs/2026-10-30_NAVIGATOR_PHASE2_BUILD.md 5.1). Reset to 0 with a new
 * version. */
#define SUITE_VERSION_BUILD   9

/* ---- everything below is derived; do not edit --------------------- */

#define SUITE_VER_STR2_(x)  #x
#define SUITE_VER_STR_(x)   SUITE_VER_STR2_(x)

/* "1.0.0". Numeric triple for the MSI ProductVersion. */
#define SUITE_VERSION_MSI \
    SUITE_VER_STR_(SUITE_VERSION_MAJOR) "." \
    SUITE_VER_STR_(SUITE_VERSION_MINOR) "." \
    SUITE_VER_STR_(SUITE_VERSION_PATCH)

/* "1.0.0", plus the suffix if any. The canonical human and manifest form. */
#define SUITE_VERSION_STR   SUITE_VERSION_MSI SUITE_VERSION_SUFFIX

/* "v1.0.0". The same thing with the leading v, kept for anywhere
 * that still wants it. The title bar and the About box do NOT: they show
 * the bare version after the name. */
#define SUITE_VERSION_DISPLAY  "v" SUITE_VERSION_STR

/* "1.0", or "1.0.1" once there is a patch. What the title bar shows:
 * the operator's title is exactly "Teletext Navigator 1.0", and a hotfix
 * must still be visible there, so the patch appears only when it is not
 * zero. The updater and the MSI keep using the full triple. */
#if SUITE_VERSION_PATCH == 0
#define SUITE_VERSION_TITLE \
    SUITE_VER_STR_(SUITE_VERSION_MAJOR) "." \
    SUITE_VER_STR_(SUITE_VERSION_MINOR) SUITE_VERSION_SUFFIX
#else
#define SUITE_VERSION_TITLE  SUITE_VERSION_STR
#endif

/* ------------------------------------------------------------------
 * THE IDENTITY.
 * ------------------------------------------------------------------
 *
 * The MSI reads SUITE_APP_NAME_LONG and SUITE_APP_NAME_SHORT back out
 * through the C preprocessor in installer\build_navigator_msi.bat,
 * exactly as it does the version, so the installer and the application
 * cannot disagree about what the product is called. Both are the same
 * name.
 */
#define SUITE_APP_NAME        "Teletext Navigator"
#define SUITE_APP_NAME_LONG   SUITE_APP_NAME
#define SUITE_APP_NAME_SHORT  SUITE_APP_NAME
#define SUITE_PUBLISHER       "The Teletext"

/*
 * NABU_BRIDGE_TEST is a TEST-ONLY bench build (2026-10-31, gate G2 of the
 * NABU hang fix): the NABU serial bridge at 115200 on the bench's COM1.
 * It must never be installed or published, so its title says so in
 * capitals, and it keeps its settings under its own key so that running
 * it cannot touch an installed Navigator's.
 */
#ifdef NABU_BRIDGE_TEST
#define SUITE_WINDOW_TITLE    SUITE_APP_NAME " " SUITE_VERSION_TITLE \
    " - TEST ONLY, NABU BRIDGE AT 115200 - DO NOT INSTALL OR PUBLISH"
#define SUITE_SETTINGS_KEY    "Software\\The Teletext\\Teletext Navigator BRIDGE TEST ONLY"
#else
/* Exactly what the title bar shows: "Teletext Navigator 1.0". */
#define SUITE_WINDOW_TITLE    SUITE_APP_NAME " " SUITE_VERSION_TITLE

/* Per-user settings, HKCU. The MSI's HKLM key has the same path. */
#define SUITE_SETTINGS_KEY    "Software\\The Teletext\\Teletext Navigator"
#endif

/* %LOCALAPPDATA%\<this>\ holds the updater's downloads. */
#define SUITE_DATA_DIR_W      L"Teletext Navigator"

/* Documents\<this>\ is where GW-BASIC LOADs and SAVEs. */
#define SUITE_BASIC_DIR_W     L"Teletext BASIC"
#define SUITE_BASIC_DIR       "Teletext BASIC"

/* The HTTP User-Agent product token. */
#define SUITE_UA_PRODUCT      "Teletext-Navigator"

#endif /* SUITE_VERSION_H */
