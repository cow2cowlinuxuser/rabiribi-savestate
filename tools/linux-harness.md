# Linux Proton harness notes (reautomation)

What this Cloud VM actually used, so a local harness can match it and
then drive the same scripts here. Not a switch away from Windows: the
question is whether cross-session restore is easier under Proton.

## Steam launch

- Start with `steam-launch-wrapper` and **no extra exe args**
  (`tools/linux-rabi.sh launch`, `tools/baserun.sh`). Extra args, or
  `steam://run/400910//-noaudio`, pop "Launch Game with custom arguments".
- Steam Launch Options may still be `-noaudio`; Steam then applies it as
  the official command and does not prompt. The dialog is injected URL
  args, not the saved Launch Options field.
- `RABI_NOAUDIO=1` puts `-noaudio` on the wrapper argv and will re-open
  the dialog on Linux Steam.
- Windows Launch Options is `-xaudio2`. This VM has no `/dev/snd`; that
  flag exits before `d3d11.dll` attaches.

## Scripts

```
tools/linux-rabi.sh build     # zig cc -target x86-windows-gnu
tools/linux-rabi.sh deploy    # DLLs + examples/rabiribi cfgs + native DllOverrides
tools/linux-rabi.sh status    # also prints `ps` (pid / window / Steam / slot)
tools/linux-rabi.sh ps        # one-shot running-state snapshot
tools/linux-rabi.sh launch    # wrapper, no extra argv; no custom-args dialog
tools/linux-rabi.sh xrestore [frame [quit]]
./tools/baserun.sh -n 6       # module bases across launches
```

`wrapper.sh` / `xrestore.sh` are thin names for `linux-rabi.sh`.
`noaslr.py` pins wrapper image bases.

`baserun.sh` sets `D3D9SW_SAVE_AT=240` and `D3D9SW_QUIT_AT=600` (module
table only prints on a save), archives each log under `det/base-linux/`
**before** parsing, and an EXIT trap puts the knobs back to 0. It waits
until `d3d11_sw.log` shows a fresh `process attach`, not Proton's boot stub.

## Cfg next to the exe

Deployed from `examples/rabiribi/`. The live knobs that matter here:

```
D3D11SW_GPU=0
D3D9SW_THREADS=4
D3D9SW_XINPUT=0
D3D9SW_KEY_UNSTICK=1
D3D9SW_SLOTFILE=1
D3D9SW_SAVE_AT=0
D3D9SW_LOAD_AT=0
D3D9SW_QUIT_AT=0
```

Prefix `DllOverrides` native for `d3d11,dxgi,dsound,xaudio2_9,xinput1_4`
(otherwise DXVK wins). `WINEDLLOVERRIDES` is set the same way in the scripts.

## What already ran on this VM

- In-session F5 / Shift+F5 room-to-room: `logs/linux-proton-room-to-room.zip`
- Two Continue-clicked baserun launches (2 and 4 of a botched six):
  `logs/linux-proton-baserun-partial.zip`. Same map both times, including
  `rabiribi.exe` at `00400000` (the preferred base Windows never grants).
- One title save and repeated restores of it: `tools/linux-rabi.sh cycle 3`
  after `launch`. It needs `D3D9SW_LOAD_VK`: shift plus the save key does not
  survive this VM's input path, so load simply never fired.
- Exactly two restores hold. Six sessions in six: restores one and two keep
  presenting, the third always stops. No crash - the process stays up with one
  thread at 100% of a core, the wrapper log stops mid-frame, and the window
  keeps showing the last frame it drew. That last part is why the run waits for
  three `present:` lines and never looks at a screenshot: a wedged game
  photographs as a perfectly good title screen, and a pid proves nothing.
- A stuck Left after an in-game restore is the keyboard, not the pad.
  `D3D9SW_XINPUT=0` already pins every XInput slot absent. DirectInput is held
  in the present while the game's key buffer is rewound, so a Left that was
  down at save time never sees a KEYUP. `D3D9SW_KEY_UNSTICK=1` injects one
  after resume (extended scan code, or it releases numpad 4 instead). The
  cycle script also XTEST-keyups Left/Right/Up/Down after every press, because
  `xdotool keyup --window` is XSendEvent and Wine can miss it. Linux `xinput`
  is not installed on this VM and is not required for the release.
- That only came clean once the restore stopped rewinding regions inside a
  heap it had already decided to hold (`D3D9SW_PARTHOLD`, on by default).
  Before it, 2 of 7 sessions survived; the rest died at `ntdll+50260` writing
  `7FFFFFF8` / `7FFFFFFF` / `00000004`, a list unlink with one half from the
  save and one from the present.
- Next experiment, now that repeated in-session restores hold: `D3D9SW_SLOTFILE=1`,
  save in one launch, `D3D9SW_LOAD_AT` in the next. Repeat after a reboot
  before treating a fixed base as boot-stable.
