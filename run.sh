#!/usr/bin/env bash
#
# Run one of the built Windows executables under Wine on a headless Linux box.
# Picks the win32 or win64 Wine prefix from the exe's PE machine type, and wraps
# it in a virtual X server because the engine and probes create windows.
#
#   ./run.sh rr_harness32.exe 2 1        # savestate engine, all four conditions
#   ./run.sh ss_harness32.exe 3 2 synth  # savestate engine, synthetic table
#   ./run.sh test_seam.exe               # software rasteriser coverage audit
#   ./run.sh dispprobe32.exe             # monitor-size probe
#
# Engine knobs are read from the environment (see examples/rabiribi/d3d9_sw.cfg
# for the full list), so e.g. `D3D9SW_HEAPBLOCKS=0 ./run.sh ...` works.
set -euo pipefail
cd "$(dirname "$0")"

exe="${1:?usage: ./run.sh <exe> [args...]}"; shift || true
[ -f "$exe" ] || { echo "no such exe: $exe (run ./build.sh first)" >&2; exit 1; }

export WINEDEBUG="${WINEDEBUG:--all}"
# Pick the prefix from the exe's PE width. Set unconditionally rather than
# honouring an inherited WINEPREFIX, so a win32 value left in the environment
# cannot collide with a win64 exe (or vice versa).
if file "$exe" | grep -q "x86-64"; then
  export WINEPREFIX="$HOME/.wine-rabiribi64" WINEARCH=win64
else
  export WINEPREFIX="$HOME/.wine-rabiribi" WINEARCH=win32
fi

exec xvfb-run -a wine "$exe" "$@"
