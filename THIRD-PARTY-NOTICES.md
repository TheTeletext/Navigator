# Third-Party Notices

Teletext Navigator incorporates the third-party components listed
below. Each section gives the upstream origin, the copyright notice as
found in the actual source files, the governing license, where the full
license text lives in this repository, and what the program uses the
component for.

Copyright signs that appear as the ISO-8859-1 byte 0xA9 in the original
files are rendered here as "(C)".

## freeWAIS-sf 2.2.14

- Upstream origin: freeWAIS-sf 2.2.14 (released 2000-06-07), the final
  release of the Pfeifer/Govert lineage at Universitaet Dortmund,
  descended from CNIDR freeWAIS and the Thinking Machines WAIS
  implementation. Tarball hosted at
  https://www.nic.funet.fi/index/files/index/networking/services/freeWAIS/freeWAIS-sf/
- Copyright, as found in the source headers (for example
  `src/wais/zutil.c`):
  - `(C) Copyright 1997, Universitaet Dortmund, all rights reserved.`
  - `(C) Copyright CNIDR (see ../doc/CNIDR/COPYRIGHT)`
  and in the license text itself:
  - `Copyright (c) MCNC, Clearinghouse for Networked Information
    Discovery and Retrieval, 1993.`
- License: CNIDR 1993 permissive license.
- License text: `licenses/CNIDR-1993.txt` (reproduced from
  `doc/CNIDR/COPYRIGHT` in the upstream 2.2.14 tarball).
- Used for: the WAIS protocol engine (Z39.50-1988 encode/decode,
  transport, document IDs) behind the F2 WAIS Search module. The
  preserved protocol sources live in `src/wais/`.

## W3C libwww 5.4.1

- Upstream origin: W3C's libwww reference library, version 5.4.1, from
  the W3C source repository (GitHub mirror tag `5.4.1`). Preserved in
  `src/libwww/`.
- Copyright, as found in `src/libwww/COPYRIGH` and the per-file
  headers (`(c) COPYRIGHT MIT 1995.`):
  - `Copyright (C) 1995-2003 World Wide Web Consortium, (Massachusetts
    Institute of Technology, European Research Consortium for
    Informatics and Mathematics, Keio University). All Rights
    Reserved.`
  - `Copyright (C) 1995 CERN. "This product includes computer software
    created and made available by CERN. This acknowledgment shall be
    mentioned in full in any product which includes the CERN computer
    software included herein or parts thereof."`
- License: W3C IPR Software Notice (the W3C Software Notice and
  License; permissive and GPL-compatible).
- License text: `licenses/W3C-Software-Notice.txt` (plain-text
  extraction of `src/libwww/LICENSE.html`; the HTML original and
  `src/libwww/COPYRIGH` are kept in-tree).
- Historical note: the libwww tree also ships `src/libwww/COPYING.LIB`
  (LGPL v2). That file is historical; the governing upstream license
  for libwww is the W3C Software Notice above. Both files are preserved
  in-tree and both licensors are credited.
- Used for: the HTTP/HTML retrieval and parsing engine behind the F4
  Web module (and the Active Channel / Active Desktop scenes built on
  it).

## FFmpeg TrueSpeech decoder

- Upstream origin: the DSP Group TrueSpeech compatible decoder from the
  FFmpeg project (libavcodec), adapted into
  `src/truespeech_decoder.[ch]` and `src/truespeech_data.h`.
- Copyright, as found in `src/truespeech_decoder.c`:
  - `Copyright (c) 2005 Konstantin Shishkov`
- License: GNU Lesser General Public License v2.1 or later.
- License text: `licenses/LGPL-2.1.txt`.
- Compliance note: LGPL v2.1 section 3 permits relicensing a copy under
  the GPL; the "or later" grant reaches GPLv3, and AGPLv3 links with
  GPLv3 code. Navigator publishes its complete corresponding source, so
  all source-availability obligations are satisfied.
- Used for: decoding DSP Group TrueSpeech audio in the retro Audio
  Player.

## minimp3

- Upstream origin: https://github.com/lieff/minimp3, vendored as
  `src/minimp3.h`.
- Copyright, as found in `src/minimp3.h`:
  - `To the extent possible under law, the author(s) have dedicated all
    copyright and related and neighboring rights to this software to
    the public domain worldwide.`
- License: CC0 1.0 Universal (public domain dedication), per
  http://creativecommons.org/publicdomain/zero/1.0/ as referenced in
  the header.
