# Teletext Navigator

The retro internet client of [The Teletext](http://www.teletext.ca): one
Windows app with a client for each live retro service of The Teletext, each
speaking the real protocol to a real server rather than imitating it.

[![license](https://img.shields.io/badge/license-AGPL--3.0-blue)](COPYING)

---

## Status

**1.5**, November 2026.

---

## What it does

| tab | protocol | default server |
| --- | --- | --- |
| RIPscrip | RIPscrip 1.54 over telnet | ripscrip.teletext.ca:23 |
| NABU | a built-in NABU PC on the NABU adapter protocol | nabu.teletext.ca:5816 |
| Videotex | Minitel (port 516) and NAPLPS (port 1983) | videotex.teletext.ca |
| Retro Web Browser | HTTP, W3C libwww 5.4.1 | http://www.teletext.ca/ |
| Gopher | RFC 1436 | gopher.teletext.ca:70 |
| ARPANET FTP-Mail | RFC 765 and RFC 691 | arpanet.teletext.ca:2121 |
| WAIS | Z39.50-1988, freeWAIS-sf | wais.teletext.ca:210, database `teletext` |
| Archie | Prospero ARDP over UDP | archie.teletext.ca:1525, telnet on 2323 |
| FTP | RFC 959, anonymous, download only | ftp.teletext.ca:21 |
| Newsgroups | NNTP, RFC 3977, read-only | news.teletext.ca:119 |
| IRC | RFC 1459, read-only view | irc.teletext.ca:6667, channel `#retro` |
| CU-SeeMe Live TV | Cornell CU-SeeMe over UDP | cu-seeme.teletext.ca:7648 |
| Terminal | Telnet VT220 with Zmodem, Finger and QOTD | telnet.teletext.ca |
| GW-BASIC | Microsoft GW-BASIC on a built-in IBM PC | (local) |

The NABU tab emulates a NABU Personal Computer inside the app (the Marduk
core, booting on MGT IPL, this project's fork of OpenNabu IPL), and can also
act as a real NABU's Internet Adapter over a serial cable. The GW-BASIC tab
runs Microsoft's own 1983 GW-BASIC, unmodified, on the libx86emu x86 core.

---

## Download

The installer, with its SHA-256, is on the download page:
http://www.teletext.ca/software

Run `Teletext_Navigator_1.5.0.msi`. It installs to
`Program Files\Teletext Navigator` for all users.

**The installer is not code-signed.** Windows SmartScreen will probably show
"Windows protected your PC" the first time: choose **More info**, then
**Run anyway**. Check the download against the SHA-256 on the download page
first.

The app checks for its own updates (Help, then Check for Updates) at
https://apps.teletext.ca/navigator/manifest.json, and verifies every
installer against the SHA-256 in that manifest before running it.

---

## Requirements

Tested on 64-bit Windows 10 22H2 and later, including Windows 11.
Earlier versions are untested. Nothing else to install.

---

## Build

MSYS2 UCRT64, GCC 16. From the project root:

```
build_x64.bat
installer\build_navigator_msi.bat
```

---

## License

Free software under the **GNU Affero General Public License, version 3 or
later**. See [COPYING](COPYING).

Third-party components keep their own licences, listed in
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) and the `licenses/`
directory.

---

## Credits

The protocol authors and the projects this is built on are named in
[CREDITS.md](CREDITS.md).

---

Dedicated to the loving memory of

Basile Papadopoulos
and
Despina Kavalou-Papadopoulos

---

## Contact

The Teletext
http://www.teletext.ca
