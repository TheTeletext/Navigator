@REM build_navigator_msi.bat - Build Teletext_Navigator_<version>.msi with WiX.
@REM
@REM Part of Teletext Navigator.
@REM
@REM The exe is TeletextNavigator.exe, the WiX sources are
@REM TeletextNavigator.wxs/.wxl, the icon comes from assets\brand\, the
@REM wizard art from tools\gen_navigator_art.py, and the payload is staged
@REM in payload-navigator\.
@REM Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
@REM Licensed under the GNU Affero General Public License v3.0 or later.
@REM
@REM PREREQUISITES, pinned. Install once:
@REM     dotnet tool install --global wix --version 5.0.2
@REM     wix extension add --global WixToolset.UI.wixext/5.0.2
@REM     wix extension add --global WixToolset.Util.wixext/5.0.2
@REM Needs a .NET SDK 6.0 or later; 8.0.423 is what this was built with.
@REM
@REM Run build_x64.bat FIRST. This script packages the exe that is there;
@REM it does not compile it.
@REM
@REM THE VERSION IS NOT TYPED ANYWHERE IN THIS FILE OR IN THE .wxs. It is
@REM read back out of src\suite_version.h through the same C preprocessor
@REM that compiles the app, so the MSI ProductVersion, the app's About
@REM box and the resource VERSIONINFO cannot disagree.

@echo off
setlocal EnableDelayedExpansion

set HERE=%~dp0
set ROOT=%HERE%..
set CC=C:\msys64\ucrt64\bin\gcc.exe
set WIX=wix
set PAYLOAD=%HERE%payload-navigator
set OUTDIR=%HERE%out

if not exist "%ROOT%\TeletextNavigator.exe" (
    echo ERROR: TeletextNavigator.exe not found. Run build_x64.bat first.
    exit /b 1
)

@REM Flash in the Retro Web Browser tab, docs\2026-11-05_RETROWEB_RUFFLE.md.
@REM NAV_FLASH=1, the default, ships nav_ruffle.dll (build it first with
@REM third_party\ruffle\build_nav_ruffle.bat) and its licence notices.
@REM NAV_FLASH=0 leaves them out: the no-go switch. Use the same value as
@REM for build_x64.bat.
if not defined NAV_FLASH set NAV_FLASH=1
if not "%NAV_FLASH%"=="0" if not "%NAV_FLASH%"=="1" (
    echo ERROR: NAV_FLASH must be 0 or 1, not "%NAV_FLASH%"
    exit /b 2
)
if "%NAV_FLASH%"=="1" if not exist "%ROOT%\nav_ruffle.dll" (
    echo ERROR: nav_ruffle.dll not found. Run third_party\ruffle\build_nav_ruffle.bat first,
    echo        or set NAV_FLASH=0 to build without Flash.
    exit /b 1
)
echo NAV_FLASH=%NAV_FLASH%

@REM ------------------------------------------------------------------
@REM Version, straight out of the header.
@REM
@REM Asking the preprocessor rather than parsing the header by hand means
@REM there is no second parser to get out of step. The echo below expands
@REM to a line like:  MGTVER 1 2 0 "-rc.1"
@REM ------------------------------------------------------------------
set ARTDIR=%HERE%art-navigator
set ICONDIR=%ROOT%\assets\brand

set VERFILE=%TEMP%\tn_ver_%RANDOM%.txt
echo MGTVER SUITE_VERSION_MAJOR SUITE_VERSION_MINOR SUITE_VERSION_PATCH SUITE_VERSION_SUFFIX ^
    | %CC% -E -P -include "%ROOT%\src\suite_version.h" -x c - > "%VERFILE%" 2>nul
if errorlevel 1 (
    echo ERROR: could not preprocess src\suite_version.h
    del "%VERFILE%" 2>nul
    exit /b 1
)

set VMAJ=
for /f "tokens=1-5" %%a in ('findstr /b MGTVER "%VERFILE%"') do (
    set VMAJ=%%b
    set VMIN=%%c
    set VPAT=%%d
    set VSUF=%%e
)
del "%VERFILE%" 2>nul

if "%VMAJ%"=="" (
    echo ERROR: could not read the version out of src\suite_version.h
    exit /b 1
)
@REM VSUF arrives quoted, as a string literal such as "-rc.1". Strip the quotes.
set VSUF=%VSUF:"=%

set MSIVERSION=%VMAJ%.%VMIN%.%VPAT%
set FULLVERSION=%MSIVERSION%%VSUF%

@REM The two display names, from the same header, for the same reason.
@REM Each expands to one quoted string literal, so tokens=1,* splits the
@REM marker off and leaves the whole quoted name in %%b.
set NAMEFILE=%TEMP%\tn_name_%RANDOM%.txt
echo MGTLONG SUITE_APP_NAME_LONG ^
    | %CC% -E -P -include "%ROOT%\src\suite_version.h" -x c - > "%NAMEFILE%" 2>nul
