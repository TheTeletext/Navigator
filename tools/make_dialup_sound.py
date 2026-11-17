#!/usr/bin/env python3
# make_dialup_sound.py - build assets/sound/dialup.wav, the BBS tab's modem
# handshake (Navigator 1.7.2, docs/2026-11-16_NAVIGATOR_172_MODEM_SIM.md).
#
# Part of Teletext Navigator.
# Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
# Licensed under the GNU Affero General Public License v3.0 or later.
#
# SOURCE: "The Sound of dial-up Internet" by wtermini,
#   https://freesound.org/people/wtermini/sounds/546450/
#   Creative Commons 0 (CC0 1.0, public domain dedication). 28.67 s.
# The original upload needs a Freesound login; the site's public high-quality
# preview of the same sound (MP3, 48 kHz stereo, LAME 3.99) is fetched
# instead and pinned by SHA-256 below, so a rebuild is reproducible.
#
# PROCESSING (ffmpeg, bit-exact flags so the output is byte-reproducible):
#   * the two channels carry the same signal (L-R measured at -51 dB), so
#     they are averaged to mono;
#   * leading silence (0 to 1.17 s) trimmed at -50 dBFS;
#   * trailing silence trimmed at -50 dBFS, needing 50 ms of sound to count,
#     so the 10 ms click at 27.01 s inside the tail silence is dropped too;
#   * resampled to 22,050 Hz, 16-bit PCM WAV: the waveOut format the app's
#     audio path plays with no decoder (everything above 8 kHz in the
#     recording is at -67 dB, so nothing audible is lost).
#
# Usage: python -I tools/make_dialup_sound.py  [--ffmpeg PATH]
import argparse
import hashlib
import os
import subprocess
import sys
import urllib.request

URL = "https://cdn.freesound.org/previews/546/546450_11279746-hq.mp3"
SHA256 = "87de6e1a2b28f32d573866ca1c9f7fa0fdc7cd85c149a5e9e3551df2bea2d6e9"

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "build", "sound_cache")
OUT = os.path.join(ROOT, "assets", "sound", "dialup.wav")

FILTER = ("pan=mono|c0=0.5*c0+0.5*c1,"
          "silenceremove=start_periods=1:start_threshold=-50dB:start_duration=0.05,"
          "areverse,"
          "silenceremove=start_periods=1:start_threshold=-50dB:start_duration=0.05,"
          "areverse,"
          "aresample=22050:resampler=soxr")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(65536), b""):
            h.update(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ffmpeg", default=r"C:\ffmpeg\bin\ffmpeg.exe")
    a = ap.parse_args()
    os.makedirs(CACHE, exist_ok=True)
    src = os.path.join(CACHE, "546450_hq.mp3")
    if not os.path.exists(src) or sha256(src) != SHA256:
        req = urllib.request.Request(URL, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=60) as r, open(src + ".part", "wb") as f:
            f.write(r.read())
        os.replace(src + ".part", src)
    got = sha256(src)
    if got != SHA256:
        sys.exit("downloaded file does not match the pinned SHA-256: " + got)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    cmd = [a.ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", src,
           "-af", FILTER, "-ac", "1", "-c:a", "pcm_s16le",
           "-map_metadata", "-1", "-fflags", "+bitexact", "-flags:a", "+bitexact",
           OUT]
    subprocess.run(cmd, check=True)
    size = os.path.getsize(OUT)
    secs = (size - 44) / 2.0 / 22050.0
    print("%s  %d bytes  ~%.2f s  sha256 %s" % (OUT, size, secs, sha256(OUT)))


if __name__ == "__main__":
    main()
