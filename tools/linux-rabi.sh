#!/usr/bin/env bash
# Linux restore path for the Rabi-Ribi savestate wrapper under Proton.
# This is the Linux-side future (not distro Wine + a no-game harness).
#
# Mirrors tools/wrapper.ps1 and tools/xrestore.ps1 without adding a Wine/MinGW
# toolchain to the tree. The compiler is still zig cc, same flags as build.ps1.
# Native (our) DLLs next to rabiribi.exe only win if Wine is told to prefer
# them; Proton's DXVK d3d11 is otherwise builtin-first.
# Audio is ON by default: the Pulse/ALSA/PipeWire mixer is exactly what a
# native Linux build has to survive, so the restore has to work with it live.
# RABI_NOAUDIO=1 puts the old -noaudio silence back for an A/B.
# F5 saves, Shift+F5 loads; in-session room-to-room restore works in the
# same world.
# First launch shows language select; later launches go straight to the title
# (PRESS START). xrestore / SAVE_AT frame numbers are title-relative after that.
#
#   tools/linux-rabi.sh build
#   tools/linux-rabi.sh deploy
#   tools/linux-rabi.sh status
#   tools/linux-rabi.sh ps        # pid / window / Steam hold / slot files
#   tools/linux-rabi.sh launch
#   tools/linux-rabi.sh cycle [n]  # one title save, then n restores of it
#   tools/linux-rabi.sh xrestore [frame [quit]]
#
# xrestore always sets D3D11SW_GPU=0 (docs/determinism.md: do not restore
# against the live GPU backend).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STEAM_ROOT="${STEAM_ROOT:-$HOME/.steam/debian-installation}"
GAME="${RABI_GAME:-$STEAM_ROOT/steamapps/common/Rabi-Ribi}"
PROTON="${RABI_PROTON:-$STEAM_ROOT/steamapps/common/Proton - Experimental/proton}"
COMPAT="${STEAM_COMPAT_DATA_PATH:-$STEAM_ROOT/steamapps/compatdata/400910}"
APPID=400910
OUT="$ROOT/x86"
DET="$ROOT/det/xrestore"
# Where cmd_cycle parks the one snapshot it made, to prove the restores after it
# all came back to that same file. Outside the game folder so the wrapper never
# sees it as a slot of its own.
DET_SLOT="${RABI_DET_SLOT:-/tmp/rabi-cycle-slot0.bin}"

# Wine must load the game-folder copies, not DXVK/wined3d. dsound is
# deliberately absent: our same-folder dsound.dll forwards to the silent
# software device in ds_sw.c, so forcing it native means no sound no matter
# what the launch flags say. Wine's builtin dsound reaches winepulse.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d11,dxgi,xaudio2_9,xinput1_4=n,b}"

dlls=(d3d11.dll dxgi.dll dsound.dll xaudio2_9.dll xinput1_4.dll)

die() { echo "  $*" >&2; exit 1; }

need_game() {
	[[ -x "$GAME/rabiribi.exe" ]] || die "game folder not found: $GAME"
}

game_running() {
	# Match the Wine process by name, not by command line. Every stage of the
	# Steam launcher chain - steam-launch-wrapper, reaper, srt-bwrap, pv-adverb,
	# proton - carries the exe path as an argument, so an args- match is true a
	# second after launch and stays true after the game itself is gone. It made
	# wait_for_game return before the game existed and wait_until_exit report an
	# exit nine seconds in, while the game ran on for minutes.
	pgrep -x 'rabiribi.exe' >/dev/null 2>&1
}

zig_bin() {
	if [[ -n "${ZIG:-}" && -x "$ZIG" ]]; then
		echo "$ZIG"
		return
	fi
	if command -v zig >/dev/null 2>&1; then
		command -v zig
		return
	fi
	if [[ -x /home/ubuntu/zig/zig ]]; then
		echo /home/ubuntu/zig/zig
		return
	fi
	die "zig not on PATH (build.ps1 uses zig cc; install Zig, do not commit it)"
}

