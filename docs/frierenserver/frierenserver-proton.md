# frierenserver Proton A/B (2026-09-17)

Laptop `frierenserver`, user `fernserver`, Ubuntu 24.04, DISPLAY=:0, Cursor 3.20.21. Tree: `origin/main` `4a9fa0d`. Did **not** checkout PR #3. `D3D9SW_KEY_UNSTICK` left off.

## Capability

| | |
|---|---|
| Steam | snap `steam` 1.0.0.87 rev 271, already running, logged in |
| Game | Rabi-Ribi appid **400910**, `ver1.65` depot, `rabiribi.exe` PE32 |
| Proton | **Experimental 11.0-100**, prefix `compatdata/400910` |
| LaunchOptions | `-xaudio2`. Audio device exists (PipeWire). `-noaudio` was not required |
| GPU | AMD Renoir iGPU, Mesa 25.2.8, accelerated |
| Zig | 0.15.2. Cloud VM used 0.16 |

`linux-rabi.sh` defaults `STEAM_ROOT=$HOME/.steam/debian-installation`. This box is snap Steam at `$HOME/snap/steam/common/.local/share/Steam`. Needed `STEAM_ROOT` / `RABI_GAME` / `STEAM_COMPAT_DATA_PATH` overrides.

`tools/noaslr.py` is not on main. Used PR #3's pin helper as a one-off; did not take KEY_UNSTICK or other PR #3 savestate patches.

## Launch

`./tools/linux-rabi.sh launch` (no extra exe argv) **failed**:

```
pressure-vessel-wrap: E: Child process exited with code 1: bwrap: setting up uid map: Permission denied
```

Unattended path that worked (still **no extra exe argv**):

```
HOME=/home/fernserver/snap/steam/common \
  steam-runtime-steam-remote steam://rungameid/400910
```

Game: pid **48663**, window `Rabi-Ribi ver 1.65` **1286×752 windowed** (`D3D11SW_BORDERLESS=0` after deploy). Wrapper `process attach`, `swrast: 6 worker threads`, ~57–59 fps on the title.

## SAVE_AT=0 on main still means frame 1

Cfg had `D3D9SW_SAVE_AT=0` `LOAD_AT=0` `QUIT_AT=0`. Main still treated 0 as frame zero. First present froze:

`save-at: frame 1, cut 1 of 1` → slot 0 **166.7 MB** in **263.3 ms**, census `mapid 0` (pre-title).

PR #3 `18086de` (`SAVE_AT=0` means never) is the fix. Not pulled this sitting.

## Title save + one restore

At PRESS START. Hotkeys `SAVE_VK=1` `LOAD_VK=2`. `xdotool --window` is XSendEvent; Wine ignored it. XTEST (`xdotool keydown 1` with the game focused) worked.

| | |
|---|---|
| Title save (key 1) | `save: slot 0, 153 regions, 413.8 MB`, **426.5 ms**, census **mapid 8** |
| Restore (key 2) | `load: slot 0, 148 restored, 0 skipped`, `witness: player IS back at x=0 y=0, world 8`, `resume: done`, clock wound **46.8 s** |
| After restore | same pid **48663**, PRESS START still drawn, presents kept coming (**~57 fps**). Not a hung last-frame screenshot |
| Second/third restore | **not attempted** (cloud Proton VM: third restore always hangs) |

Clobber 100 ms later: 21 of 153 regions differ, 2 REVERTED. Process stayed alive.

## Leftover Left / dinput vs Windows

Both the accidental frame-1 save and the title save logged the same Proton freeze split:

```
wine: froze 34 game/mixer thread(s), left 3 wineserver-side thread(s) running
those threads belong to: dinput.dll x1, mmdevapi.dll x1, XAudio2_8.dll x1
rewound threads belong to: rabiribi.exe x34
```

That **matches the cloud Proton VM** and **differs from Windows** (Windows freezes dinput with everyone else; 400 ms frames did not stick Left). No leftover walk at title, so KEY_UNSTICK stayed on the shelf.