- License text: the dedication in the `src/minimp3.h` header itself.
- Used for: MP3 decoding in the shared audio service (Gopher and Web
  module audio playback, retro Audio Player).

## Ubuntu Font (Ubuntu Mono)

- Upstream origin: the Ubuntu font family by Canonical Ltd (with
  Dalton Maag). Bundled as `src/fonts/UbuntuMono-Regular.ttf` and
  `src/fonts/UbuntuMono-Bold.ttf`, embedded in the binary as RCDATA
  resources.
- Copyright: Canonical Ltd, licensed under the terms in the in-tree
  license file.
- License: Ubuntu Font Licence, Version 1.0.
- License text: `src/fonts/COPYING.UbuntuFontLicense.txt` (in-tree
  copy).
- Used for: the F3 Gopher client's rendering font.

## GCC runtime (libgcc, libssp)

- Upstream origin: the GNU Compiler Collection (MinGW-w64 / MSYS2
  UCRT64 toolchain). Navigator links these runtime libraries
  statically.
- Copyright: Free Software Foundation, Inc.
- License: GPLv3 with the GCC Runtime Library Exception, which permits
  distributing the runtime as part of programs under any license.
- License text: https://www.gnu.org/licenses/gcc-exception-3.1.html
- Used for: compiler runtime support and stack-smashing protection
  (`-fstack-protector-strong`, `-lssp`) in the shipped binary.

## Hershey Fonts (RIPscrip stroked fonts 1 to 10)

- Upstream origin: the public-domain Hershey vector font distribution,
  in the `.jhf` format. The glyphs are the digitised vector letterforms
  developed by **Dr. Allen Vincent Hershey at the United States National
  Bureau of Standards** (now the National Institute of Standards and
  Technology) in the 1960s. As work produced at a US federal agency the
  vector data is in the **public domain**, and it has been distributed
  as such for decades, most widely through the Usenet `.jhf` collection.
- What is in this repository: not the upstream `.jhf` files themselves
  but generated C tables, `src/rip_hershey_data.c`, produced from them
  by `tools/gen_hershey.py`. The generator reads the `.jhf` coordinate
  pairs and emits them as a compact `signed char` array; it performs no
  redrawing and adds no glyph of its own, so the shipped outlines are
  the public-domain Hershey outlines and nothing else.
- License: **public domain**. The distribution asks that the following
  two acknowledgements accompany the font data, and they are reproduced
  here verbatim for that purpose. They are also carried in the header of
  `src/rip_hershey_data.c` and shown in Navigator's About box.

  - The Hershey Fonts were originally created by Dr. A. V. Hershey
    while working at the U. S. National Bureau of Standards.
  - The format of the Font data in this distribution was originally
    created by James Hurt, Cognition, Inc., 900 Technology Park Drive,
    Billerica, MA 01821.

  Note that the second acknowledgement credits the `.jhf` FILE FORMAT,
  not the letterforms. Neither acknowledgement is a copyright claim and
  neither imposes a condition beyond attribution.
- Used for: the RIPscrip stroked fonts, font numbers 1 to 10, in the
  RIPscrip tab. RIPscrip names the same font families the Hershey set
  provides, and the 1.54 specification's own font metric tables size
  them at exactly the Hershey natural capital height.
- **WHY PUBLIC-DOMAIN GLYPHS SPECIFICALLY.** A RIPscrip terminal of the
  period drew its stroked fonts from Borland's BGI `.CHR` stroke files,
  which are proprietary and are not redistributable. Sourcing the
  letterforms from the public-domain Hershey set is what lets this
  renderer produce the correct stroked output with **no proprietary font
  data of any kind in the tree or in the shipped binary**. It is the
  same discipline applied to the Zmodem CRC files described below:
  where a component would have brought in a licence Navigator cannot
  ship, it was replaced with one that carries no such condition rather
  than worked around.
- Not used: no Borland `.CHR` font file, no BGI font data, and no
  TeleGrafix font asset is read, converted, embedded or redistributed by
  this project.

## font8x8 (RIPscrip default font 0)

- Upstream origin: `font8x8` by Daniel Hepper, itself based on the
  public-domain 8x8 VGA font by Marcel Sondaas. Vendored as a generated
  C table in `src/rip_font8x8_data.c`, produced by
  `tools/gen_font8x8.py`.
- License: public domain.
- Used for: the RIPscrip default 8x8 bitmap font (font number 0) in the
  RIPscrip tab.

## RIPscrip protocol

