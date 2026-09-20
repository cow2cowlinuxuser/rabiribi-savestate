#!/usr/bin/env bash
#
# Does snap containment touch a single byte of the game's address space?
#
# The claim under test is "0 contiguous blocks affected by the snap containment
# or handling at all". That is a falsifiable statement about layout, and this
# script is what falsifies it or lets it stand.
#
# The theory it challenges says a snap scope is a cgroup: accounting plus a kill
# boundary, with no allocator of its own and no say in where anything lands.
# If that is true the game's region list must be byte-identical whether Steam
# came from the snap or from a package outside it, because the same Proton
# builds the same address space either way. If it is false the diff prints the
# blocks that moved and the theory is finished - which is the outcome worth
# having, and the reason this compares rather than asserts.
#
# What is compared is the .regions sidecar, not /proc/pid/maps. The sidecar is
# what the restore actually consumes: it names every captured region with its
# base, size, allocation base and type. A block that moved between launches is
# a block that would have made a cross-session restore refuse or straddle, so
# this measures the thing that matters instead of a thing nearby.
#
#   ./tools/snap-vs-host.sh --snap    -n 3    # inside the snap scope
#   ./tools/snap-vs-host.sh --host    -n 3    # a Steam outside it
#   ./tools/snap-vs-host.sh --compare                # the answer
#
# Run the two halves on the same boot. Wine's layout is near-deterministic
# within a boot and can shift across one, so a reboot in the middle would put a
# difference in the diff that has nothing to do with snap and would read as a
# hit. That confound is the whole reason the modes are separate invocations.
#
# D3D11SW_GPU=0 throughout: the software rasteriser means a bad launch costs a
# process rather than a display driver, and the adapter is not asked to place
# anything, so the comparison is about the game and Wine alone.

set -euo pipefail
cd "$(dirname "$0")/.."

MODE=""
N=3
START_TIMEOUT=120
RUN_TIMEOUT=240
APPID=400910
# The region table is only written when a save is taken, so the sweep needs
# one, and QUIT_AT ends every launch at the same frame. A run that stops
# whenever somebody reaches for the window is not comparable to the one before
# it - and here that matters twice over, because an uneven frame count would
# show up as moved blocks and be read as snap's doing.
SAVE_AT=240
QUIT_AT=600

while [ $# -gt 0 ]; do
	case "$1" in
	--snap) MODE=snap; shift ;;
	--host) MODE=host; shift ;;
	--compare) MODE=compare; shift ;;
	-n) N="$2"; shift 2 ;;
	*) echo "usage: $0 --snap|--host|--compare [-n launches]" >&2; exit 1 ;;
	esac
done
[ -n "$MODE" ] || { echo "pick --snap, --host or --compare" >&2; exit 1; }

KEEP="det/snap-vs-host"
mkdir -p "$KEEP"

# ------------------------------------------------------------------ comparing
#
# Sorted because the save walks the address space in whatever order it walks
# it, and two launches that captured the same blocks in a different order are
# the same address space. Sorting removes an ordering difference that would
# otherwise be indistinguishable from a placement one.
normalise() { grep -v '^#' "$1" | sort; }

