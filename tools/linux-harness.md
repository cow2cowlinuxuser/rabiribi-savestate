# Linux Proton harness notes (reautomation)

What this Cloud VM actually used, so a local harness can match it and
then drive the same scripts here. Not a switch away from Windows: the
question is whether cross-session restore is easier under Proton.

## Steam launch

- Appid `400910`, Proton Experimental, `-noaudio` in **Steam Launch Options**.
- Start with `steam://rungameid/400910` only. `tools/baserun.sh` and
  `tools/linux-rabi.sh launch` do that.
- Do **not** pass `-noaudio` on the exe line and do **not** use
  `steam://run/400910//-noaudio`. Steam rewrites that to injected extra
  args and shows "Launch Game with custom arguments" even when Launch
  Options already matches. That dialog has no "don't ask again".
- Windows Launch Options is `-xaudio2`. This VM has no `/dev/snd`; that
  flag exits before `d3d11.dll` attaches.

## Scripts

```
tools/linux-rabi.sh build     # zig cc -target x86-windows-gnu
tools/linux-rabi.sh deploy    # DLLs + examples/rabiribi cfgs + native DllOverrides
tools/linux-rabi.sh status
tools/linux-rabi.sh launch    # steam://rungameid, LaunchOptions supplies -noaudio
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
- Next experiment, once a local harness can launch without the args
  dialog: `D3D9SW_SLOTFILE=1`, save in one launch, `D3D9SW_LOAD_AT` in the next.
  Repeat after a reboot before treating a fixed base as boot-stable.
