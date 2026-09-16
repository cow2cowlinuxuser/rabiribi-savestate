#!/usr/bin/env bash
#
# Set up WSL2 to build the wrappers and run the savestate engine under Wine.
#
# Two paths, deliberately kept apart, because they cost very different things:
#
#   harness   No Steam, no login, no game, no window. Builds the PE32 harnesses
#             and runs them on a virtual framebuffer. This is where the freeze
#             reproduces - all five Wine sessions that stopped mid-save did so
#             without Rabi-Ribi being involved - so it is the path that answers
#             the question, and it needs nothing from Valve.
#
#   game      Steam, a login, the real game, a real window. Only needed to watch
#             Rabi-Ribi itself. Slower to set up and more to go wrong.
#
# Start with the harness. Reach for the game path when there is something to
# look at.
#
#   ./tools/wsl-setup.sh deps        # zig, 32-bit wine, xvfb
#   ./tools/wsl-setup.sh harness     # build, then run rr_harness32 headless
#   ./tools/wsl-setup.sh steam       # install Steam and log in by QR
#   ./tools/wsl-setup.sh game        # deploy the wrappers next to Rabi-Ribi
#   ./tools/wsl-setup.sh status      # what is present and what is not
set -euo pipefail
cd "$(dirname "$0")/.."

