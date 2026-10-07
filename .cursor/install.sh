#!/usr/bin/env bash
#
# Idempotent bootstrap for building rabiribi-savestate on Linux and running its
# no-game harnesses/probes under Wine. Installs the zig cross compiler, a 32-bit
# Wine, and a headless X server, initialises the Wine prefixes, then builds.
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG_VERSION="0.14.1"

echo "== zig ${ZIG_VERSION} =="
if [ "$(/opt/zig/zig version 2>/dev/null || true)" != "${ZIG_VERSION}" ]; then
  tmp="$(mktemp -d)"
  curl -fsSL --retry 4 -o "$tmp/zig.tar.xz" \
    "https://ziglang.org/download/${ZIG_VERSION}/zig-x86_64-linux-${ZIG_VERSION}.tar.xz"
  sudo rm -rf /opt/zig && sudo mkdir -p /opt/zig
  sudo tar -xf "$tmp/zig.tar.xz" -C /opt/zig --strip-components=1
  sudo ln -sf /opt/zig/zig /usr/local/bin/zig
  rm -rf "$tmp"
fi
zig version

echo "== wine (32-bit) + headless X =="
if ! command -v wine >/dev/null 2>&1 || ! command -v xvfb-run >/dev/null 2>&1; then
  sudo dpkg --add-architecture i386
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    wine wine32:i386 wine64 xvfb
fi
wine --version

echo "== wine prefixes =="
export WINEDEBUG="${WINEDEBUG:--all}"
# The project's real targets are PE32 (Rabi-Ribi, DDPR), so the win32 prefix is
# the one the harnesses and probes use; the win64 prefix only serves the x64
# test/DLL builds. Baking both into the image saves ~10s of first-run wineboot.
if [ ! -f "$HOME/.wine-rabiribi/system.reg" ]; then
  WINEPREFIX="$HOME/.wine-rabiribi" WINEARCH=win32 wineboot --init >/dev/null 2>&1 || true
fi
if [ ! -f "$HOME/.wine-rabiribi64/system.reg" ]; then
  WINEPREFIX="$HOME/.wine-rabiribi64" WINEARCH=win64 wineboot --init >/dev/null 2>&1 || true
fi

echo "== build =="
./build.sh

echo
echo "install complete. Run a harness/probe with:  ./run.sh rr_harness32.exe 2 1"
