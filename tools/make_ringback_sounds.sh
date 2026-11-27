#!/bin/bash
# make_ringback_sounds.sh - add North American ringback to the BBS tab's two
# modem-theatre recordings (docs/NAVIGATOR_BBS_RINGBACK_2026-11-19.md).
#
# Part of Teletext Navigator.
# Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
# Licensed under the GNU Affero General Public License v3.0 or later.
#
# Runs with SoX 14.4.2 (on hermes; the Z840 has no SoX) in a scratch folder
# holding COPIES of the two assets in ./sources:
#   sources/dialup.copy.wav     = assets/sound/dialup.wav     (22,050 Hz s16 mono)
#   sources/intercept.copy.wav  = assets/sound/intercept.wav  (24,000 Hz s16 mono)
# and writes ./out/dialup_ring.wav and ./out/intercept_ring.wav, which become
# assets/sound/dialup_ring.wav and assets/sound/intercept_ring.wav. The
# masters are only read.
#
# The ring: 440 + 480 Hz (North American ringback), 2.0 s, each tone at
# 0.047 FS (the level both assets' 350 + 440 Hz dial tone measures), band-
# passed 300-3400 Hz (the telephone band used for the intercept's voice),
# 5 ms ramps. Made ONCE at 24 kHz; the success copy is that ring resampled to
# 22,050 Hz with SoX's VHQ resampler.
#
# The recordings are cut on zero-valued samples in their quiet gaps (sample
# counts measured in the report, section 2) and joined with -D (no dither),
# so every kept sample is the original's, bit for bit.
set -euo pipefail
cd "$(dirname "$0")"

# ---- the feel calls (the operator's ear decides) --------------------------
RING_S=2.0          # one ring, seconds on
RING_OFF_S=4.0      # silence between ring 1 and ring 2 (success)
FAIL_RING=36000     # failure: the ring CUT OFF at 1.5 s of its 2 s (operator,
                    # 2026-11-19: a disconnected number's intercept seizes the
                    # line mid-ring). 36000 is a zero sample of the ring (440
                    # and 480 Hz both complete whole cycles at 1.5 s), so the
                    # cut is abrupt but has no click.
# ---- cut points (samples) ---------------------------------------------------
DIAL_KEEP=83884     # dialup: 0 .. 3.804 s, 1.0 s after the last DTMF digit
DIAL_RESUME=109546  # dialup: 4.968 s, 50 ms before the answering line opens
ICPT_AT=105839      # intercept: 4.410 s, 1.0 s after the last DTMF digit
ICPT_RESUME=115572  # intercept: 4.8155 s, the zero sample before the first SIT
                    # tone's ramp: the SIT cuts in right after the cut ring
                    # (the cut's 0.405 s of near-silence before it is left out)

mkdir -p work out
: > commands.log
: > sox_stderr.log
run() { printf '%s\n' "$*" >> commands.log; "$@" 2>> sox_stderr.log; }
F24=(-r 24000 -c 1 -b 16 -e signed-integer)
F22=(-r 22050 -c 1 -b 16 -e signed-integer)

# ---- the ring ---------------------------------------------------------------
run sox -R -n "${F24[@]}" work/rb440.wav synth "$RING_S" sine 440 vol 0.047
run sox -R -n "${F24[@]}" work/rb480.wav synth "$RING_S" sine 480 vol 0.047
run sox -R -m -v 1 work/rb440.wav -v 1 work/rb480.wav "${F24[@]}" out/ring_24k.wav sinc 300-3400 fade t 0.005 -0 0.005
run sox -R out/ring_24k.wav "${F22[@]}" out/ring_22k.wav rate -v 22050

# ---- silences (digital zero, -D: no dither, like the intercept's own gap) --
run sox -R -D -n "${F22[@]}" work/off_22k.wav trim 0 "$RING_OFF_S"

# ---- success: dialing, ring, 4 s, ring, the answer and the handshake --------
run sox -R -D sources/dialup.copy.wav "${F22[@]}" work/dial_a.wav trim 0 "${DIAL_KEEP}s"
run sox -R -D sources/dialup.copy.wav "${F22[@]}" work/dial_b.wav trim "${DIAL_RESUME}s"
run sox -R -D work/dial_a.wav out/ring_22k.wav work/off_22k.wav out/ring_22k.wav work/dial_b.wav "${F22[@]}" out/dialup_ring.wav

# ---- failure: dialing, one ring cut off, the SIT at once, the rest unchanged -
run sox -R -D out/ring_24k.wav "${F24[@]}" work/ring_cut_24k.wav trim 0 "${FAIL_RING}s"
run sox -R -D sources/intercept.copy.wav "${F24[@]}" work/icpt_a.wav trim 0 "${ICPT_AT}s"
run sox -R -D sources/intercept.copy.wav "${F24[@]}" work/icpt_b.wav trim "${ICPT_RESUME}s"
run sox -R -D work/icpt_a.wav work/ring_cut_24k.wav work/icpt_b.wav "${F24[@]}" out/intercept_ring.wav

sha256sum out/*.wav sources/*.wav
