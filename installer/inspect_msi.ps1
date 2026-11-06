# inspect_msi.ps1 - list what is actually inside a built MSI's File table.
# Written because a 109 MB payload produced a 16 MB package and that
# needed proving rather than assuming.
param([string]$Msi)
$ErrorActionPreference = 'Stop'
$inst = New-Object -ComObject WindowsInstaller.Installer
$db = $inst.GetType().InvokeMember("OpenDatabase", "InvokeMethod", $null, $inst, @($Msi, 0))
$sql = "SELECT " + [char]96 + "FileName" + [char]96 + ", " + [char]96 + "FileSize" + [char]96 +
       " FROM " + [char]96 + "File" + [char]96
$v = $db.GetType().InvokeMember("OpenView", "InvokeMethod", $null, $db, @($sql))
$v.GetType().InvokeMember("Execute", "InvokeMethod", $null, $v, $null) | Out-Null
$n = 0; $total = 0; $big = @()
while ($true) {
    $r = $v.GetType().InvokeMember("Fetch", "InvokeMethod", $null, $v, $null)
    if ($null -eq $r) { break }
    $fn = $r.GetType().InvokeMember("StringData", "GetProperty", $null, $r, @(1))
    $sz = $r.GetType().InvokeMember("IntegerData", "GetProperty", $null, $r, @(2))
    $n++; $total += $sz
    if ($sz -gt 1000000) { $big += ("  {0,-40} {1,15:N0}" -f $fn, $sz) }
}
Write-Host ("files in the MSI          : {0}" -f $n)
Write-Host ("total uncompressed bytes  : {0:N0}" -f $total)
Write-Host ("package bytes on disk     : {0:N0}" -f (Get-Item $Msi).Length)
Write-Host "files over 1 MB:"
$big