echo MGTSHORT SUITE_APP_NAME_SHORT ^
    | %CC% -E -P -include "%ROOT%\src\suite_version.h" -x c - >> "%NAMEFILE%" 2>nul
for /f "tokens=1,* delims= " %%a in ('findstr /b MGTLONG "%NAMEFILE%"')  do set LONGNAME=%%b
for /f "tokens=1,* delims= " %%a in ('findstr /b MGTSHORT "%NAMEFILE%"') do set SHORTNAME=%%b
del "%NAMEFILE%" 2>nul
set LONGNAME=%LONGNAME:"=%
set SHORTNAME=%SHORTNAME:"=%
@REM The product name carries the title form of the version, as the title bar does.
set VTITLE=%VMAJ%.%VMIN%
if not "%VPAT%"=="0" set VTITLE=%VMAJ%.%VMIN%.%VPAT%
if "%LONGNAME%"=="" (
    echo ERROR: could not read the display names out of src\suite_version.h
    exit /b 1
)

echo Identity from src\suite_version.h:
echo    MSI ProductVersion : %MSIVERSION%
echo    canonical string   : %FULLVERSION%
echo    long name  (wizard): %LONGNAME%
echo    short name (ARP,   : %SHORTNAME%
echo                shortcuts^)
echo.

@REM ------------------------------------------------------------------
@REM Stage the payload.
@REM
@REM Everything here is DERIVED from a canonical file in the repository,
@REM never hand-maintained, so the installed documentation cannot drift
@REM from the repository's. payload\ is generated and gitignored.
@REM ------------------------------------------------------------------
echo Staging payload...
if exist "%PAYLOAD%" rmdir /s /q "%PAYLOAD%"
mkdir "%PAYLOAD%"
mkdir "%PAYLOAD%\LICENSES"

copy /y "%ROOT%\TeletextNavigator.exe"     "%PAYLOAD%\" >nul || exit /b 1
copy /y "%ROOT%\README.TXT"                "%PAYLOAD%\README.TXT"  >nul || exit /b 1
copy /y "%ROOT%\COPYING"                   "%PAYLOAD%\LICENSE.TXT" >nul || exit /b 1
copy /y "%ROOT%\THIRD-PARTY-NOTICES.md"    "%PAYLOAD%\NOTICES.TXT" >nul || exit /b 1
copy /y "%ROOT%\licenses\*.txt"            "%PAYLOAD%\LICENSES\"   >nul || exit /b 1
if "%NAV_FLASH%"=="1" copy /y "%ROOT%\nav_ruffle.dll" "%PAYLOAD%\" >nul || exit /b 1

@REM Help (Navigator 1.6, docs/2026-11-13_WinHelp_Revival_Phase2b.md): The
@REM Teletext WinHelp viewer (LGPL 2.1+, its own repository beside this one,
@REM github.com/TheTeletext/WinHelp), its licence text BESIDE it (LGPL
@REM obligation), its How to Use Help, and TELETEXT.HLP (compiled, kept in
@REM help\). The viewer must be built from a pushed, tagged commit.
if not defined WINHELP_DIR set "WINHELP_DIR=%ROOT%\..\mgt-winhelp"
copy /y "%WINHELP_DIR%\build\TTWINHLP.EXE" "%PAYLOAD%\TTWINHLP.EXE" >nul || exit /b 1
copy /y "%WINHELP_DIR%\COPYING.LIB"        "%PAYLOAD%\COPYING.LIB"  >nul || exit /b 1
copy /y "%WINHELP_DIR%\help\TTWINHLP.HLP"  "%PAYLOAD%\TTWINHLP.HLP" >nul || exit /b 1
copy /y "%ROOT%\help\TELETEXT.HLP"         "%PAYLOAD%\TELETEXT.HLP" >nul || exit /b 1
@REM The printed manual (manual\build_manual.ps1; kept in the repository).
copy /y "%ROOT%\manual\Teletext Navigator Manual.pdf" "%PAYLOAD%\Teletext Navigator Manual.pdf" >nul || exit /b 1

@REM ------------------------------------------------------------------
@REM The NABU folder: boot firmware ONLY. The NABU tab runs the Marduk
@REM core inside the program and reads nothing in NABU\ but the two
@REM firmware images staged below (src\nabu_native_module.c).
@REM ------------------------------------------------------------------
mkdir "%PAYLOAD%\NABU"