if [ "$MODE" = compare ]; then
	shopt -s nullglob
	snap=("$KEEP"/snap-*.regions)
	host=("$KEEP"/host-*.regions)
	shopt -u nullglob
	[ ${#snap[@]} -gt 0 ] || { echo "no snap captures in $KEEP - run --snap first" >&2; exit 1; }
	[ ${#host[@]} -gt 0 ] || { echo "no host captures in $KEEP - run --host first" >&2; exit 1; }

	# Within-group first. Two launches of the SAME Steam that already disagree
	# set the noise floor, and a cross-group difference smaller than that floor
	# is not evidence of anything. Skipping this step is how ordinary launch
	# variance gets published as a snap effect.
	echo "=== within-group variance (the noise floor) ==="
	for grp in snap host; do
		files=("$KEEP"/$grp-*.regions)
		if [ ${#files[@]} -lt 2 ]; then
			printf '%s: only %d capture(s), no floor measurable\n' "$grp" "${#files[@]}"
			continue
		fi
		base="${files[0]}"
		for f in "${files[@]:1}"; do
			d=$(diff <(normalise "$base") <(normalise "$f") | grep -c '^[<>]' || true)
			printf '%s: %s vs %s - %d differing line(s)\n' \
				"$grp" "$(basename "$base")" "$(basename "$f")" "$d"
		done
	done

	echo
	echo "=== snap vs host ==="
	a="${snap[0]}"
	b="${host[0]}"
	printf 'comparing %s with %s\n\n' "$(basename "$a")" "$(basename "$b")"
	if diff -q <(normalise "$a") <(normalise "$b") >/dev/null; then
		echo "IDENTICAL - 0 blocks differ. Snap containment placed nothing."
		exit 0
	fi
	diff <(normalise "$a") <(normalise "$b") | sed -n '1,60p'
	n=$(diff <(normalise "$a") <(normalise "$b") | grep -c '^[<>]' || true)
	printf '\n%d differing line(s). Compare against the floor above before\n' "$n"
	printf 'calling any of it snap: a difference at or below the floor is\n'
	printf 'ordinary launch variance wearing snap-coloured clothes.\n'
	exit 1
fi

# -------------------------------------------------------------------- running
#
# Two Steams, one box. RABI_DIR and the launcher differ; everything downstream
# is identical on purpose, because every uncontrolled difference is a false
# positive waiting to be attributed to containment.
if [ "$MODE" = snap ]; then
	GAME="${RABI_SNAP_DIR:-$HOME/snap/steam/common/.local/share/Steam/steamapps/common/Rabi-Ribi}"
	launch() { HOME="$HOME/snap/steam/common" steam-runtime-steam-remote "steam://rungameid/$APPID" >/dev/null 2>&1 & }
else
	GAME="${RABI_HOST_DIR:-$HOME/.steam/debian-installation/steamapps/common/Rabi-Ribi}"
	launch() { steam "steam://rungameid/$APPID" >/dev/null 2>&1 & }
fi
[ -d "$GAME" ] || { echo "no Rabi-Ribi at $GAME - set RABI_SNAP_DIR or RABI_HOST_DIR" >&2; exit 1; }

LOG="$GAME/d3d9_sw_savestate_rabiribi.txt"
REGIONS="$GAME/d3d9sw_slot0.regions"
CFG="$GAME/d3d9_sw.cfg"

cfg_set() {
	local key="$1" val="$2"
	touch "$CFG"
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

running() { pgrep -x 'rabiribi.exe' >/dev/null 2>&1; }
not_running() { ! running; }

wait_for() {
	local t=0
	while [ "$t" -lt "$1" ]; do
		if "$2"; then return 0; fi
		sleep 1
		t=$((t + 1))
	done
	return 1
}

running && { echo "the game is already running; close it first" >&2; exit 1; }

cfg_set D3D9SW_SAVE_AT "$SAVE_AT"
cfg_set D3D9SW_QUIT_AT "$QUIT_AT"
cfg_set D3D11SW_GPU 0
cfg_set D3D9SW_SLOTFILE 1

printf 'mode %s, %d launch(es), game at %s\n' "$MODE" "$N" "$GAME"

for i in $(seq 1 "$N"); do
	printf '\n=== %s launch %d of %d ===\n' "$MODE" "$i" "$N"
	rm -f "$LOG" "$REGIONS"
	launch

	if ! wait_for "$START_TIMEOUT" running; then
		echo "  never started, skipping"
		continue
	fi
	echo "  up as pid $(pgrep -x 'rabiribi.exe' | head -1)"

	if ! wait_for "$RUN_TIMEOUT" not_running; then
		echo "  still running past QUIT_AT, stopping it"
		pkill -x 'rabiribi.exe' || true
		sleep 3
	fi

	if [ -s "$REGIONS" ]; then
		cp "$REGIONS" "$KEEP/$MODE-$i.regions"
		printf '  kept %s (%d region line(s))\n' "$KEEP/$MODE-$i.regions" \
			"$(grep -vc '^#' "$KEEP/$MODE-$i.regions" || true)"
	else
		echo "  no .regions written - the save never happened"
	fi
done

printf '\ncaptured. Run both modes on this boot, then: %s --compare\n' "$0"
