# build_manual.ps1 - Teletext Navigator User's Guide (PDF), headless.
#   help\src + manual\src + manual\figures.txt --gen_manual.py--> build\*.typ
#   manual.typ --Typst--> manual\Teletext Navigator Manual.pdf
# Fonts: ONLY the vendored Liberation fonts in manual\fonts (SIL OFL 1.1),
# embedded; system fonts are ignored, so every machine builds the same PDF.
# Typst is a build-time tool, not in the repository (typst.app, Apache-2.0;
# 0.15.1 used, from its GitHub release, sha256 19ce3551... of the zip).
# Screenshots: manual\shots, made by capture_shots.ps1 (kept in the repo).
#
# Part of Teletext Navigator. Copyright (C) 2026 Dimitri Papadopoulos and
# The Teletext. GNU Affero General Public License v3.0 or later.
param([string]$Typst = "D:\tools\typst-0.15.1\typst.exe")
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot; $root = Split-Path $here
& python "$here\gen_manual.py"
if ($LASTEXITCODE) { "GENERATOR FAILED"; exit 1 }
$pdf = Join-Path $here "Teletext Navigator Manual.pdf"
& $Typst compile --root $root --font-path "$here\fonts" --ignore-system-fonts "$here\manual.typ" $pdf
if ($LASTEXITCODE) { "TYPST FAILED"; exit 1 }
$f = Get-Item $pdf
"{0} {1:N0} B sha256 {2}" -f $f.Name, $f.Length, (Get-FileHash $pdf -Algorithm SHA256).Hash.ToLower().Substring(0, 16)