- RIPscrip is a trademark of TeleGrafix Communications, Inc. This
  project implements the published RIPscrip 1.54 protocol
  specification, pinned at `docs/ripscrip-ref/RIPSCRIP-1.54.DOC`. No
  TeleGrafix code or asset is used or redistributed.

## mbzm (Zmodem receive core)

- Upstream origin: mbzm, https://github.com/roscopeco/mbzm, a small
  receive-only Zmodem implementation written against the published
  ZMODEM protocol specification. Vendored in `third_party/mbzm/`.
- Copyright, as found in the source headers and `LICENSE`:
  - `Copyright (c)2020 Ross Bamford`
- License: MIT.
- License text: `licenses/LICENSE.mbzm.txt` (a copy of the upstream
  `LICENSE`, which is also kept in-tree at `third_party/mbzm/LICENSE`).
- Used for: the Zmodem protocol primitives (header encode and decode,
  ZDLE escaping, data subpacket reads) behind the RIPscrip tab's file
  download. The session driver, the Win32 threading, the file naming
  policy and every security decision are Navigator code in
  `src/zmodem_recv.[ch]` and are not part of the vendored core.
- MODIFICATIONS, and this one matters for licensing: upstream mbzm
  vendors its two CRC files (`crc16.c`, `crc32.c` and their headers)
  from Synchronet's smblib, which is **GNU GPL v2 or later**, not MIT.
  Navigator ships no copyleft code, so all four files were REMOVED from
  the vendored tree and replaced with implementations written from the
  polynomial definitions alone (CRC-16/XMODEM, poly 0x1021, init 0; and
  CRC-32, reflected poly 0xEDB88320, init 0xFFFFFFFF, final complement).
  The replacements build their tables at run time, are placed in the
  public domain by their author, and are checked against the standard
  "123456789" vectors. Nothing from Synchronet, lrzsz or Omen
  Technology is present in the binary or in this repository.
  See `licenses/LICENSE.mbzm-smblib-REMOVED.txt` for the full account
  and `third_party/mbzm/MGT-CHANGES.md` for the vendoring notes.
  Upstream's `rz.c` example driver was also dropped.

## Marduk (NABU emulation core, compiled into Navigator)

- What it is: the emulation core of Marduk, an emulator for the NABU
  Personal Computer. The **NABU** tab runs a NABU inside Navigator's own
  window, and this is the code that emulates the machine. It is compiled
  and linked directly into `TeletextNavigator.exe`.
- Upstream origin: https://github.com/buricco/marduk, upstream commit
  `bd99967c2aded5f8a5fe25ac3bbb595a71b0dfea` (2026-03-03). Vendored at
  `third_party/marduk/`.
- Licence: **MIT**. The grant is one text covering four copyright
  holders, because Marduk carries three chip emulation cores of separate
  authorship:
  - Z80 emulation, Copyright (c) 2019 **Nicolas Allemand**
  - TMS9918 emulation, Copyright (c) 2021, 2022 **Troy Schrapel**
  - AY-3-8910 emulation, Copyright (c) 2001-2022 **Mitsutaka Okazaki**
  - NABU emulation, Copyright 2022, 2023 **S. V. Nickolas**, and
    Copyright 2023 **Marcin Wołoszczuk**
  The full text is upstream's own and is reproduced unmodified at
  `third_party/marduk/license.txt`. It ships with the binary at
  `LICENSES\LICENSE.marduk.txt`, which is a byte-identical copy, because
  MIT requires the copyright and permission notice to accompany every
  copy of the software.
- Which files are compiled in: `z80.c`, `tms9918.c`, `tms_util.c`,
  `emu2149.c`, `disk.c`, `modem.c`. Every one was checked individually
  and each carries the same MIT grant in its own header.
- What Navigator modified: Navigator's changes to the vendored copy are
  listed in `third_party/marduk/MGT-CHANGES.md`, and each modified file
  is marked as modified in its own header, as MIT requires.
- What is NOT Marduk: the machine driver, the frame loop, the lifecycle,
  the GDI renderer, the keyboard and every other Win32 decision live in
  `src/nabu_core.[ch]` and `src/nabu_native_module.[ch]`. Those are the
  Navigator's own work and are AGPLv3 like the rest of it.
- Used for: the NABU tab.

## nabud (interoperability reference, no code used)

This entry exists to record a conclusion, not to satisfy an obligation.