say() { printf '\n== %s ==\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# The cross-build lives on the Linux branch, not on main. Saying so by name
# beats letting ./build.sh fail with "no such file" and leaving the reader to
# work out which of several plausible things went wrong.
need_build_sh() {
	[ -x ./build.sh ] || die "no ./build.sh in this checkout - it arrives with the\
 Linux dev-environment branch (PR #2, cursor/linux-wine-dev-env-e911). Merge or\
 cherry-pick that first."
}

in_wsl() { grep -qi microsoft /proc/version 2>/dev/null; }

# WSLg is the reason this script does not ship an X server for the game path.
# Windows 11 runs a Wayland compositor for WSL and plumbs it in at
# /mnt/wslg, so a Linux GUI program opens in a window on the Windows desktop
# with no VcXsrv, no DISPLAY export and no X11 forwarding. On Windows 10 none
# of that exists and the game path needs a third-party X server instead.
has_wslg() { [ -d /mnt/wslg ]; }

# ------------------------------------------------------------------ deps

cmd_deps() {
	in_wsl || warn "this does not look like WSL - the steps still work on a\
 normal Debian or Ubuntu box, only the WSLg notes will not apply"

	say "32-bit architecture"
	# Everything this project builds is PE32, and Wine can only load PE32 if the
	# host has i386 libraries. This single line is what people miss most often,
	# and the failure it causes - wine reporting a perfectly valid exe as not
	# executable - does not point at its own cause.
	sudo dpkg --add-architecture i386
	sudo apt-get update -qq

	say "wine (32-bit), a virtual X server, and the build's own needs"
	sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		wine wine32:i386 wine64 xvfb curl xz-utils file python3
	wine --version

	say "zig"
	if ! command -v zig >/dev/null 2>&1; then
		if [ -x .cursor/install.sh ]; then
			# It already installs the version the build is tested against, and
			# having two scripts disagree about that is worse than a dependency.
			./.cursor/install.sh
			return
		fi
		die "zig is not installed and .cursor/install.sh is not in this checkout.\
 Install zig 0.14.1 from https://ziglang.org/download/ and put it on PATH."
	fi
	zig version

	say "wine prefixes"
	export WINEDEBUG="${WINEDEBUG:--all}"
	[ -f "$HOME/.wine-rabiribi/system.reg" ] ||
		WINEPREFIX="$HOME/.wine-rabiribi" WINEARCH=win32 wineboot --init >/dev/null 2>&1 || true
	[ -f "$HOME/.wine-rabiribi64/system.reg" ] ||
		WINEPREFIX="$HOME/.wine-rabiribi64" WINEARCH=win64 wineboot --init >/dev/null 2>&1 || true

	echo
	echo "deps done. Next:  ./tools/wsl-setup.sh harness"
}

# ------------------------------------------------------------------ harness

cmd_harness() {
	need_build_sh
	say "build"
	./build.sh

	say "run"
	# Arguments match run.sh's documented example: all four conditions, one
	# cycle. The log is the point, not the exit status, so the run is allowed to
	# fail without taking the script down - a hang killed by hand still leaves
	# the breadcrumb that says where.
	./run.sh rr_harness32.exe 2 1 || warn "the harness did not exit cleanly - read\
 the log anyway, that is what it is for"

	say "where the save stopped"
	local log=d3d9_sw_savestate_rr_harness32.txt
	if [ -f "$log" ]; then
		# Each stage announces itself before it runs, so the last one printed is
		# the call that did not come back. Everything else in this file is
		# settings, which is why it is filtered rather than tailed.
		grep 'save stage:' "$log" | tail -n 12 || true
		echo
		echo "the last line above is the call that did not return."
		# The trailing true matters: not finding the line is the expected
		# result, and under set -e a bare failing grep would end the script
		# one line before it says anything useful.
		grep -q 'savestate saved' "$log" &&
			echo "...except it saved. The freeze is gone on this configuration." || true
	else
		warn "no $log was written - the engine did not get as far as opening it"
	fi
}

# ------------------------------------------------------------------ steam

cmd_steam() {
	say "Steam"
	if ! command -v steam >/dev/null 2>&1; then
		sudo DEBIAN_FRONTEND=noninteractive apt-get install -y steam-installer ||
			die "steam-installer is not available. On Debian enable non-free, or\
 fetch the .deb from https://store.steampowered.com/about/"
	fi

	has_wslg || die "no /mnt/wslg, so there is nowhere to draw the login window.\
 WSLg ships with Windows 11; on Windows 10 install an X server on the Windows\
 side and export DISPLAY before running this."

	cat <<'EOF'

Log in with the QR code, not a password.

Steam's login window offers "Sign in with Steam app" - scan the square with the
Steam mobile app and the session is authorised from the phone. Nothing is typed
on this machine and no password is stored on it.

What is stored afterwards is a refresh token, in
~/.steam/steam/config/config.vdf. That is not a password: it only works for
this one machine, and it can be revoked from the phone under
Steam Guard -> Authorised Devices. Subsequent launches reuse it, so the QR is
a first-run step rather than a routine one.

EOF
	read -r -p "press enter to open Steam..." _
	steam
}

# ------------------------------------------------------------------ game

cmd_game() {
	local rabi
	rabi="${RABI_DIR:-$HOME/.steam/debian-installation/steamapps/common/Rabi-Ribi}"
	[ -d "$rabi" ] || die "no Rabi-Ribi at $rabi - install it in Steam first, or\
 set RABI_DIR to where it landed"

	need_build_sh
	say "build"
	./build.sh

	say "deploy"
	# All four move together or none do. A folder holding two builds of the
	# savestate engine is the split-engine failure, and it presents as the game
	# not reaching its first frame rather than as anything that names a DLL.
	local n
	for n in d3d11 dxgi xaudio2_9 xinput1_4; do
		[ -f "x86/$n.dll" ] || die "x86/$n.dll was not built - deploying a partial\
 set is how the engine ends up mismatched against itself"
	done
	for n in d3d11 dxgi xaudio2_9 xinput1_4; do
		cp -f "x86/$n.dll" "$rabi/$n.dll"
		echo "  $n.dll"
	done

	cat <<'EOF'

Two things Steam still needs told, in the game's Properties:

  Launch options   WINEDLLOVERRIDES="d3d11,dxgi,xaudio2_9,xinput1_4=n,b" %command%

The overrides are what make Proton load our DLLs instead of its own. Without
them the wrappers sit in the folder and nothing calls them, which looks
exactly like the wrappers not working.

And set D3D11SW_GPU=0 in d3d9_sw.cfg before any cross-session restore. On
Windows that is a hard rule - a restore against the live backend hands the
driver a device from a process that has exited, and an evening of that ended
in a bluescreen. Under Wine the blast radius is smaller, but the software
rasteriser is verified correct here (see logs/test_seam.log) so there is
nothing to gain by risking it.
EOF
}

# ------------------------------------------------------------------ status

cmd_status() {
	local w
	printf '%-14s %s\n' "wsl" "$(in_wsl && echo yes || echo 'no (or not detected)')"
	printf '%-14s %s\n' "wslg" "$(has_wslg && echo 'yes, GUI apps will open' ||
		echo 'no, headless only')"
	for w in zig wine steam xvfb-run; do
		printf '%-14s %s\n' "$w" "$(command -v $w >/dev/null 2>&1 &&
			echo "$(command -v $w)" || echo '-')"
	done
	printf '%-14s %s\n' "i386" "$(dpkg --print-foreign-architectures 2>/dev/null |
		grep -qx i386 && echo enabled || echo 'NOT enabled - wine cannot load PE32')"
	printf '%-14s %s\n' "build.sh" "$([ -x ./build.sh ] && echo present ||
		echo 'absent (merge PR #2)')"
	printf '%-14s %s\n' "harnesses" "$(ls rr_harness32.exe ss_harness32.exe 2>/dev/null |
		tr '\n' ' ' || echo '- (run: harness)')"
}

# ------------------------------------------------------------------

case "${1:-}" in
deps) cmd_deps ;;
harness) cmd_harness ;;
steam) cmd_steam ;;
game) cmd_game ;;
status) cmd_status ;;
*)
	sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac
