TELETEXT NAVIGATOR 1.6
The Teletext, November 2026

Teletext Navigator is the retro internet client of The Teletext
(http://www.teletext.ca): one Windows app with a client for each of
the live retro services of The Teletext, the way they were reached
in the 1980s and 1990s.

  RIPscrip     a RIPscrip 1.54 graphical terminal
               (ripscrip.teletext.ca)
  NABU         a built-in NABU Personal Computer that boots onto
               The Teletext NABU channel (nabu.teletext.ca)
  Videotex     Minitel and NAPLPS terminals (videotex.teletext.ca)
  Web          a Web 1.0 browser built on W3C libwww
               (http://www.teletext.ca)
  Gopher       Gopher, RFC 1436 (gopher.teletext.ca)
  ARPANET      FTP-Mail, RFC 765/691 (arpanet.teletext.ca)
  WAIS         WAIS search over Z39.50-1988 via freeWAIS-sf
               (wais.teletext.ca)
  Archie       Archie search of the anonymous FTP archive over
               the original Prospero/ARDP protocol
               (archie.teletext.ca)
  FTP          anonymous FTP, download only (ftp.teletext.ca)
  Newsgroups   a read-only Netnews reader, NNTP (news.teletext.ca)
  IRC          a read-only view of the #retro channel
               (irc.teletext.ca)
  CU-SeeMe     CU-SeeMe live television over UDP
               (cu-seeme.teletext.ca)
  Terminal     a VT220 Telnet terminal with Zmodem downloads and
               Finger and QOTD shortcuts (telnet.teletext.ca)
  GW-BASIC     Microsoft's own GW-BASIC, running inside the app

Every tab starts on its teletext.ca address.

The Videotex tab is a terminal for the two videotex formats of the
1980s, chosen with its Format menu. Minitel, the French Teletel
terminal: a 40 x 25 screen with the Minitel's own keyboard on
screen (SOMMAIRE, ENVOI, RETOUR and the rest), its PC keyboard
equivalents and optional CRT scan lines, on port 516. NAPLPS, the
North American format of Bell Canada's Alex service in Montreal:
drawn pages on the 256 x 200 videotex raster, on port 1983. Both
decoders were written for this project from the published
specifications.

The NABU tab is a built-in emulator of the 1980s NABU Personal
Computer. Press Connect and a NABU boots inside the app, in
seconds, straight onto The Teletext NABU channel. A custom host
and port reaches any other NABU server. The emulation is the
Marduk core (MIT) compiled in, booting on MGT IPL, this project's
fork of OpenNabu IPL (MIT). If you own a real NABU, the app can
also act as its Internet Adapter over a serial cable.

The Web tab plays Flash movies inside the page, the way a 1990s
browser with the Flash plug-in did: a movie placed in a page with
EMBED or OBJECT plays in its own box, and a .swf address opened
directly fills the window. Click a movie to give it the mouse and
keyboard; its sound plays through your normal speakers. A movie
that cannot be played shows a placeholder in its place. The player
is Ruffle (https://ruffle.rs), a Flash Player emulator by the
Ruffle contributors, MIT or Apache-2.0, built into the app as
nav_ruffle.dll; nothing is downloaded. Its licence texts, and those
of every component built into it, are in the LICENSES directory
(LICENSE.ruffle.txt, LICENSE.ruffle-crates.txt). Flash is a
trademark of Adobe Inc.; Ruffle is independent of Adobe.

The GW-BASIC tab runs Microsoft's 1983 GW-BASIC source (MIT),
built by T. K. Chia's project, unmodified on a small IBM PC inside
the app (the libx86emu x86 core). LOAD and SAVE use the Teletext
BASIC folder in your Documents, which comes with two type-in
listings (LOAD "RETRO649" and LOAD "DIALUP"). Ctrl+V types in a
listing copied from the web.

HELP AND THE MANUAL

Help, Contents (or F1 on any tab) opens the help in WinHelp, the
help viewer of Windows 3.1, brought back to Windows 11: The
Teletext WinHelp (TTWINHLP.EXE), a native port of the Wine
project's WinHelp viewer, licensed under the GNU LGPL 2.1 or later
(COPYING.LIB; source at https://github.com/TheTeletext/WinHelp).
The help file, TELETEXT.HLP, was compiled with the Microsoft Help
Compiler of the period: a page for every tab and service, with
Search, Back, History and the glossary. Help, How to Use Help
explains the viewer itself.

The Teletext Navigator User's Guide, a 79-page PDF, installs
beside the app; the Start Menu entry "Teletext Navigator Manual"
opens it.

REQUIREMENTS

Tested on 64-bit Windows 10 22H2 and later, including Windows 11.
Earlier versions are untested. Nothing else to install.

INSTALL

Run the installer, Teletext_Navigator_1.6.0.msi. It installs to
Program Files\Teletext Navigator for all users and adds a Start
Menu entry; the desktop shortcut is optional and can be turned off
during setup. Uninstall from Settings, Apps and features.

The installer is not code-signed, so Windows SmartScreen will
probably show "Windows protected your PC" the first time. Choose
More info, then Run anyway. You can check the download against the
SHA-256 published beside it before you do.

The app checks for its own updates: Help, then Check for Updates.

LICENSE

Teletext Navigator is free software, released under the GNU Affero
General Public License v3.0 or later; see LICENSE.TXT. Third-party
attribution and license texts are in NOTICES.TXT and the LICENSES
directory.

The complete Corresponding Source is at

  https://github.com/TheTeletext/Navigator

Build it with build_x64.bat under MSYS2 UCRT64, and the installer
with installer\build_navigator_msi.bat (it takes the help viewer
from a checkout of github.com/TheTeletext/WinHelp beside this one,
or WINHELP_DIR). The GW-BASIC interpreter
the app embeds is built from firmware/gwbasic/ by mgt_build.sh
under the MSYS2 POSIX shell, and is checked against upstream's own
release.

CREDITS

CREDITS.TXT names the people whose protocols this app speaks, and
carries the dedication.

CONTACT

The Teletext
http://www.teletext.ca