- What it is: nabud, a NABU Network adaptor server by **Jason R.
  Thorpe**, https://github.com/thorpej/nabud. It is the server the
  Montreal Greek Times NABU channel runs, and it is the far end that the
  NABU tab and the serial bridge talk to.
- Licence: **BSD 2-Clause**, Copyright (c) 2022 Jason R. Thorpe.
  Confirmed by reading the `LICENSE` file and the per-file headers of
  the running installation rather than assumed.
- What was taken from it: nothing that its licence reaches. Two things
  in Navigator were settled by reading nabud's source, and both are
  facts about an external wire protocol rather than nabud's expression
  of anything:
  - The CHANGE_CHANNEL exchange in `src/nabu_channel.c`, namely the
    message byte `0x85`, the two-byte acknowledgement `0x10 0x06`, the
    little-endian channel number and the `0xE4` confirmation. These are
    the classic NABU adapter protocol, which a real 1982 machine emits.
    nabud does not originate them either: its own `adaptor.c` credits
    them to NabuNetworkEmulator (`AdaptorEmulator.cs`) by Nick Daniels.
    nabud's code is also the opposite side of the conversation, a server
    receiving; Navigator's is a client sending, written from scratch
    against a different transport abstraction.
  - The native serial rate in `src/nabu_serial.h`, `(3579540 / 2) / 16`,
    which is 111860.625 bits per second. That is a division of the
    NABU's own crystal, a hardware fact, and nabud's `conn.c` derives it
    the same way for the same reason.
- Conclusion: **no attribution is required**, because no copyrightable
  expression was copied. It is credited here anyway, because reading
  someone's source to learn a protocol is a debt even when it is not a
  legal one, and because the in-source comments that cite nabud should
  resolve to something a reader can find.
- Consequently nabud is NOT listed in the Help > About box, which names
  what Navigator embeds and ships.

## OpenNabu IPL, and MGT IPL (Navigator's fork of it)

- What they are: replacement boot firmware for the NABU Personal
  Computer, 4 KB, written in Z80 assembly. The **NABU** tab boots
  the emulated machine on `firmware/mgtipl/mgtipl.bin`, which is the
  Navigator's own fork, and falls back to stock OpenNabu IPL
  (`third_party/opennabu/opennabu.bin`) if the fork is missing.
- Upstream origin: OpenNabu IPL by **S. V. Nickolas** (buricco),
  https://github.com/buricco/opennabuipl. The same author wrote Marduk,
  the NABU emulation core the native tab is a port of.
- Licence: **MIT**, Copyright 2012, 2013, 2015, 2023 S. V. Nickolas. The
  full text is at `third_party/opennabuipl/license.txt` and
  `firmware/mgtipl/license.txt`, and is reproduced at the head of every
  copy of the source. It is the licence that governs the fork as well.
  It ships with the binary at `LICENSES\LICENSE.opennabu.txt`, which is
  a byte-identical copy of upstream's own file.
- What Navigator ships: the unmodified upstream source, vendored at
  `third_party/opennabuipl/`; Navigator's **modified** copy at
  `firmware/mgtipl/mgtipl.a80`, with every change listed in
  `firmware/mgtipl/MGT-CHANGES.md`; and the assembled images. Modified
  copies are marked as such in the file header, as MIT requires the
  copyright and permission notice to be preserved.
- BOTH images are assembled from source this repository holds:
  `mgtipl.bin` from the fork, and the fallback `opennabu.bin` (sha256
  `44ee0854bb85896a...`) from the vendored upstream source
  `third_party/opennabuipl/opennabu.a80` with Debian z80asm 1.8.
- Installed, both images sit in the program's NABU folder, as
  `NABU\mgtipl.bin` and `NABU\opennabu.bin`;
  `installer\build_navigator_msi.bat` stages both.
- Why a fork: the tab is an emulated NABU tuned to one channel, so the
  fork removes the floppy probe, the Winchester probe, the power-on RAM
  test, the boot menu and the last-device memory, and goes straight to
  the channel. It does not change one byte of what goes on the wire.
- Note that this is REPLACEMENT firmware, openly licensed, and is not the
  preserved NABU firmware described below. The native tab needs no
  preserved ROM at all.

## WiX Toolset (installer build tooling, and installer UI resources)

- Upstream origin: the WiX Toolset, https://wixtoolset.org/, source at
  https://github.com/wixtoolset/wix. Installed as a .NET global tool and
  pinned; it is not vendored in this repository.
