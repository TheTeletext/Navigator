/*
 * suite_branding.c - About dialog and version string.
 *
 * Since Navigator 1.7.1 the About box is the Windows 3.1 information
 * dialog (suite_infodlg.c), laid out as NetManage's About Finger
 * (refshots/ref37): the program's icon, name and version bold on the gray
 * face, a rule, the copyright centred, then the licence and the credits in
 * a white box in the bold System face with its own scroll bar, and OK.
 * Until 1.7 it was a MessageBox: gray text on the gray face (ref36), a
 * Windows 95 look. It makes no sound (no MessageBox, no icon flag).
 *
 * THE NAME SPELLINGS HERE ARE DELIBERATELY PLAIN ASCII. The dialog's text
 * is ANSI (the system code page), and a character outside it arrives as a
 * question mark or worse. Marcin Woloszczuk's name carries an l-with-stroke
 * in the licence text, which CP1252 has no room for, so it is written
 * unaccented here. CREDITS.md and THIRD-PARTY-NOTICES.md are UTF-8 and
 * carry it correctly; they are the surfaces that spell the names in full.
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 */

#include "suite_shell.h"
#include "suite_infodlg.h"

static const char ABOUT_BODY[] =
    "GNU Affero General Public License v3.0 or later.\r\n"
    "See COPYING for the full license text.\r\n"
    "\r\n"
    SUITE_PUBLISHER "\r\n"
    "http://www.teletext.ca\r\n"
    "\r\n"
    "Embedded protocol code (full credits in THIRD-PARTY-NOTICES.md):\r\n"
    "  freeWAIS-sf 2.2.14 (CNIDR 1993)\r\n"
    "  libwww 5.4.1 (W3C Software License)\r\n"
    "\r\n"
    "NABU emulation compiled into this program:\r\n"
    "  Marduk, by S. V. Nickolas and Marcin Woloszczuk. MIT.\r\n"
    "  Its Z80 core by Nicolas Allemand, its TMS9918 core by\r\n"
    "  Troy Schrapel, its AY-3-8910 core by Mitsutaka Okazaki.\r\n"
    "  MGT IPL, this program's fork of OpenNabu IPL by\r\n"
    "  S. V. Nickolas. MIT. Changes listed in MGT-CHANGES.md.\r\n"
    "\r\n"
    "GW-BASIC compiled into this program:\r\n"
    "  Microsoft GW-BASIC (1983 source), Microsoft. MIT.\r\n"
    "  OEM modules by D. Spinellis, S. Gros and T. K. Chia. MIT.\r\n"
    "  Its x86 CPU is libx86emu by SciTech Software,\r\n"
    "  D. Mosberger-Tang, E. Eich and SUSE (S. Winterfeldt).\r\n"
    "  X11-style licence. Screen font: Modern DOS by\r\n"
    "  Jayvee Enaguas. CC0 (public domain). The BBS tab draws\r\n"
    "  its box and block characters with the same font.\r\n"
    "\r\n"
    "Font data used by the RIPscrip renderer:\r\n"
    "  Hershey Fonts, created by Dr. A. V. Hershey while working\r\n"
    "  at the U. S. National Bureau of Standards. Distribution\r\n"
    "  format originally created by James Hurt, Cognition Inc.\r\n"
    "  Public domain.\r\n"
    "  font8x8 by Daniel Hepper, based on the public-domain 8x8\r\n"
    "  VGA font by Marcel Sondaas. Public domain.\r\n"
    "\r\n"
    "Flash player of the Web tab (nav_ruffle.dll):\r\n"
    "  Ruffle, by the Ruffle contributors (https://ruffle.rs).\r\n"
    "  MIT or Apache-2.0. Its components' licences are in\r\n"
    "  LICENSES\\LICENSE.ruffle.txt and LICENSE.ruffle-crates.txt.\r\n"
    "\r\n"
    "Help viewer (TTWINHLP.EXE):\r\n"
    "  The Teletext WinHelp, based on the Wine project's WinHelp\r\n"
    "  viewer. LGPL 2.1 or later (COPYING.LIB); source at\r\n"
    "  https://github.com/TheTeletext/WinHelp.\r\n"
    "\r\n"
    "Newsgroups tab:\r\n"
    "  A read-only Netnews reader (NNTP, RFC 3977). It never posts.\r\n"
    "\r\n"
    "CU-SeeMe tab:\r\n"
    "  Idle screen: RCA Indian-head test pattern (1939),\r\n"
    "  public domain, via Wikimedia Commons.\r\n"
    "\r\n"
    "BBS tab:\r\n"
    "  Modem handshake: \"The Sound of dial-up Internet\" by\r\n"
    "  wtermini (freesound.org). CC0 (public domain).\r\n"
    "  Announcement voice generated with Google Gemini\r\n"
    "  text-to-speech (voice Despina).\r\n"
    "\r\n"
    "RIPscrip is a trademark of TeleGrafix Communications, Inc.\r\n"
    "Flash is a trademark of Adobe Inc.; Ruffle is independent of Adobe.\r\n"
    "\r\n"
    "Dedicated to the loving memory of\r\n"
    "Basile Papadopoulos\r\n"
    "and\r\n"
    "Despina Kavalou-Papadopoulos";

void suite_about_show(HWND parent)
{
    w31_info_t in;
    ZeroMemory(&in, sizeof in);
    in.caption   = "About " SUITE_APP_TITLE;
    in.title     = SUITE_APP_NAME;
    in.version   = "Version " SUITE_VERSION_STR;
    in.subtitle  = "Copyright (c) 2026 Dimitri Papadopoulos\nand The Teletext";
    in.body      = ABOUT_BODY;
    in.buttons   = W31INFO_OK;
    in.max_lines = 14;
    in.width     = 520;
    w31_info_dialog(parent, &in);
}
