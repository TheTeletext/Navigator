@REM build_nav_ruffle.bat - build nav_ruffle.dll, the Flash player of the
@REM Retro Web Browser tab (Ruffle, MIT OR Apache-2.0), and copy it beside
@REM TeletextNavigator.exe at the repository root.
@REM Part of Teletext Navigator. See README-NAVIGATOR.md in this folder.
@REM
@REM Needs MSYS2 UCRT64 Rust (pacman -S mingw-w64-ucrt-x86_64-rust). Builds
@REM OFFLINE from vendor\ and Cargo.lock; nothing is downloaded. The source
@REM path is remapped so the DLL does not depend on where the tree sits.

@echo off
setlocal
set PATH=C:\msys64\ucrt64\bin;%PATH%
set HERE=%~dp0
cd /d "%HERE%"
set CARGO_ENCODED_RUSTFLAGS=--remap-path-prefix=%HERE%=ruffle/
cargo build --profile navigator --locked --offline -p nav_ruffle
if errorlevel 1 exit /b 1
if "%~1"=="--no-copy" goto done
copy /y "%HERE%target\navigator\nav_ruffle.dll" "%HERE%..\..\nav_ruffle.dll" >nul
if errorlevel 1 exit /b 1
echo copied nav_ruffle.dll beside TeletextNavigator.exe
:done
endlocal
