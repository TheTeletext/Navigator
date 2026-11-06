# OpenNabu IPL source, as vendored by Teletext Navigator

Upstream: https://github.com/buricco/opennabuipl
Licence: **MIT**, Copyright 2012, 2013, 2015, 2023 S. V. Nickolas.
See `license.txt`, which is upstream's own and is reproduced unmodified.

This directory is a **pristine, unmodified** copy of the upstream source, at
the commit recorded in `UPSTREAM-COMMIT.txt`. Nothing here is edited.
Navigator's own modified copy is a separate file, `firmware/mgtipl/mgtipl.a80`,
and every difference between the two is listed in
`firmware/mgtipl/MGT-CHANGES.md`.

Keeping the two apart is the point. A historian, or a future session, can
diff them and see exactly what Navigator changed and what it inherited.

## Building it

Upstream's `Makefile` calls the `z80asm` from the Debian repositories,
version 1.8. That is NOT the same assembler as z88dk's `z88dk-z80asm`; the
dialects differ and the source does not assemble under z88dk unmodified.
Navigator assembles both this and its fork on the Unicorn server, which is
Debian, with:

    z80asm -o opennabu.bin opennabu.a80

Assembling the vendored source with Debian z80asm 1.8 produces a 4096-byte
image with sha256

    44ee0854bb85896afbf056503e7a19efff250999f0885f227836e9bb9fa2fedd

## The assembled image

`third_party/opennabu/opennabu.bin` is exactly this source assembled
(sha256 `44ee0854...`), and it is what the installer ships as the fallback
boot ROM. It waits for the keyboard's power-on byte before it starts, which
the NABU tab's built-in keyboard provides, and it boots the live channel
end to end.
