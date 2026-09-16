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

GAME="${RABI_DIR:-$HOME/.steam/debian-installation/steamapps/common/Rabi-Ribi}"
[ -d "$GAME" ] || { echo "no Rabi-Ribi at $GAME - set RABI_DIR" >&2; exit 1; }

LOG="$GAME/d3d9_sw_savestate_rabiribi.txt"
CFG="$GAME/d3d9_sw.cfg"
KEEP="det/base-linux"
mkdir -p "$KEEP"

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

running() { pgrep -f 'rabiribi\.exe' >/dev/null 2>&1; }

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
	steam "steam://rungameid/$APPID" >/dev/null 2>&1 &

	if ! wait_for "$START_TIMEOUT" running; then
		echo "  never started, skipping"
		continue
	fi
	echo "  started"

	if ! wait_for "$RUN_TIMEOUT" not_running; then
		echo "  did not close itself, ending it"
		pkill -f 'rabiribi\.exe' || true
		sleep 2
	fi

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