cmd_build() {
	local zig warn d3d11src
	zig="$(zig_bin)"
	mkdir -p "$OUT"
	warn=(-O2 -Wall -Wno-incompatible-function-pointer-types)
	d3d11src=(d3d11_sw.c dxbc.c savestate.c swrast.c trace.c dsoundhook.c ds_sw.c xa2_sw.c gameheap.c gpuprobe.c gpu.c d3d11.def)
	echo "=== zig cc -target x86-windows-gnu (same flags as build.ps1) ==="
	(cd "$ROOT" && "$zig" cc "${warn[@]}" -DD3D9SW_VARIANT=d3d11 \
		-DSWRAST_DEFAULT_THREADS=32 -DSWRAST_THREADS_PHYSICAL \
		-target x86-windows-gnu -shared -o "$OUT/d3d11.dll" "${d3d11src[@]}" \
		-lgdi32 -luser32 -lwinmm "-Wl,--image-base=0x60000000")
	(cd "$ROOT" && "$zig" cc "${warn[@]}" -target x86-windows-gnu -shared \
		-o "$OUT/dxgi.dll" dxgi_fwd.c dxgi.def "-Wl,--image-base=0x61000000")
	(cd "$ROOT" && "$zig" cc "${warn[@]}" -target x86-windows-gnu -shared \
		-o "$OUT/dsound.dll" dsound_fwd.c dsound.def)
	(cd "$ROOT" && "$zig" cc "${warn[@]}" -target x86-windows-gnu -shared \
		-o "$OUT/xaudio2_9.dll" xa2_fwd.c xaudio2_9.def -luser32 \
		"-Wl,--image-base=0x62000000")
	(cd "$ROOT" && "$zig" cc "${warn[@]}" -target x86-windows-gnu -shared \
		-o "$OUT/xinput1_4.dll" xinput_sw.c xinput.def \
		"-Wl,--image-base=0x63000000")
	echo "pinning wrapper image bases:"
	python3 "$ROOT/tools/noaslr.py" \
		"$OUT/d3d11.dll" "$OUT/dxgi.dll" "$OUT/xaudio2_9.dll" "$OUT/xinput1_4.dll"
	echo "  built into $OUT"
}

set_knobs() {
	local cfg="$1"
	shift
	python3 - "$cfg" "$@" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
kv = dict(a.split("=", 1) for a in sys.argv[2:])
text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
lines = text.splitlines()
seen = set()
out = []
for line in lines:
    key = line.split("=", 1)[0] if line.startswith("D3D") else None
    if key and key in kv:
        out.append(f"{key}={kv[key]}")
        seen.add(key)
    else:
        out.append(line)
for k, v in kv.items():
    if k not in seen:
        out.append(f"{k}={v}")
path.write_text("\n".join(out).rstrip() + "\n", encoding="utf-8")
PY
}

cmd_deploy() {
	need_game
	if game_running; then
		die "the game is running - close it first, or the files are locked"
	fi
	[[ -f "$OUT/d3d11.dll" ]] || die "no $OUT/d3d11.dll - run: tools/linux-rabi.sh build"
	for n in "${dlls[@]}"; do
		[[ -f "$OUT/$n" ]] || die "missing $OUT/$n"
		cp -f "$OUT/$n" "$GAME/$n"
	done
	cp -f "$ROOT/examples/rabiribi/d3d11_sw.cfg" "$GAME/d3d11_sw.cfg"
	cp -f "$ROOT/examples/rabiribi/d3d9_sw.cfg" "$GAME/d3d9_sw.cfg"
	# CPU-only restore: software raster, 4 threads on this class of box, pad pinned.
	set_knobs "$GAME/d3d9_sw.cfg" \
		D3D11SW_GPU=0 D3D9SW_XINPUT=0 D3D9SW_DIFFWRITE=1 D3D9SW_ENTS=1 \
		D3D9SW_SLOTFILE=1 D3D9SW_SAVE_AT=0 D3D9SW_LOAD_AT=0 D3D9SW_QUIT_AT=0
	set_knobs "$GAME/d3d11_sw.cfg" D3D11SW_GPU=0
	pin_dll_overrides
	echo "  deployed wrappers + cfg into $GAME"
	cmd_status
}

wrapper_state() {
	local live=0 off=0 n
	for n in d3d11.dll dxgi.dll; do
		[[ -f "$GAME/$n" ]] && live=$((live + 1))
		[[ -f "$GAME/$n.off" ]] && off=$((off + 1))
	done
	if [[ $live -eq 2 && $off -eq 0 ]]; then
		echo on
	elif [[ $off -eq 2 && $live -eq 0 ]]; then
		echo off
	else
		echo "mixed (live=$live disabled=$off)"
	fi
}

