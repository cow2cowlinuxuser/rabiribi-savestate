#!/usr/bin/env bash
#
# Does the module map repeat across launches under Proton?
#
# On Windows this question had a discouraging answer. rabiribi.exe is
# DYNAMICBASE, Windows picks its ASLR base once per boot, and clearing the flag
# to pin it made Steam's DRM stub refuse to launch the binary at all. That one
# moving module is why cross-session restore fails at the region stage, and
# tools/baserun.ps1 exists to measure it.
#
# The Proton session on 2026-09-16 reported something the Windows side never
# did:
#
#   module rabiribi.exe   17208 KB at 00400000, rewound
#
# 00400000 is the link-time preferred base - the address the exe asks for. Wine
# appears to grant it. If that holds, the blocker is absent here rather than
# merely safer to test, with no binary edit for Steam to object to.
#
# One launch is not a result, which is the whole point of this script. It also
# reports per module rather than per run, because "the map repeated" and "the
# exe repeated but Steam's modules did not" are different answers and only the
# first one makes cross-session restore easy.
#
#   ./tools/baserun.sh -n 6
#
# Safe to iterate on: the software rasteriser is what runs here (D3D11SW_GPU=0),
# so a bad launch costs a process, not a display driver. That is the difference
# that makes a sweep like this reasonable on Linux and not on Windows.
set -euo pipefail
cd "$(dirname "$0")/.."

N=6
START_TIMEOUT=120
RUN_TIMEOUT=240
APPID=400910
SAVE_AT=240
QUIT_AT=600

while [ $# -gt 0 ]; do
	case "$1" in
	-n) N="$2"; shift 2 ;;
	*) echo "usage: $0 [-n launches]" >&2; exit 1 ;;
	esac
done

STEAM_ROOT="${STEAM_ROOT:-$HOME/.steam/debian-installation}"
GAME="${RABI_DIR:-$STEAM_ROOT/steamapps/common/Rabi-Ribi}"
[ -d "$GAME" ] || { echo "no Rabi-Ribi at $GAME - set RABI_DIR" >&2; exit 1; }

PROTON="${RABI_PROTON:-$STEAM_ROOT/steamapps/common/Proton - Experimental/proton}"
COMPAT="${STEAM_COMPAT_DATA_PATH:-$STEAM_ROOT/steamapps/compatdata/400910}"
WRAP="$STEAM_ROOT/ubuntu12_32/steam-launch-wrapper"
REAPER="$STEAM_ROOT/ubuntu12_32/reaper"
SLR="$STEAM_ROOT/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point"

LOG="$GAME/d3d9_sw_savestate_rabiribi.txt"
CFG="$GAME/d3d9_sw.cfg"
KEEP="det/base-linux"
mkdir -p "$KEEP"
# A leftover launch*.txt from a crashed earlier sweep would mix into the answer.
rm -f "$KEEP"/launch*.txt

# Windows LaunchOptions is -xaudio2. This VM has no sound device (no /dev/snd,
# no Pulse), so that flag dies before d3d11.dll attaches. -noaudio is the
# playable Proton path. Keep Steam LaunchOptions empty so the Linux client
# does not prompt; pass -noaudio on the exe line instead of through steam://.
[ -x "$WRAP" ] && [ -x "$REAPER" ] && [ -x "$SLR" ] && [ -x "$PROTON" ] \
	|| { echo "steam-launch-wrapper/reaper/proton/SLR missing" >&2; exit 1; }
export DISPLAY="${DISPLAY:-:1}"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_ROOT"
export STEAM_COMPAT_DATA_PATH="$COMPAT"
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3d11,dxgi,dsound,xaudio2_9,xinput1_4=n,b}"
ATTACH="$GAME/d3d11_sw.log"

# The module table is only printed when a save is taken, so the sweep needs one.
# QUIT_AT then ends every launch at the same frame - a run that stops whenever
# somebody reaches for the window is not comparable to the one before it.
cfg_set() {
	local key="$1" val="$2"
	touch "$CFG"
	# Rewrite in place rather than appending. Appending is how the Windows cfg
	# ended up with two knobs sharing a line, because the file had no trailing
	# newline and nobody noticed until a restore refused for no stated reason.
	grep -v "^${key}=" "$CFG" >"$CFG.tmp" 2>/dev/null || true
	printf '%s=%s\n' "$key" "$val" >>"$CFG.tmp"
	mv "$CFG.tmp" "$CFG"
}

# Always put the triggers back. xrestore.ps1 shipped without this once, and the
# next ordinary launch tried to restore a slot nobody had asked for.
restore_cfg() {
	cfg_set D3D9SW_SAVE_AT 0
	cfg_set D3D9SW_QUIT_AT 0
	echo "cfg: SAVE_AT and QUIT_AT put back to 0"
}
trap restore_cfg EXIT

# Wine's process args look like S:\steamapps\common\Rabi-Ribi\rabiribi.exe.
# pgrep -f rabiribi.exe also matches steam-launch-wrapper / this script, so a
# sweep would think the game was still up after QUIT_AT and then pkill itself.
running() {
	ps -eo args= | grep -E '[\\]rabiribi\.exe( |$)' | grep -v grep >/dev/null
}

