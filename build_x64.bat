@REM build_x64.bat - Build Teletext Navigator for x64 Windows (MSYS2 UCRT64)
@REM Part of Teletext Navigator.
@REM
@REM Three compile groups per BLUEPRINT 7.3:
@REM   1. WAIS protocol files (src/wais/*.c + sockets_win.c, win_stubs.c):
@REM      preserved CNIDR code, needs -std=gnu89 -fcommon -fpermissive.
@REM   2. libwww 5.4.1 (src/libwww/*.c): preserved W3C source, needs
@REM      -std=gnu89 -fcommon -fpermissive + HAVE_DIRENT_H + NAME_MAX.
@REM      HT_EXPAT / HT_ZLIB / HT_MD5 / HT_POSIX_REGEX disabled in
@REM      src/libwww/windows/config.h.
@REM   3. Navigator shell and per-module files (-O2 -fstack-protector-strong).

@echo off
setlocal EnableDelayedExpansion

set CC=C:\msys64\ucrt64\bin\gcc.exe
set WINDRES=C:\msys64\ucrt64\bin\windres.exe

@REM Flash in the Retro Web Browser tab, docs\2026-11-05_RETROWEB_RUFFLE.md.
@REM NAV_FLASH=1, the default, compiles src\web_flash.c and the libwww
@REM EMBED and NOEMBED DTD entries. The player itself, nav_ruffle.dll, is
@REM built separately by third_party\ruffle\build_nav_ruffle.bat and is
@REM loaded at run time, so this script needs no Rust. THE NO-GO SWITCH:
@REM "set NAV_FLASH=0" before running this script builds exactly the
@REM pre-Flash exe, proven to differ only in the PE timestamp and checksum.
if not defined NAV_FLASH set NAV_FLASH=1
if not "%NAV_FLASH%"=="0" if not "%NAV_FLASH%"=="1" (
    echo NAV_FLASH must be 0 or 1, not "%NAV_FLASH%"
    exit /b 2
)
echo NAV_FLASH=%NAV_FLASH%

set CFLAGS_WAIS=-std=gnu89 -fcommon -fpermissive -O2 -fstack-protector-strong -D_WIN32 -DTELL_USER -DTCPIP -Isrc -Isrc\wais
set CFLAGS_LIBWWW=-std=gnu89 -fcommon -fpermissive -O2 ^
    -D_WINDOWS -DWIN32 -D_WIN32 ^
    -DHAVE_DIRENT_H -DNAME_MAX=255 ^
    -Isrc\libwww -DNAV_FLASH=%NAV_FLASH% ^
    -Wno-implicit-function-declaration -Wno-implicit-int ^
    -Wno-int-conversion -Wno-incompatible-pointer-types ^
    -Wno-discarded-qualifiers -Wno-pointer-sign ^
    -Wno-format -Wno-unused-result
set CFLAGS_SUITE=-O2 -fstack-protector-strong -D_WIN32 -Isrc -Isrc\wais
@REM web_module.c drives libwww directly; needs the libwww header path
@REM and _WINDOWS so wwwsys.h's WWW_MSWINDOWS / WWW_WIN_WINDOW branches
@REM activate (NO_STDIO gets defined for free, suppressing libwww's
@REM stdio output in the GUI Navigator binary).
set CFLAGS_WEB=-O2 -fstack-protector-strong -D_WIN32 -D_WINDOWS ^
    -Isrc -Isrc\wais -Isrc\libwww -DNAV_FLASH=%NAV_FLASH%
@REM 2026-06-15 Newspaper dispatch: WebView2 SDK headers are MSVC-style
@REM EXTERN_C __declspec(selectany) const IID declarations and GCC
@REM warns ~295 times per TU that includes WebView2.h. The host
@REM service is the only TU that does so; CFLAGS_WV2 carries the
@REM include path. WinHTTP for the newspaper_service index fetch.
set CFLAGS_WV2=-O2 -fstack-protector-strong -D_WIN32 -D_WINDOWS ^
    -Isrc -Ithird_party\webview2\build\native\include
set CFLAGS_NEWS=-O2 -fstack-protector-strong -D_WIN32 -Isrc
@REM 2026-08-02 Phase 4 Zmodem receive: the vendored mbzm core lives in
@REM third_party\mbzm and is reached only by src\zmodem_recv.c, so the
@REM include path is confined to those translation units. mbzm is MIT
@REM (Ross Bamford); its two Synchronet GPL CRC files were removed and
@REM replaced with public-domain ones, see third_party\mbzm\MGT-CHANGES.md.
set CFLAGS_ZMODEM=-O2 -fstack-protector-strong -D_WIN32 -Isrc ^
    -Ithird_party\mbzm\include
@REM 2026-08-05 Native NABU, phase 1: the vendored Marduk emulation core
@REM lives in third_party\marduk and is reached only by src\nabu_core.c,
@REM so the include path is confined to those translation units. Marduk
@REM and the three chip cores it carries (z80, TMS9918, AY-3-8910) are
@REM all MIT; see third_party\marduk\license.txt.
@REM
@REM NOTHING CALLS THIS YET. The machine is compiled into Navigator from
@REM this phase on so it cannot rot, but the NABU tab in the shipping
@REM build is still the external-player one. See
@REM docs\2026-08-05_NABU_NATIVE_PHASE1.md.
set CFLAGS_NABU=-O2 -fstack-protector-strong -D_WIN32 -Isrc ^
    -Ithird_party\marduk
@REM 2026-10-20 GW-BASIC tab: libx86emu (third_party\libx86emu, X11/BSD
@REM style licence) runs Microsoft's GWBASICA.EXE, which is embedded as
@REM RCDATA from firmware\gwbasic. The CPU's include path is confined to
@REM the machine and the core, as Marduk's is.
set CFLAGS_GWB=-O2 -fstack-protector-strong -D_WIN32 -Isrc ^
    -Ithird_party\libx86emu\include
@REM Link: Windows in-box libraries only. -lbcrypt is the CNG SHA-256 that
@REM verifies a downloaded installer (src\suite_sha256.c); -lwinhttp serves
@REM the updater and the HTTPS fetches; -lshell32 opens outbound links in
@REM the system browser. No vendored crypto.
set LDFLAGS=-s -Wl,--no-insert-timestamp -lbcrypt -lws2_32 -lcomctl32 -lcomdlg32 -lgdi32 -lgdiplus -lmsimg32 -lole32 -loleaut32 -luuid -lwinmm -lwinhttp -lshell32 -static -mwindows -Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -lssp

set TARGET=TeletextNavigator.exe

echo Compiling WAIS protocol files (CNIDR preserved, x64 / GCC 15 / UCRT64)...
for %%f in (zutil zprot wprot wutil wmessage transprt docid list ztype1 irfileio cutil panic ustubs ui_win) do (
    echo   wais\%%f.c
    %CC% %CFLAGS_WAIS% -c -o src\wais\%%f.o src\wais\%%f.c
    if errorlevel 1 exit /b 1
)

echo Compiling libwww 5.4.1 (W3C source preserved, Win32 + GCC 15 / UCRT64)...
set FILES_LWWW_UTILS=HTArray HTAssoc HTAtom HTChunk HTHash HTList HTMemory HTString HTTrace HTUU md5
set FILES_LWWW_CORE=HTAlert HTAnchor HTChannl HTDNS HTError HTEscape HTEvent HTFormat HTHost HTInet HTLib HTLink HTMemLog HTMethod HTNet HTNoFree HTParse HTProt HTReqMan HTResponse HTStream HTTCP HTTimer HTTrans HTUTree HTUser HTWWWStr
set FILES_LWWW_HTTP=HTAABrow HTAAUtil HTCookie HTDigest HTTChunk HTTP HTTPGen HTTPReq HTTPRes HTTPServ HTPEP
set FILES_LWWW_HTML=HTMLPDTD SGML HTMLGen HTTeXGen HTPlain HTML HText HTHInit HTStyle
set FILES_LWWW_INIT=HTInit HTProfil
set FILES_LWWW_MIME=HTBound HTHeader HTMIME HTMIMPrs HTMIMERq HTMIMImp
set FILES_LWWW_STREAM=HTConLen HTEPtoCl HTFSave HTFWrite HTGuess HTMerge HTNetTxt HTSChunk HTTee HTXParse
set FILES_LWWW_TRANS=HTANSI HTBufWrt HTLocal HTReader HTSocket HTWriter
set FILES_LWWW_APP=HTAccess HTDialog HTEvtLst HTFilter HTHist HTHome HTLog HTProxy HTRules
set FILES_LWWW_FILE=HTBInit HTBind HTFile HTMulti
set FILES_LWWW_FTP=HTFTP HTFTPDir
set FILES_LWWW_GOPHER=HTGopher
set FILES_LWWW_NEWS=HTNDir HTNews HTNewsLs HTNewsRq
set FILES_LWWW_TELNET=HTTelnet
set FILES_LWWW_CACHE=HTCache
set FILES_LWWW_DIR=HTIcons HTDescpt HTDir

@REM Build a space-separated list of all libwww .o paths for the link
@REM step below. Compile each .c -> .o, append the .o path to LWWW_OBJS.
set LWWW_OBJS=
for %%g in (UTILS CORE HTTP HTML INIT MIME STREAM TRANS APP FILE FTP GOPHER NEWS TELNET CACHE DIR) do (
    for %%f in (!FILES_LWWW_%%g!) do (
        echo   libwww\%%f.c
        %CC% %CFLAGS_LIBWWW% -c -o src\libwww\%%f.o src\libwww\%%f.c
        if errorlevel 1 exit /b 1
        set LWWW_OBJS=!LWWW_OBJS! src\libwww\%%f.o
    )
)

echo Compiling Navigator Win32 glue (sockets_win, win_stubs)...
for %%f in (sockets_win win_stubs) do (
    echo   %%f.c
    %CC% %CFLAGS_WAIS% -c -o src\%%f.o src\%%f.c
    if errorlevel 1 exit /b 1
)

echo Compiling Navigator shell and modules...
for %%f in (suite_shell suite_fonts suite_branding suite_clipboard suite_logo http_fetch suite_sha256 suite_update audio_format_detect wav_parser tsp_metafile truespeech_decoder wais_module arpamail_module archie_ardp archie_module ftp_fetch vt_term telnet_proto finger_proto qotd_proto telnet_module irc_client irc_module rip_ega rip_parser rip_text rip_button rip_hershey_data rip_font8x8_data ripscrip_module naplps_surface naplps_decode naplps_font_data naplps_module minitel_decode minitel_screen minitel_font_data minitel_view cuseeme_proto cuseeme_video cuseeme_deltamod cuseeme_mulaw cuseeme_idvi cuseeme_module audio_meter_service) do (
    echo   %%f.c
    %CC% %CFLAGS_SUITE% -c -o src\%%f.o src\%%f.c
    if errorlevel 1 exit /b 1
)
echo Compiling the vendored Zmodem receive core (mbzm, MIT)...
for %%f in (zserial zheaders znumbers crc16 crc32) do (
    echo   third_party\mbzm\%%f.c
    %CC% %CFLAGS_ZMODEM% -c -o third_party\mbzm\%%f.o third_party\mbzm\%%f.c
    if errorlevel 1 exit /b 1
)

echo   zmodem_recv.c (Navigator Zmodem receive session, uses CFLAGS_ZMODEM)
%CC% %CFLAGS_ZMODEM% -c -o src\zmodem_recv.o src\zmodem_recv.c
if errorlevel 1 exit /b 1

echo Compiling the vendored NABU emulation core (Marduk, MIT)...
for %%f in (z80 tms9918 tms_util emu2149 disk modem) do (
    echo   third_party\marduk\%%f.c
    %CC% %CFLAGS_NABU% -c -o third_party\marduk\%%f.o third_party\marduk\%%f.c
    if errorlevel 1 exit /b 1
)

echo   nabu_core.c (the NABU machine, uses CFLAGS_NABU)
%CC% %CFLAGS_NABU% -c -o src\nabu_core.o src\nabu_core.c
if errorlevel 1 exit /b 1

echo   nabu_channel.c (the adapter CHANGE_CHANNEL exchange, shared)
%CC% %CFLAGS_NABU% -c -o src\nabu_channel.o src\nabu_channel.c
if errorlevel 1 exit /b 1

echo   nabu_serial.c (the real-NABU serial bridge)
%CC% %CFLAGS_NABU% -c -o src\nabu_serial.o src\nabu_serial.c
if errorlevel 1 exit /b 1

echo   nabu_native_module.c (the NABU Native tab's GDI renderer, uses CFLAGS_NABU)
%CC% %CFLAGS_NABU% -c -o src\nabu_native_module.o src\nabu_native_module.c
if errorlevel 1 exit /b 1

echo Compiling the vendored x86 core (libx86emu) for the GW-BASIC tab...
for %%f in (api decode mem ops ops2 prim_ops) do (
    echo   third_party\libx86emu\%%f.c
    %CC% %CFLAGS_GWB% -c -o third_party\libx86emu\%%f.o third_party\libx86emu\%%f.c
    if errorlevel 1 exit /b 1
)

echo   gwb_machine.c (the PC that runs GW-BASIC, uses CFLAGS_GWB)
%CC% %CFLAGS_GWB% -c -o src\gwb_machine.o src\gwb_machine.c
if errorlevel 1 exit /b 1

echo   gwbasic_module.c (the GW-BASIC tab, uses CFLAGS_GWB)
%CC% %CFLAGS_GWB% -c -o src\gwbasic_module.o src\gwbasic_module.c
if errorlevel 1 exit /b 1

echo   web_module.c (libwww-driven, uses CFLAGS_WEB)
%CC% %CFLAGS_WEB% -c -o src\web_module.o src\web_module.c
if errorlevel 1 exit /b 1

set FLASH_OBJS=
if "%NAV_FLASH%"=="1" (
    echo   web_flash.c - Flash in the Retro Web tab, uses CFLAGS_WEB
    %CC% %CFLAGS_WEB% -c -o src\web_flash.o src\web_flash.c
    if errorlevel 1 exit /b 1
    set FLASH_OBJS=src\web_flash.o
)

echo   web_module_cdf.c (Active Channel parser + dialog, libwww-driven, uses CFLAGS_WEB)
%CC% %CFLAGS_WEB% -c -o src\web_module_cdf.o src\web_module_cdf.c
if errorlevel 1 exit /b 1

echo   audio_service.c (shared Sun .au + MP3 player, libwww + winmm + minimp3, uses CFLAGS_WEB)
%CC% %CFLAGS_WEB% -c -o src\audio_service.o src\audio_service.c
if errorlevel 1 exit /b 1

echo   gopher_protocol.c (plain RFC 1436 over TCP, uses CFLAGS_SUITE)
%CC% %CFLAGS_SUITE% -c -o src\gopher_protocol.o src\gopher_protocol.c
if errorlevel 1 exit /b 1

echo   gophermap.c (gophermap line parser, uses CFLAGS_SUITE)
%CC% %CFLAGS_SUITE% -c -o src\gophermap.o src\gophermap.c
if errorlevel 1 exit /b 1

echo   gopher_module.c (F3 Gopher client UI, uses CFLAGS_SUITE)
%CC% %CFLAGS_SUITE% -c -o src\gopher_module.o src\gopher_module.c
if errorlevel 1 exit /b 1

echo   gopher_search_dialog.c (type 7 modal, in-memory DLGTEMPLATE)
%CC% %CFLAGS_SUITE% -c -o src\gopher_search_dialog.o src\gopher_search_dialog.c
if errorlevel 1 exit /b 1

echo   suite.rc (icon + embedded fonts + version resource)
%WINDRES% -i src\suite.rc -O coff -o src\suite_res.o
if errorlevel 1 exit /b 1

echo Linking %TARGET%...
%CC% -o %TARGET% ^
    src\wais\zutil.o src\wais\zprot.o src\wais\wprot.o src\wais\wutil.o ^
    src\wais\wmessage.o src\wais\transprt.o src\wais\docid.o src\wais\list.o ^
    src\wais\ztype1.o src\wais\irfileio.o src\wais\cutil.o src\wais\panic.o ^
    src\wais\ustubs.o src\wais\ui_win.o ^
    %LWWW_OBJS% ^
    src\sockets_win.o src\win_stubs.o ^
    src\suite_shell.o src\suite_fonts.o src\suite_branding.o ^
    src\suite_clipboard.o src\suite_logo.o ^
    src\http_fetch.o src\suite_sha256.o src\suite_update.o ^
    src\wais_module.o src\arpamail_module.o src\web_module.o ^
    src\archie_ardp.o src\archie_module.o src\ftp_fetch.o ^
    src\web_module_cdf.o %FLASH_OBJS% ^
    src\audio_service.o src\audio_format_detect.o ^
    src\wav_parser.o src\tsp_metafile.o src\truespeech_decoder.o ^
    src\gopher_protocol.o src\gophermap.o src\gopher_module.o ^
    src\gopher_search_dialog.o ^
    src\vt_term.o src\telnet_proto.o src\finger_proto.o src\qotd_proto.o ^
    src\telnet_module.o ^
    src\irc_client.o src\irc_module.o ^
    src\rip_ega.o src\rip_parser.o src\rip_text.o src\rip_button.o ^
    src\rip_hershey_data.o src\rip_font8x8_data.o ^
    src\ripscrip_module.o ^
    src\naplps_surface.o src\naplps_decode.o ^
    src\naplps_font_data.o src\naplps_module.o ^
    src\minitel_decode.o src\minitel_screen.o ^
    src\minitel_font_data.o src\minitel_view.o ^
    src\nabu_core.o src\nabu_native_module.o ^
    src\nabu_channel.o src\nabu_serial.o ^
    third_party\marduk\z80.o third_party\marduk\tms9918.o ^
    third_party\marduk\tms_util.o third_party\marduk\emu2149.o ^
    third_party\marduk\disk.o third_party\marduk\modem.o ^
    src\gwb_machine.o src\gwbasic_module.o ^
    third_party\libx86emu\api.o third_party\libx86emu\decode.o ^
    third_party\libx86emu\mem.o third_party\libx86emu\ops.o ^
    third_party\libx86emu\ops2.o third_party\libx86emu\prim_ops.o ^
    src\zmodem_recv.o ^
    third_party\mbzm\zserial.o third_party\mbzm\zheaders.o ^
    third_party\mbzm\znumbers.o ^
    third_party\mbzm\crc16.o third_party\mbzm\crc32.o ^
    src\cuseeme_proto.o src\cuseeme_video.o src\cuseeme_deltamod.o ^
    src\cuseeme_mulaw.o src\cuseeme_idvi.o src\cuseeme_module.o ^
    src\audio_meter_service.o ^
    src\suite_res.o ^
    %LDFLAGS%
if errorlevel 1 exit /b 1

echo BUILD SUCCESSFUL: %TARGET%
endlocal