cmd_status() {
	need_game
	echo "wrapper is $(wrapper_state) in $GAME"
	if [[ -f "$GAME/d3d11_sw.log" ]]; then
		echo "  last d3d11_sw.log: $(wc -c < "$GAME/d3d11_sw.log") bytes, $(date -r "$GAME/d3d11_sw.log" '+%F %T')"
	else
		echo "  last d3d11_sw.log: none"
	fi
	if [[ -f "$GAME/d3d9_sw_savestate_rabiribi.txt" ]]; then
		echo "  last savestate log: $(wc -c < "$GAME/d3d9_sw_savestate_rabiribi.txt") bytes"
	fi
	cmd_ps
}

# One snapshot of whether the game is actually alive. Steam can still show
# Stop after rabiribi.exe and its window are gone; pgrep -x is the truth.
cmd_ps() {
	local pid w log
	echo "=== rabi ps $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
	if pid=$(pgrep -x 'rabiribi.exe' 2>/dev/null); then
		echo "rabiribi.exe: RUNNING pid=$pid"
		ps -p "$pid" -o pid,etime,stat,cmd --no-headers || true
		if [[ -r "/proc/$pid/cmdline" ]]; then
			echo "cmdline: $(tr '\0' ' ' < "/proc/$pid/cmdline")"
		fi
	else
		echo "rabiribi.exe: NOT RUNNING"
	fi
	if command -v xdotool >/dev/null 2>&1; then
		w=$(DISPLAY="${DISPLAY:-:1}" xdotool search --name 'Rabi-Ribi' getwindowname 2>/dev/null | head -1 || true)
		if [[ -n "$w" ]]; then
			echo "window: $w"
		else
			echo "window: none"
		fi
	fi
	log="$STEAM_ROOT/logs/console_log.txt"
	if [[ -f "$log" ]]; then
		echo "steam 400910 (last):"
		grep '400910' "$log" | grep -E 'Game process (added|updated|removed)|ExecuteSteamURL|ShowGameArgs' | tail -5 | sed 's/^/  /'
	fi
	shopt -s nullglob
	local slots=("$GAME"/d3d9sw_slot*.bin)
	if ((${#slots[@]})); then
		ls -lh "${slots[@]}" | awk '{print "slot:", $9, $5, $6, $7, $8}'
	else
		echo "slot: none"
	fi
}

cmd_off() {
	need_game
	if game_running; then die "the game is running - close it first"; fi
	local n
	for n in d3d11.dll dxgi.dll; do
		[[ -f "$GAME/$n" ]] && mv -f "$GAME/$n" "$GAME/$n.off"
	done
	[[ -f "$GAME/d3d11_sw.log" ]] && mv -f "$GAME/d3d11_sw.log" "$GAME/d3d11_sw.log.ours"
	echo "wrapper is now $(wrapper_state) - this run uses Proton/DXVK d3d11"
}

cmd_on() {
	need_game
	if game_running; then die "the game is running - close it first"; fi
	local n
	for n in d3d11.dll dxgi.dll; do
		[[ -f "$GAME/$n.off" ]] && mv -f "$GAME/$n.off" "$GAME/$n"
	done
	echo "wrapper is now $(wrapper_state)"
}

pin_dll_overrides() {
	# Proton's DXVK is builtin-first for d3d11. Native game-folder copies
	# only load if the prefix says so. Direct `proton waitforexitandrun`
	# also exits 53 when Steam is already holding the app; steam:// works.
	local reg="$COMPAT/pfx/user.reg"
	[[ -f "$reg" ]] || return 0
	python3 - "$reg" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
text = p.read_text(encoding="utf-8", errors="replace")
needle = "[Software\\\\Wine\\\\DllOverrides]"
keys = {
    '"d3d11"="native,builtin"',
    '"dxgi"="native,builtin"',
    '"dsound"="native,builtin"',
    '"xaudio2_9"="native,builtin"',
    '"xinput1_4"="native,builtin"',
}
if needle not in text:
    sys.exit(0)
start = text.index(needle)
end = text.find("\n[", start + 1)
block = text[start:end if end != -1 else None]
missing = [k for k in keys if k not in block]
if not missing:
    sys.exit(0)
insert_at = (end if end != -1 else len(text))
# Insert before the next section (and its leading newline).
extra = "".join(k + "\n" for k in missing)
if end == -1:
    p.write_text(text + extra, encoding="utf-8")
else:
    p.write_text(text[:end] + extra + text[end:], encoding="utf-8")
print("  prefix DllOverrides: native d3d11,dxgi,dsound,xaudio2_9,xinput1_4")
PY
}

launch_game() {
	need_game
	[[ -d "$COMPAT" ]] || die "compatdata missing: $COMPAT"
	if game_running; then die "game already running"; fi
	pin_dll_overrides
	export DISPLAY="${DISPLAY:-:1}"
	export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT"
	export STEAM_COMPAT_DATA_PATH="$COMPAT"
	# Bare `proton waitforexitandrun` exits 53 while Steam holds the app.
	# steam://rungameid pops Steam's "custom arguments" prompt if
	# LaunchOptions is set. The path that actually started rabiribi.exe
	# on this box is steam-launch-wrapper + reaper + SteamLinuxRuntime.
	local wrap="$STEAM_ROOT/ubuntu12_32/steam-launch-wrapper"
	local reaper="$STEAM_ROOT/ubuntu12_32/reaper"
	local slr="$STEAM_ROOT/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point"
	[[ -x "$wrap" && -x "$reaper" && -x "$slr" && -x "$PROTON" ]] \
		|| die "steam-launch-wrapper/reaper/proton/SLR missing"
	local args=()
	[[ -n "${RABI_NOAUDIO:-}" ]] && args+=(-noaudio)
	"$wrap" -- "$reaper" SteamLaunch AppId="$APPID" -- \
		"$slr" --verb=waitforexitandrun -- \
		"$PROTON" waitforexitandrun "$GAME/rabiribi.exe" "${args[@]}"
}

cmd_launch() {
	echo "WINEDLLOVERRIDES=$WINEDLLOVERRIDES"
	launch_game
}

# One save on the title screen, then N restores of that one snapshot.
#
# This is the thing shift could not do. D3D9SW_LOAD_VK gives load a key of its
# own, so each press is independent and nothing has to stay held across a second
# keystroke - which neither Wine's input path nor this VM's X driver managed.
# The keys come from the deployed cfg rather than being hardcoded, so a cfg that
# says something else is tested instead of silently ignored.
#
# Waits on the wrapper's own log counters, not on a timer: a press that did not
# register looks exactly like a restore still running otherwise. And it settles
# between presses, because a load sent while the previous one was still resuming
# was dropped.
cmd_cycle() {
	local want="${1:-2}" cfg="$GAME/d3d9_sw.cfg"
	local d11="$GAME/d3d11_sw.log" ss="$GAME/d3d9_sw_savestate_rabiribi.txt"
	local wid savek loadk pid n before

	need_game
	game_running || die "the game is not running - tools/linux-rabi.sh launch"
	command -v xdotool >/dev/null 2>&1 || die "xdotool is needed to send the keys"
	savek=$(sed -n 's/^D3D9SW_SAVE_VK=//p' "$cfg" | tail -1)
	loadk=$(sed -n 's/^D3D9SW_LOAD_VK=//p' "$cfg" | tail -1)
	[[ -n "$savek" ]] || die "D3D9SW_SAVE_VK is not set in $cfg"
	[[ -n "$loadk" ]] || die "D3D9SW_LOAD_VK is not set in $cfg - load would need shift, \
which does not survive automation"
	export DISPLAY="${DISPLAY:-:1}"
	wid=$(xdotool search --name 'Rabi-Ribi ver' | head -1)
	[[ -n "$wid" ]] || die "no Rabi-Ribi window"
	pid=$(pgrep -x 'rabiribi.exe')
	echo "cycle: pid $pid, save on '$savek', load on '$loadk', $want restore(s)"

	# The keys are sent as single characters, which is what the cfg spells them
	# as. A function key would need the F5 form, so pass it through as written.
	#
	# Nothing here is fatal. Under set -e a failing xdotool - which is what a
	# window that has just gone away looks like - killed the whole cycle silently,
	# so a process that died between two restores was reported as neither a
	# restore nor a death.
	press() {
		if ! xdotool windowactivate --sync "$wid" 2>/dev/null; then
			echo "  press '$1': the window is gone"
			return 1
		fi
		sleep 0.3
		xdotool keydown --window "$wid" "$1" 2>/dev/null || return 1
		sleep 0.12
		xdotool keyup --window "$wid" "$1" 2>/dev/null || true
		return 0
	}
	# Counted from the logs, which are the only place that says a save or a load
	# actually happened rather than that a key was sent. grep -c prints 0 and
	# exits 1 on no match, so the exit status is discarded rather than defaulted -
	# an `|| echo 0` here appended a second line and every comparison after it
	# was a syntax error.
	count() {
		local c
		c=$(grep -ac "$2" "$1" 2>/dev/null) || true
		echo "${c:-0}"
	}
	saves() { count "$d11" 'savestate saved slot'; }
	# The savestate log, not the wrapper log. The wrapper prints "restored" after
	# the rewound thread has returned all the way out, so a restore that lands and
	# then faults on resume never reaches that line - which read as "the key did
	# not register" for a restore that had in fact happened. This line is written
	# while the threads are still frozen.
	loads() { count "$ss" 'load: slot'; }
	settled() {
		local what="$1" was="$2" fn="$3" j got died=0
		for ((j = 0; j < 60; j++)); do
			game_running || died=1
			got=$("$fn")
			if (( got > was )); then
				echo "  $what: landed after $((j * 500)) ms$(
					((died)) && echo ", but the process is gone")"
				((died)) && return 2
				return 0
			fi
			if ((died)); then
				echo "  $what: THE GAME DIED without logging it"
				return 2
			fi
			sleep 0.5
		done
		echo "  $what: never logged - the key did not reach the wrapper"
		return 1
	}

	local saves_after_one rc
	before=$(saves)
	press "$savek" || die "could not send the save key"
	settled "save" "$before" saves || return $?
	ls -lh "$GAME"/d3d9sw_slot0.bin | awk '{print "  slot:", $9, $5}'
	# The one snapshot every restore below has to come back to. Kept so a load
	# key that turned out to save is caught: that would pass every check below
	# and prove nothing.
	saves_after_one=$(saves)
	cp -f "$GAME/d3d9sw_slot0.bin" "$DET_SLOT" 2>/dev/null || true

	for ((n = 1; n <= want; n++)); do
		before=$(loads)
		# Long enough that the previous restore has finished resuming. A press a
		# fifth of a second after one landed was swallowed.
		sleep 3
		if ! game_running; then
			echo "  restore $n: the process died before the key was sent," \
			     "$((n - 1)) restore(s) in"
			grep -E 'fault: C0000005' "$ss" | tail -1 | sed 's/^/  /'
			return 2
		fi
		press "$loadk" || {
			grep -E 'fault: C0000005' "$ss" | tail -1 | sed 's/^/  /'
			return 2
		}
		settled "restore $n" "$before" loads
		rc=$?
		echo "  after restore $n: pid $(pgrep -x 'rabiribi.exe' || echo GONE)"
		if ((rc != 0)); then
			grep -E 'fault: C0000005' "$ss" | tail -1 | sed 's/^/  /'
			return $rc
		fi
	done

	if (( $(saves) != saves_after_one )); then
		echo "  WARNING: something saved again during the restores - the later"
		echo "  loads did not all come back to the same snapshot"
	elif [[ -f "$DET_SLOT" ]] && ! cmp -s "$DET_SLOT" "$GAME/d3d9sw_slot0.bin"; then
		echo "  WARNING: the slot file changed under us"
	else
		echo "  slot is still the one save, so every restore came back to it"
	fi
	echo "cycle: done, $want restore(s) of one save"
	cmd_ps
}

wait_for_game() {
	local t0 now
	t0=$(date +%s)
	while true; do
		if game_running; then
			return 0
		fi
		now=$(date +%s)
		if (( now - t0 > 90 )); then
			return 1
		fi
		sleep 0.5
	done
}

wait_until_exit() {
	local timeout="${1:-240}" t0 now
	t0=$(date +%s)
	while game_running; do
		now=$(date +%s)
		if (( now - t0 > timeout )); then
			pkill -f 'steamapps/common/Rabi-Ribi/rabiribi.exe' 2>/dev/null || \
				pkill -f '[\\]rabiribi\.exe' 2>/dev/null || true
			sleep 2
			return 1
		fi
		sleep 1
	done
	return 0
}

run_once() {
	local label="$1"
	local log="$GAME/d3d9_sw_savestate_rabiribi.txt"
	rm -f "$log"
	if [[ -f "$GAME/reset-save.ps1" ]]; then
		echo "  reset-save.ps1 is a Windows script; not invoked here" >&2
	else
		echo "  reset-save.ps1 missing - save data is NOT pinned" >&2
	fi
	launch_game &
	local launch_pid=$!
	if ! wait_for_game; then
		echo "  never started" >&2
		wait "$launch_pid" || true
		return 1
	fi
	# Software raster can be slower than 60 fps; give the 1800-frame quit room.
	if ! wait_until_exit 420; then
		echo "  timed out; killed" >&2
	fi
	wait "$launch_pid" || true
	mkdir -p "$DET"
	if [[ -f "$log" ]]; then
		cp -f "$log" "$DET/$label.txt"
		echo "$DET/$label.txt"
		return 0
	fi
	return 1
}

cmd_xrestore() {
	local frame="${1:-1200}" quit="${2:-1800}"
	need_game
	[[ -f "$GAME/d3d11.dll" ]] || die "wrapper not deployed - run: tools/linux-rabi.sh deploy"
	mkdir -p "$DET"

	echo "=== launch 1: save at frame $frame, quit at $quit ==="
	set_knobs "$GAME/d3d9_sw.cfg" \
		D3D11SW_GPU=0 D3D9SW_SLOTFILE=1 \
		"D3D9SW_SAVE_AT=$frame" D3D9SW_LOAD_AT=0 "D3D9SW_QUIT_AT=$quit"
	local l1
	if ! l1="$(run_once launch1-save)"; then
		echo "  no savestate log from launch 1" >&2
		set_knobs "$GAME/d3d9_sw.cfg" D3D9SW_SAVE_AT=0 D3D9SW_LOAD_AT=0 D3D9SW_QUIT_AT=0
		exit 1
	fi
	echo "  log: $l1"
	shopt -s nullglob
	local slots=("$GAME"/d3d9sw_slot*.bin)
	if ((${#slots[@]})); then
		local s
		for s in "${slots[@]}"; do
			echo "  slot file: $(basename "$s")  $(wc -c < "$s") bytes"
		done
	else
		echo "  no slot file was written - the rest of this test is meaningless" >&2
		set_knobs "$GAME/d3d9_sw.cfg" D3D9SW_SAVE_AT=0 D3D9SW_LOAD_AT=0 D3D9SW_QUIT_AT=0
		exit 1
	fi

	echo
	echo "=== launch 2: restore at frame $frame, quit at $quit ==="
	set_knobs "$GAME/d3d9_sw.cfg" D3D9SW_SAVE_AT=0 "D3D9SW_LOAD_AT=$frame"
	local l2
	if ! l2="$(run_once launch2-restore)"; then
		echo "  no log from launch 2" >&2
		set_knobs "$GAME/d3d9_sw.cfg" D3D9SW_SAVE_AT=0 D3D9SW_LOAD_AT=0 D3D9SW_QUIT_AT=0
		exit 1
	fi

	echo
	echo "=== what the restore said ==="
	grep -E 'load-at:|REFUSED|refus|abort|mismatch|cannot|excluded now but was saved|restore|slotfile|region stage' "$l2" | head -40 | sed 's/^/  /' || true
	echo
	echo "=== and whether it survived ==="
	grep -E 'quit-at: frame|exception|fault|C0000005' "$l2" | head -6 | sed 's/^/  /' || true

	set_knobs "$GAME/d3d9_sw.cfg" D3D9SW_SAVE_AT=0 D3D9SW_LOAD_AT=0 D3D9SW_QUIT_AT=0
	echo
	echo "  knobs disarmed - the game launches and plays normally again"
	echo "  logs in det/xrestore/"
}

cmd_stop() {
	if game_running; then
		pkill -f 'steamapps/common/Rabi-Ribi/rabiribi.exe' 2>/dev/null || \
			pkill -f '[\\]rabiribi\.exe' 2>/dev/null || true
		sleep 1
		echo "  stopped rabiribi"
	else
		echo "  not running"
	fi
}

usage() {
	sed -n '2,26p' "$0"
}

cmd="${1:-}"
shift || true
case "$cmd" in
	build) cmd_build ;;
	deploy) cmd_deploy ;;
	status) cmd_status ;;
	ps) cmd_ps ;;
	on) cmd_on ;;
	off) cmd_off ;;
	launch) cmd_launch ;;
	cycle) cmd_cycle "${1:-2}" ;;
	stop) cmd_stop ;;
	xrestore) cmd_xrestore "${1:-1200}" "${2:-1800}" ;;
	-h|--help|help|"") usage ;;
	*) die "unknown command: $cmd" ;;
esac
