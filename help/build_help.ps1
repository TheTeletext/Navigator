# build_help.ps1 - build TELETEXT.HLP (Teletext Navigator Help) from help\src\*.txt.
# The source is plain text for editors (markup: see hlpsrc.py). The build
# runs the viewer repository's tools (github.com/TheTeletext/WinHelp, checked
# out beside this one): hlpsrc.py -> RTF + HPJ -> Microsoft Help Compiler
# 3.10.505, headless. The compiler is NOT in either repository (Microsoft
# licence); HC.EXE and HC.ERR must be in $HcDir. The compiled TELETEXT.HLP
# is kept in this repository, so building Navigator or its MSI never needs
# the compiler.
param([string]$HcDir = "D:\winhelp_build", [string]$Viewer = "$PSScriptRoot\..\..\mgt-winhelp")
$srcs = @(Get-ChildItem "$PSScriptRoot\src\*.txt" | Sort-Object Name | ForEach-Object { $_.FullName })
& "$Viewer\tools\build_hlp.ps1" -Base TELETEXT -Src $srcs -OutDir $PSScriptRoot -HcDir $HcDir
exit $LASTEXITCODE
