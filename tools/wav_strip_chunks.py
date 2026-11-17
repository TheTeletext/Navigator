#!/usr/bin/env python3
# wav_strip_chunks.py - write a copy of a PCM WAV holding only its 'fmt '
# and 'data' chunks (editor metadata such as cue, LIST and XMP '_PMX' left
# out). The audio bytes are copied unchanged; the input is only read.
#
# Part of Teletext Navigator.
# Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
# Licensed under the GNU Affero General Public License v3.0 or later.
#
# Usage: python -I tools/wav_strip_chunks.py <master.wav> <copy.wav>
import struct
import sys


def chunks(b):
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        sys.exit("not a RIFF/WAVE file")
    i = 12
    while i + 8 <= len(b):
        cid = b[i:i + 4]
        n = struct.unpack("<I", b[i + 4:i + 8])[0]
        yield cid, b[i + 8:i + 8 + n]
        i += 8 + n + (n & 1)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    b = open(src, "rb").read()
    found = {cid: body for cid, body in chunks(b) if cid in (b"fmt ", b"data")}
    if b"fmt " not in found or b"data" not in found:
        sys.exit("missing fmt or data chunk")
    if struct.unpack("<H", found[b"fmt "][:2])[0] != 1:
        sys.exit("not PCM")
    out = b""
    for cid in (b"fmt ", b"data"):
        body = found[cid]
        out += cid + struct.pack("<I", len(body)) + body + (b"\0" if len(body) & 1 else b"")
    out = b"RIFF" + struct.pack("<I", 4 + len(out)) + b"WAVE" + out
    open(dst, "wb").write(out)
    print("%s: %d B -> %s: %d B (fmt + data only, data %d B unchanged)"
          % (src, len(b), dst, len(out), len(found[b"data"])))


if __name__ == "__main__":
    main()
