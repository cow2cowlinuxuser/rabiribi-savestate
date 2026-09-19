---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver GPU ladder + commit-hold (2026-09-19)

Same sitting as the host-commit fill, with `D3D11SW_GPU` walked 0 → 1 → 2 → 3.
`BORDERLESS=0`, `PARTHOLD=1`, `EXCLSKIP=1`, `steam://rungameid/400910` only.
CONTINUE Rabi Rabi Beach. Wrappers stayed the artifact.

`gpu_mode()` in `d3d11_sw.c`: **0** software throughout, **1** adapter presents
the software frame, **2** adapter also draws it, **3** also releases textures
onto the adapter. Pixel sampling is a different knob (`D3D11SW_GPU_HALFPIXEL`).

## Named outcomes

**1. GPU=1 looks like software rast because it still is.**
Log: `gpu: first frame presented through the adapter` plus `swrast: 6 worker
threads`. Pixels still come from the CPU raster. ~60 fps present, but the image
matches GPU=0.

**2. GPU=2/3 actually draw on the adapter.**
`gpu: first frame drawn and presented entirely on the adapter`. In-world
`perf gpu: ~270 draw(s)/frame … 0 declined/frame - the frame is fully covered`.
Play feels better. GPU=3 then: `216 texture(s) released to the adapter, 220.0 MB
no longer in this process`. RSS ~414 MB vs ~630 MB on GPU=1.

**3. In-session restore dies on every GPU>0 path we tried.**
Same death: `C0000005 at ntdll.dll+5025B`, thread 500, a few frames after
`resume: done`. `winevulkan.dll` on the stack-scan. Copy completed (`157
restored`). GPU=0 with PARTHOLD lived two pairs. GPU=1, 2, and 3 all died the
same way under a held host fill (960–1632 MB).

**4. Host commit fill still does not pin the island.**
GPU=3 save: 165 regions / 46 threads. GPU=0 save: 139 regions / 34 threads.
The extra threads park in ntdll. Releasing textures to the adapter makes play
cheaper; it does not make restore live.

## Left on disk

`/home/fernserver/Downloads/frierenserver-commit-hold-gpu/` (gpu1/2/3 pair
indexes, sslogs, shots). Live cfg left at `D3D11SW_GPU=3`.