kill_game() {
	ps -eo pid,args= | awk '/[\\]rabiribi\.exe( |$)/ && !/awk/ {print $1}' \
		| xargs -r kill 2>/dev/null || true
}

# A wine process that exists for a second is not a session. The first sweep
# treated Proton's boot stub as "started" then "exited" and reported 0 of 0
# modules moved. The module table is in the savestate log, which only exists
# after our d3d11.dll has attached and taken a save.
attached() {
	running || return 1
	[ -f "$ATTACH" ] || return 1
	[ -f "$KEEP/.t0" ] || return 1
	local t0 mt
	t0=$(cat "$KEEP/.t0")
	mt=$(stat -c %Y "$ATTACH")
	[ "$mt" -ge "${t0%.*}" ] || return 1
	grep -q 'process attach' "$ATTACH" 2>/dev/null
}

wait_until_attached() {
	local t=0
	while [ "$t" -lt "$START_TIMEOUT" ]; do
		if attached; then return 0; fi
		sleep 1
		t=$((t + 1))
	done
	return 1
}

wait_for() { # wait_for <seconds> <predicate-true-means-done>
	local t=0
	while [ "$t" -lt "$1" ]; do
		if "$2"; then return 0; fi
		sleep 1
		t=$((t + 1))
	done
	return 1
}
not_running() { ! running; }

running && { echo "the game is already running; close it first" >&2; exit 1; }

cfg_set D3D9SW_SAVE_AT "$SAVE_AT"
cfg_set D3D9SW_QUIT_AT "$QUIT_AT"
cfg_set D3D11SW_GPU 0

for i in $(seq 1 "$N"); do
	printf '\n=== launch %d of %d ===\n' "$i" "$N"
	rm -f "$LOG"
	date +%s >"$KEEP/.t0"
	# Not steam://: LaunchOptions in the client is how Linux Steam pops the
	# confirm dialog, and -xaudio2 is not playable here (no sound device).
	"$WRAP" -- "$REAPER" SteamLaunch AppId="$APPID" -- \
		"$SLR" --verb=waitforexitandrun -- \
		"$PROTON" waitforexitandrun "$GAME/rabiribi.exe" -noaudio \
		>/dev/null 2>&1 &

	if ! wait_until_attached; then
		echo "  never started, skipping"
		kill_game
		continue
	fi
	echo "  started (wrapper attached)"

	if ! wait_for "$RUN_TIMEOUT" not_running; then
		echo "  did not close itself, ending it"
		kill_game
		sleep 2
	fi
	# Let wineserver finish tearing down so the next launch is a real process,
	# not a half-dead prefix.
	sleep 5

	# Archived before parsing, not after. The Windows sweep deleted each trace
	# at the top of the loop and read it at the bottom; when the read failed,
	# five launches of evidence were already gone.
	if [ -f "$LOG" ]; then
		cp -f "$LOG" "$KEEP/launch$i.txt"
		printf '  kept %s (%s module line(s))\n' "$KEEP/launch$i.txt" \
			"$(grep -c ' KB at ' "$LOG" || true)"
	else
		echo "  no log written - the save never happened, so there is no module table"
	fi
done

# ---------------------------------------------------------------- the answer

printf '\n=== module bases across %d launch(es) ===\n' "$N"

# One name/base pair per module per launch, deduped within a launch because a
# session that saves more than once prints the table more than once.
pairs() {
	local f
	for f in "$KEEP"/launch*.txt; do
		[ -f "$f" ] || continue
		grep -oE 'module [^ ]+ +[0-9]+ KB at [0-9A-F]{8}' "$f" |
			sed -E 's/module ([^ ]+) +[0-9]+ KB at ([0-9A-F]{8})/\1 \2/' |
			sort -u
	done
}

pairs | sort -u | awk '
{ n[$1]++; if (b[$1] == "") b[$1] = $2; else b[$1] = b[$1] ", " $2 }
END {
	for (m in n) printf "%-26s %d  %s\n", m, n[m], b[m]
}' | sort -k2,2rn -k1,1 | while read -r line; do
	# Anything with more than one distinct base is a mover, and the movers are
	# the entire subject. Reported first, by the sort above.
	count=$(echo "$line" | awk '{print $2}')
	if [ "${count:-1}" -gt 1 ]; then
		printf '  MOVED  %s\n' "$line"
	else
		printf '  fixed  %s\n' "$line"
	fi
done

printf '\n'
movers=$(pairs | sort -u | awk '{n[$1]++} END {c=0; for (m in n) if (n[m]>1) c++; print c}')
total=$(pairs | sort -u | awk '{n[$1]=1} END {print length(n)}')
printf '  %s of %s module(s) moved across the sweep\n' "$movers" "$total"

if [ "${movers:-1}" = "0" ] && [ "$N" -gt 2 ]; then
	cat <<'EOF'

  Nothing moved. If rabiribi.exe is among those - and on Windows it never was -
  then the address-space reason cross-session restore fails there does not apply
  here, and the next thing to try is the restore itself:

      D3D9SW_SLOTFILE=1, save in one launch, D3D9SW_LOAD_AT in the next.

  Worth repeating after a reboot before trusting it. Windows chooses per boot,
  and a sweep inside one boot cannot tell a fixed base from a boot-stable one.
EOF
fi
