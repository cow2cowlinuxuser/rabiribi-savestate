#!/usr/bin/env bash
#
# Linux build of every artifact build.ps1 compiles, using the same zig cross
# compiler and the same target triples. It exists so the project can be built
# and its no-game harnesses exercised (under Wine) on a Linux Cloud Agent.
#
# What it deliberately leaves out, relative to build.ps1:
#   - the Steam deploy steps (Rabi-Ribi / OSFE / Haydee), which copy DLLs into
#     game folders that only exist on the developer's Windows machine and are
#     already guarded by Test-Path there;
#   - tools/noaslr.ps1, which clears the DYNAMICBASE bit for cross-session
#     determinism and depends on Windows-only Get-ProcessMitigation. It is a
#     refinement, not a prerequisite for building or running.
#
# Everything else is byte-for-byte the same compiler invocation as build.ps1.
set -euo pipefail
cd "$(dirname "$0")"

ZIG="${ZIG:-zig}"
WARN=(-O2 -Wall -Wno-incompatible-function-pointer-types)

src=(d3d9_sw.c swrast.c savestate.c vsinterp.c trace.c tramp.c allocwatch.c \
     dsoundhook.c ds_sw.c xa2_sw.c gameheap.c d3d9.def)

echo "== d3d9_sw.dll / d3d9_sw_test.exe (x64) =="
"$ZIG" cc "${WARN[@]}" -DD3D9SW_VARIANT=stock -target x86_64-windows-gnu -shared -o d3d9_sw.dll "${src[@]}" -lgdi32 -luser32 -lwinmm
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o d3d9_sw_test.exe test.c -luser32 -lgdi32 -lwinmm

echo "== savestate harnesses (x64 + PE32) =="
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o ss_harness.exe   ss_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o ss_harness32.exe ss_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o ds_harness32.exe ds_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -o rr_harness32.exe rr_harness.c savestate.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c -luser32 -lwinmm

echo "== standalone probes / replays (PE32) =="
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu -o gh_replay32.exe gh_replay.c
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu -o dispprobe32.exe dispprobe.c -lgdi32 -luser32
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu -o baseprobe32.exe baseprobe.c

echo "== test_seam.exe (x64 coverage + sampling audit) =="
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -o test_seam.exe test_seam.c swrast.c -luser32 -lgdi32 -lwinmm

echo "== x86 D3D9 drop-in (PE32) =="
mkdir -p x86
"$ZIG" cc "${WARN[@]}" -DD3D9SW_VARIANT=stock -target x86-windows-gnu -shared -o x86/d3d9.dll "${src[@]}" -lgdi32 -luser32 -lwinmm
cp -f x86/d3d9.dll x86/d3d9_sw.dll
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu -o x86/d3d9_sw_test.exe test.c -luser32 -lgdi32 -lwinmm

echo "== x86-lowspec D3D9 (PE32, -O3 -march=haswell, needs AVX2 to run) =="
low=(-O3 -Wall -Wno-incompatible-function-pointer-types -march=haswell -mtune=skylake -ffp-contract=off)
mkdir -p x86-lowspec
"$ZIG" cc "${low[@]}" -DD3D9SW_VARIANT=lowspec -target x86-windows-gnu -shared -o x86-lowspec/d3d9.dll "${src[@]}" -lgdi32 -luser32 -lwinmm
cp -f x86-lowspec/d3d9.dll x86-lowspec/d3d9_sw.dll

echo "== software D3D11 backend (x64 + PE32) =="
d3d11src=(d3d11_sw.c dxbc.c savestate.c swrast.c trace.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c gpuprobe.c gpu.c d3d11.def)
"$ZIG" cc "${WARN[@]}" -DD3D9SW_VARIANT=d3d11 -DSWRAST_DEFAULT_THREADS=32 -DSWRAST_THREADS_PHYSICAL -target x86_64-windows-gnu -shared -o d3d11.dll "${d3d11src[@]}" -lgdi32 -luser32 -lwinmm
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -shared -o dxgi.dll dxgi_fwd.c dxgi.def
"$ZIG" cc "${WARN[@]}" -DD3D9SW_VARIANT=d3d11 -DSWRAST_DEFAULT_THREADS=32 -DSWRAST_THREADS_PHYSICAL -target x86-windows-gnu -shared -o x86/d3d11.dll "${d3d11src[@]}" -lgdi32 -luser32 -lwinmm -Wl,--image-base=0x60000000
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu -shared -o x86/dxgi.dll dxgi_fwd.c dxgi.def -Wl,--image-base=0x61000000

echo "== same-folder audio/input trampolines =="
"$ZIG" cc "${WARN[@]}" -target x86_64-windows-gnu -shared -o dsound.dll     dsound_fwd.c dsound.def
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -shared -o x86/dsound.dll dsound_fwd.c dsound.def
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -shared -o x86/xaudio2_9.dll xa2_fwd.c xaudio2_9.def -luser32 -Wl,--image-base=0x62000000
"$ZIG" cc "${WARN[@]}" -target x86-windows-gnu    -shared -o x86/xinput1_4.dll xinput_sw.c xinput.def -Wl,--image-base=0x63000000

echo "== software OpenGL (PE32) =="
# gen_gl.py reads the mingw GL headers out of zig's own libc tree; point it there.
ZIG_LIB=$("$ZIG" env | python3 -c "import json,sys;print(json.load(sys.stdin)['lib_dir'])")
export ZIG_GL_INC="$ZIG_LIB/libc/include/any-windows-any/GL"
python3 gen_gl.py
glsrc=(gl_sw.c gl_stubs.c savestate.c swrast.c trace.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c opengl32.def)
"$ZIG" cc "${WARN[@]}" -Wno-inconsistent-dllimport -DD3D9SW_VARIANT=gl -DSWRAST_DEFAULT_THREADS=8 -target x86-windows-gnu -shared -o x86/opengl32.dll "${glsrc[@]}" -lgdi32 -luser32 -lwinmm

echo
echo "build complete. artifacts:"
echo "  x64: d3d9_sw.dll d3d11.dll dxgi.dll dsound.dll ss_harness.exe test_seam.exe d3d9_sw_test.exe"
echo "  x86: x86/d3d9.dll x86/d3d11.dll x86/dxgi.dll x86/dsound.dll x86/xaudio2_9.dll x86/xinput1_4.dll x86/opengl32.dll"
echo "       x86-lowspec/d3d9.dll (needs AVX2)"
echo "  PE32 harnesses/probes: rr_harness32.exe ss_harness32.exe ds_harness32.exe gh_replay32.exe dispprobe32.exe baseprobe32.exe"
