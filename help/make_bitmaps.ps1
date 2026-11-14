# make_bitmaps.ps1 - the Help's pictures, made from Navigator's own artwork,
# so each service's Help shows the icon of its ribbon button (as the
# Chameleon help files show their services' icons):
#   assets\w31icons\<service>.bmp  -> help\src\<NAME>.BMP  (32 x 32)
#   assets\brand\teletext_navigator.ico (48 x 48) -> help\src\TTLOGO.BMP
# The ribbon icons are drawn on the button face (#C0C0C0); the face around
# each icon is flood-filled from the corners to white, the Help page's
# colour, leaving any grey inside the drawing alone. Output: 16-colour
# Windows 3.x bitmaps (BMP3, 4 bits), 8.3 names, for the Help Compiler 3.10.
# Needs ImageMagick (magick), a build-time tool only.
$root = Split-Path $PSScriptRoot
$out = Join-Path $PSScriptRoot "src"
$map = [ordered]@{
    home = "HOME"; ripscrip = "RIPSCRIP"; nabu = "NABU"; videotex = "VIDEOTEX"; web = "WEB";
    gopher = "GOPHER"; arpanet = "ARPANET"; wais = "WAIS"; archie = "ARCHIE"; ftp = "FTP";
    newsgroups = "NEWSGRPS"; irc = "IRC"; cuseeme = "CUSEEME"; terminal = "TERMINAL"; gwbasic = "GWBASIC"
}
foreach ($k in $map.Keys) {
    $src = Join-Path $root "assets\w31icons\$k.bmp"
    $dst = Join-Path $out ($map[$k] + ".BMP")
    $fill = @()
    foreach ($c in @("0,0", "31,0", "0,31", "31,31")) {
        $px = & magick $src -format "%[pixel:p{$c}]" info:
        if ($px -eq "srgb(192,192,192)" -or $px -eq "silver") { $fill += @("-draw", "color $c floodfill") }
    }
    & magick $src -fill white -fuzz 0 @fill -colors 16 -type Palette -depth 4 "BMP3:$dst"
    "{0,-12} {1}" -f ($map[$k] + ".BMP"), ((& magick identify -format "%wx%h %k colours" $dst) -join "")
}
$logo = Join-Path $out "TTLOGO.BMP"
& magick (Join-Path $root "assets\brand\teletext_navigator.ico[3]") -background white -alpha remove -alpha off -colors 16 -type Palette -depth 4 "BMP3:$logo"
"{0,-12} {1}" -f "TTLOGO.BMP", ((& magick identify -format "%wx%h %k colours" $logo) -join "")