- Versions pinned and used to build the MSI:
  - `wix` **5.0.2**
  - `WixToolset.UI.wixext` **5.0.2**
  - `WixToolset.Util.wixext` **5.0.2**
- Copyright, as found in the project's `LICENSE.TXT` at tag `v5.0.2` and
  in the NuGet metadata of all three packages:
  - `Copyright (c) .NET Foundation and contributors. All rights reserved.`
- License: the **Microsoft Reciprocal License (MS-RL)**. Stated in
  `LICENSE.TXT` at tag `v5.0.2` ("This software is released under the
  Microsoft Reciprocal License (MS-RL)") and declared as the SPDX
  expression `MS-RL` in the `wix`, `WixToolset.UI.wixext` and
  `WixToolset.Util.wixext` 5.0.2 packages. License text:
  http://opensource.org/licenses/ms-rl
- Used for: building `Teletext_Navigator_<version>.msi` from
  `installer/TeletextNavigator.wxs`. See
  `installer/build_navigator_msi.bat`.
- **IT IS NOT ONLY BUILD TOOLING.** WiX also contributes content that is
  redistributed inside the shipped MSI, so it is credited here rather
  than treated as a build-time-only dependency the way ImageMagick and
  the MSYS2 toolchain are:
  - two compiled custom-action libraries embedded as MSI binary
    streams, `Wix4UtilCA_X64` (from the Util extension, used for the
    close-the-running-app prompt) and `WixUiCa_X64` (from the UI
    extension);
  - the `WixUI_FeatureTree` dialog set, which is WiX-authored content
    compiled into the package's Dialog and Control tables;
  - four small stock UI images kept as WiX shipped them:
    `WixUI_Ico_Exclam`, `WixUI_Ico_Info`, `WixUI_Bmp_New` and
    `WixUI_Bmp_Up`.
- Not from WiX: the two large wizard images, `WixUI_Bmp_Dialog` and
  `WixUI_Bmp_Banner`, are Teletext artwork (the Teletext monogram)
  generated by `tools/gen_navigator_art.py` and they replace WiX's stock
  bitmaps in the built package. The wizard text is likewise overridden
  from `installer/TeletextNavigator.wxl`.
- Scope note: MS-RL is a reciprocal license that applies per file. It
  covers the WiX files listed above, which are redistributed unmodified
  inside the installer. It does not reach the application itself:
  `TeletextNavigator.exe` contains no WiX code.

## Windows in-box system libraries

Teletext Navigator links against Windows system libraries that ship
with the operating system (ws2_32, comctl32, comdlg32, gdi32, gdiplus,
msimg32, ole32, oleaut32, uuid, winmm, winhttp, shell32, bcrypt,
advapi32, and the UCRT). These are linked at runtime and are not
redistributed with it.

## Combined work license

Teletext Navigator as a combined work is licensed under the GNU
Affero General Public License v3.0 or later. The full license text is
in `/COPYING` at the repository root. The complete corresponding source
is published at https://github.com/TheTeletext/Navigator

## NAPLPS (videotex) tab

NOTHING IS OWED HERE, and this section exists to record that positively
rather than by silence.

The NAPLPS decoder (`src/naplps_decode.[ch]`), the videotex surface
(`src/naplps_surface.[ch]`), the 5x7 glyph set
(`src/naplps_font_data.c`, generated by `tools/gen_naplps_font.py`) and
the tab itself (`src/naplps_module.[ch]`) are original work for this
project, written from the published standard

    ANSI X3.110-1983 / CSA T500-1983
    Videotex/Teletext Presentation Level Protocol Syntax
    (North American PLPS), December 1983

No NAPLPS decoder or renderer source was read, copied or adapted.

TWO PROGRAMS WERE USED AS BLACK-BOX ORACLES during development. Using a
program as an oracle means running it and comparing its output; it ships
no part of either program, so neither licence is triggered. They are
credited in `CREDITS.md` regardless, because this project's habit is to
carry credits that are not legally compelled.

  - naplps.js by Nick Fox-Gieg, from github.com/n1ckfg/Telidon, MIT,
    "Copyright (c) 2021 Nick Fox-Gieg". MIT would require its notice to
    travel with copies of, or substantial portions of, the software. No
    portion travels, so no notice is required. If any naplps.js code is
    ever actually ported into Navigator, this changes: the full MIT
    notice would then have to be added here and to `licenses/`.
  - Personality+III 2.18a, Copyright (c) 1992 Microstar Software Ltd.,
    shareware. Its terms permit non-commercial distribution only if all
    of its files travel together unmodified, so it is NOT redistributed
    with Navigator and is not present in this repository. The reference
    copy is held outside the tree.

NO TRADEMARK NOTICE IS OWED, and that is a deliberate difference from the
RIPscrip modules, which carry one because RIPscrip is a trademark of
TeleGrafix Communications, Inc. NAPLPS is not a trademark: it is the name
of a published ANSI and CSA standard, cited here by number. "Alex" and
"Alextel" were Bell Canada service and product names for a service
withdrawn in 1994 and are used only as historical context and as a stated
compatibility target.

THE STANDARD IS NOT REDISTRIBUTED. ANSI X3.110-1983 / CSA T500-1983 is a
copyrighted document. It is held as a reference copy outside this
repository, exactly as `docs/ripscrip-ref/RIPSCRIP-1.54.DOC` is held for
the RIPscrip renderer, and it must never be added to the installer.

## Minitel mode of the Videotex tab

NOTHING IS OWED HERE EITHER, recorded positively for the same reason.

The Minitel decoder (`src/minitel_decode.[ch]`), its renderer
(`src/minitel_screen.[ch]`), its glyphs (`src/minitel_font.h`,
`src/minitel_font_data.c`, generated by `tools/gen_minitel_font.py`, which
reuses this project's own 5x7 glyphs), the terminal view
(`src/minitel_view.[ch]`) and the harness (`tools/minitel_selftest.c`)
are original work for this project, written from

    Specifications Techniques d'Utilisation du Minitel 1B (STUM1B),
    France Telecom

No Minitel decoder, emulator or emitter source was read, copied or
adapted.

eMINITEL WAS ASSESSED FOR REUSE AND NOT REUSED. eMinitel by Dylan
"Asphox" Soton is MIT ("Copyright (c) 2024 SOTON "Asphox" Dylan") and
would have been licence-compatible. It was assessed from its README and
LICENSE only and declined on the evidence (a C++/SFML codebase in a C/GDI
Navigator, and two cursor-addressing defects in its core that could not be
fixed without reading and modifying it); the reasoning is in
`docs/2026-09-27_SUITE_MINITEL_MODE.md` section 1. No portion of it is in
Navigator, so its MIT notice is not required. If any eMinitel code is
ever ported in, the full MIT notice must then be added here and to
`licenses/`.

THREE PROGRAMS WERE USED AS BLACK-BOX ORACLES, run and compared, none
shipped: VDT2BMP by Jean-Francois Del Nero (release binary; no repository
licence file), eMinitel 0.2.b (release binary), and the Minitel web
emulator hosted by MiniPavi (in a browser; no licence file). None is in
this repository or the installer. The tab's CRT scan lines
(`minitel_present_bgra`) reproduce that emulator's Scanlines effect from
MEASUREMENTS of its rendered screen with the switch on and off; no part
of its source was read, so nothing of it is copied and no notice is
owed.

THE SPECIFICATION IS NOT REDISTRIBUTED. STUM1B is a France Telecom
document held as a reference copy outside this repository, beside its
volunteer transcription, and it must never be added to the installer.

"Minitel" and "Teletel" are the names of France Telecom's terminal and
videotex service, used here descriptively for the format the tab
decodes; they may be trademarks of their owner, and no endorsement is
implied.

## Emscripten runtime (the web Minitel terminal, web/minitel/)

- What ships, and where: NOT in the Navigator executable or the installer.
  `web/minitel/build.bat` compiles Navigator's own Minitel engine
  (`src/minitel_decode.c`, `src/minitel_screen.c`,
  `src/minitel_font_data.c`, `src/naplps_font_data.c`, unchanged) and the
  glue `web/minitel/engine/mt_web.c` with Emscripten 6.0.10 into
  `engine.js` and `engine.wasm`, which a web page serves. Those two files
  contain, besides Navigator's code, small parts of:
  - Emscripten's JavaScript runtime, C library support and allocator
    (emmalloc). Copyright (c) 2010-2014 Emscripten authors, see AUTHORS.
    License: MIT, or the University of Illinois/NCSA Open Source License,
    at the user's choice.
  - musl libc. Copyright (C) 2005-2020 Rich Felker, et al. License: MIT.
  - LLVM compiler-rt builtins. The LLVM Project. License: Apache License
    v2.0 with LLVM Exceptions (the exception waives the attribution
    requirement for compiled forms; it is credited anyway).
- License texts: `web/minitel/site/third-party-licenses.txt`, verbatim
  from the Emscripten 6.0.10 SDK (its LICENSE,
  system/lib/libc/musl/COPYRIGHT and system/lib/compiler-rt/LICENSE.TXT),
  copied into the built site and linked from the page's footer.
- Toolchain provenance: emsdk tag 6.0.10 (a2b92777), emscripten tag
  6.0.10 (d6c521a7), installed outside this repository; see
  `docs/2026-09-27_WEB_MINITEL_W1.md`.
- The page connects to the estate's own copy of the MiniPavi gateway by
  Jean-Arthur Silve (GPL), which runs on the server; nothing of it is in
  this repository or the page. The page's scan lines and key map are the
  the Navigator tab's own (see "Minitel mode of the Videotex tab" above).

## Microsoft GW-BASIC, and T. K. Chia's build of it (the GW-BASIC tab)

- What it is: the interpreter the **GW-BASIC** tab runs. It is Microsoft's
  1983 GW-BASIC source, released by Microsoft in 2020, plus the "OEM"
  modules that release lacked (keyboard, screen, sound, events, Ctrl-Break),
  written by Diomidis Spinellis, S. Gros and T. K. Chia. Navigator
  runs `GWBASICA.EXE`, the variant that adds `TIMER` and `MKDIR`/`CHDIR`/
  `RMDIR`.
- Upstream origin: https://codeberg.org/tkchia/GW-BASIC, commit
  `1432e385776554e43784b411eda7b8b2a9709e73` (2026-10-05). Every tracked
  file is vendored at `firmware/gwbasic/`, including upstream's own pinned
  JWasm and JWlink source tarballs, so it builds without the network.
- How it is built: `firmware/gwbasic/mgt_build.sh` runs upstream's own
  Makefile unmodified (OEMVER pinned to the commit date). The result is
  **byte-identical** to upstream release 20261005 (`GWBASICA.EXE` sha256
  `1a7b92b1aaf893e7f040190b8fa728e3c5b4dd82caaded8fefd0a471f669f7f7`), and
  the script checks this on every build.
- Licence: **MIT**. "Copyright (c) Microsoft Corporation", and the OEM
  modules "(C) Copyright D. Spinellis, S. Gros, T.K. Chia 2020--2022",
  under the same grant. The text is upstream's `LICENSE`, reproduced
  unmodified at `firmware/gwbasic/LICENSE`, and ships with Navigator at
  `LICENSES\LICENSE.gwbasic.txt`.
- How it ships: `GWBASICA.EXE` is embedded in `TeletextNavigator.exe`
  as a resource and executed by Navigator's own x86 machine (next entry);
  it is never written to disk or run by Windows. Nothing in it was
  modified.
- Used for: the GW-BASIC tab.

## libx86emu (x86 CPU emulation, compiled into Navigator)

- What it is: the x86 CPU that runs GW-BASIC inside Navigator. It is
  compiled and linked into `TeletextNavigator.exe`.
- Upstream origin: https://github.com/wfeldt/libx86emu, commit
  `ce81129c57fdfbf690bebc210a8db97d926cc5e7` (2025-02-24). Vendored at
  `third_party/libx86emu/` (`api.c`, `decode.c`, `mem.c`, `ops.c`,
  `ops2.c`, `prim_ops.c`, `include/`).
- Licence: an **X11/BSD-style permissive licence**. Copyright (c)
  1996-1999 SciTech Software, Inc.; Copyright (c) David Mosberger-Tang;
  Copyright (c) 1999 Egbert Eich; Copyright (c) 2007-2017 SUSE LINUX GmbH
  (author Steffen Winterfeldt). Upstream's `LICENSE` and `LICENSE_INFO`
  are kept unmodified in `third_party/libx86emu/`, and the licence ships
  at `LICENSES\LICENSE.libx86emu.txt`.
- What Navigator modified: one change at two sites, for Windows. The host
  port-I/O header and the IOPL probe fall back to upstream's own
  non-x86 definitions on `_WIN32`, because every guest port access goes
  through Navigator's memory/I-O callback and never reaches host hardware.
  Both sites are marked "MGT:" in the source.
- What is NOT libx86emu: the PC around the CPU (memory map, CGA, BIOS
  and DOS services, the MZ loader, timing) is `src/gwb_machine.[ch]`, and
  the tab is `src/gwbasic_module.[ch]`. Those are Navigator's own work and
  are AGPLv3 like the rest of it.

## Modern DOS (the GW-BASIC tab's screen font)

- What it is: "Modern DOS" 8x16 and 8x8 by **Jayvee Enaguas**
  (HarvettFox96), a raster font based on the IBM VGA and Verite ROM fonts.
  The tab draws text mode in the 8x16 face and characters in the CGA
  graphics modes in the 8x8 face.
- Origin: the original source repository (notabug.org/HarvettFox96/
  ttf-moderndos) is no longer online; the font was taken from its
  distribution archive (2019-01-04), which carries the licence file. The
  two TTFs used are kept at `third_party/moderndos/`.
- Licence: **Creative Commons Zero (CC0) 1.0 Universal**, public domain
  dedication. The archive's `LICENSE.txt` is kept at
  `third_party/moderndos/LICENSE.txt` and ships at
  `LICENSES\LICENSE.moderndos.txt`. CC0 asks for nothing; the font is
  credited anyway.
- How it ships: rasterised at build time by
  `tools/font_ttf_to_cp437.py` into the C tables `src/gwb_font8x16.h` and
  `src/gwb_font8x8.h` (all 256 CP437 code points), compiled into the exe.

## Ruffle (the Flash player of the Retro Web Browser tab)

- What it is: Ruffle, a Flash Player emulator written in Rust
  (https://ruffle.rs). Navigator uses it to play Flash movies (SWF)
  that a page shows with EMBED or OBJECT, inside the page, in the Retro
  Web Browser tab. It is built into `nav_ruffle.dll`, which ships beside
  `TeletextNavigator.exe` and is loaded at run time; it is not linked
  into the exe.
- Upstream origin: https://github.com/ruffle-rs/ruffle, tag
  `nightly-2026-11-03`, commit `489fc3a99aabb4a17e0dc1e0afc729f83b03cd8f`
  (version 0.8.0-nightly.2026.11.3). The crates the player uses are
  vendored **unmodified** at `third_party/ruffle/`; only the workspace
  `Cargo.toml` is changed (its member list, and a build profile that
  keeps panics catchable). `third_party/ruffle/README-NAVIGATOR.md`
  records exactly what was copied and changed.
- Licence: **MIT OR Apache-2.0**, at your option. Copyright (c)
  2018-2022 Ruffle LLC <ruffle@ruffle.rs> and Ruffle contributors.
  Upstream's `LICENSE.md` (both licences) is kept at
  `third_party/ruffle/LICENSE.md` and ships at
  `LICENSES\LICENSE.ruffle.txt`.
- What Navigator added: `third_party/ruffle/nav_ruffle/`, a small C
  interface to Ruffle (the `navrfl_*` functions), under the same MIT OR
  Apache-2.0 terms as Ruffle. Its sound output is adapted from Ruffle's
  `frontend-utils/src/backends/audio.rs`. The page side (finding the
  movie in EMBED/OBJECT, its box in the page, its window, its input) is
  `src/web_flash.[ch]` and `src/web_module.c`, Navigator's own work,
  AGPLv3 like the rest of it.
- What else is inside the DLL: 241 Rust crates in all (Ruffle's own and
  their dependencies, all vendored at `third_party/ruffle/vendor/`). All
  are under permissive licences (MIT, Apache-2.0, BSD, Zlib, ISC,
  Unlicense, Unicode-3.0) except **symphonia** (MP3 decoding), which is
  under the **Mozilla Public License 2.0**; its source is
  `third_party/ruffle/vendor/symphonia-*`. self_cell is used under its
  Apache-2.0 option. The VP6 video decoder is a part of **NihAV** by
  Kostya Shishkov, relicensed by its author under the MIT licence for
  Ruffle (`third_party/ruffle/licenses-extra/nihav-vp6-COPYING.txt`).
  Ruffle's fallback device font is a subset of **Noto Sans** and **Noto
  Sans Math** (Copyright 2022 The Noto Project Authors), under the **SIL
  Open Font License 1.1** (`third_party/ruffle/licenses-extra/`). Every
  crate, its licence and every licence text they carry are listed in
  `LICENSES\LICENSE.ruffle-crates.txt`, generated by
  `tools/make_ruffle_notices.py`.
- System libraries: the DLL uses Windows' own Direct3D 12 (dxgi, d3d12
  and the shader compiler), OpenGL (opengl32) and audio (WASAPI) as
  fallbacks and output; none of them is redistributed.
- Without the DLL (or with `NAV_FLASH=0` at build time) the tab behaves
  exactly as it did before Flash: pages show their own NOEMBED text.