@REM ------------------------------------------------------------------
@REM The NABU tab's boot firmware.
@REM
@REM These two images go in the ROOT of NABU\, not under roms\, because
@REM that is where src\nabu_native_module.c looks for them. Its candidate
@REM list is relative to the folder holding the exe and reads, in order:
@REM
@REM     NABU\mgtipl.bin                    <- installed, preferred
@REM     firmware\mgtipl\mgtipl.bin         <- source tree
@REM     NABU\opennabu.bin                  <- installed, fallback
@REM     third_party\opennabu\opennabu.bin  <- source tree
@REM
@REM The checks below stop the build if either is missing, because both
@REM files are produced elsewhere (mgtipl.bin is assembled on the Unicorn
@REM server) and a missing one would otherwise be silent until someone
@REM pressed Connect on an installed copy.
@REM
@REM No wxs change is needed: the NabuFiles group harvests
@REM $(PayloadDir)\NABU\** wholesale.
@REM
@REM Both are MIT, by S. V. Nickolas. See THIRD-PARTY-NOTICES.md and
@REM LICENSES\LICENSE.opennabu.txt, which ships beside them.
@REM ------------------------------------------------------------------
echo Staging the NABU boot firmware...
if not exist "%ROOT%\firmware\mgtipl\mgtipl.bin" (
    echo ERROR: MGT IPL is missing at firmware\mgtipl\mgtipl.bin
    echo        Assemble it per docs\2026-08-07_NABU_MGT_IPL_FAST_BOOT.md
    exit /b 1
)
if not exist "%ROOT%\third_party\opennabu\opennabu.bin" (
    echo ERROR: the stock OpenNabu fallback is missing at
    echo        third_party\opennabu\opennabu.bin
    exit /b 1
)
copy /y "%ROOT%\firmware\mgtipl\mgtipl.bin"        "%PAYLOAD%\NABU\mgtipl.bin"   >nul || exit /b 1
copy /y "%ROOT%\third_party\opennabu\opennabu.bin" "%PAYLOAD%\NABU\opennabu.bin" >nul || exit /b 1
@REM The NABU folder holds the two firmware images and nothing else.
for %%f in ("%PAYLOAD%\NABU\*") do (
    if /i not "%%~nxf"=="mgtipl.bin" if /i not "%%~nxf"=="opennabu.bin" (
        echo ERROR: unexpected file in the staged NABU folder: %%~nxf
        exit /b 1
    )
)

@REM CREDITS.TXT is generated from CREDITS.md so the two cannot drift.
python "%ROOT%\make_credits_txt.py" "%ROOT%\CREDITS.md" "%PAYLOAD%\CREDITS.TXT"
if errorlevel 1 (
    echo ERROR: make_credits_txt.py failed
    exit /b 1
)

@REM The wizard artwork is generated, not stored: installer\art is
@REM rebuilt from the logo and the icon sources every time so the
@REM installer's face cannot fall behind the brand assets.
@REM Navigator: the Teletext art. The icon set is NOT regenerated here; it
@REM is a checked-in output of assets\brand\make_icon.sh, which needs a
@REM POSIX shell and was reviewed by eye at 16, 32 and 256 px.
python "%HERE%..\tools\gen_navigator_art.py"
if errorlevel 1 ( echo ERROR: gen_navigator_art.py failed & exit /b 1 )

@REM The WiX licence dialog wants RTF. Wrap COPYING rather than keeping a
@REM second copy of the AGPL that could fall behind it.
python "%HERE%make_license_rtf.py" "%ROOT%\COPYING" "%PAYLOAD%\LICENSE.rtf"
if errorlevel 1 (
    echo ERROR: make_license_rtf.py failed
    exit /b 1
)

@REM ------------------------------------------------------------------
@REM Build.
@REM ------------------------------------------------------------------
if not exist "%OUTDIR%" mkdir "%OUTDIR%"
set MSINAME=Teletext_Navigator_%MSIVERSION%.msi

echo Building %MSINAME% ...
%WIX% build "%HERE%TeletextNavigator.wxs" ^
    -arch x64 ^
    -ext WixToolset.UI.wixext ^
    -ext WixToolset.Util.wixext ^
    -loc "%HERE%TeletextNavigator.wxl" ^
    -d Version=%MSIVERSION% ^
    -d FullVersion=%FULLVERSION% ^
    -d LongName="%LONGNAME% %VTITLE%" ^
    -d ShortName="%SHORTNAME% %VTITLE%" ^
    -d PayloadDir=%PAYLOAD% ^
    -d ArtDir=%ARTDIR% ^
    -d IconDir=%ICONDIR% ^
    -d NavFlash=%NAV_FLASH% ^
    -o "%OUTDIR%\%MSINAME%"
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

echo.
echo BUILD SUCCESSFUL: %OUTDIR%\%MSINAME%
powershell -NoProfile -Command "$h=(Get-FileHash '%OUTDIR%\%MSINAME%' -Algorithm SHA256).Hash.ToLower(); $s=(Get-Item '%OUTDIR%\%MSINAME%').Length; Write-Host ''; Write-Host ('  bytes  : ' + $s); Write-Host ('  sha256 : ' + $h); $h | Set-Content -NoNewline '%OUTDIR%\%MSINAME%.sha256'"
echo.
echo   sha256 also written to %OUTDIR%\%MSINAME%.sha256
endlocal
