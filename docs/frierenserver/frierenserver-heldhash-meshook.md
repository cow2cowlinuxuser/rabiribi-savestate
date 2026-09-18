---
cursor:
  subagentId: "bc-be4b3cf0-0634-51a4-89e5-669f76c5bd2d"
---

# frierenserver held-heap hash + MesHook excision + pos file (2026-09-18)

Unattended. `steam://rungameid/400910` only. Wrappers left in the game dir
(replaced `d3d11.dll` with this build, pinned at `60000000`). `D3D11SW_GPU=0`
`D3D9SW_XINPUT=0` `PARTHOLD=1` `EXCLSKIP=1`. `SAVE_AT`/`LOAD_AT`/`QUIT_AT`
disarmed. Launch argv still `rabiribi.exe -nosound`. No Windows xrestore.

Local commits on `cursor/belongs-elsewhere-coalesce-bd2d`: `93dd9bd` then
`c5f28e6` (Wine handle-page fallback). Push still has no GitHub credentials.

## Named outcomes

**1. MesHook can be refused and the game still runs.**
`D3D9SW_NOMESHOOK=1` patched the executable IAT from DllMain. Process A
(linux **59326**) and process B (linux **60678**) both had **no**
`ddxx_MesHoooooook.dll` in `/proc/pid/maps`. CONTINUE into Rabi Rabi Beach
worked. The savestate log says `meshook: not loaded (NOMESHOOK=1)`.

**2. Held-heap hash: we did not paint the process heap; then B died of a
bad function pointer.**
KEY_1 did not write a new slot. KEY_2 loaded the Sep 17 belong slot
(`d3d9sw_slot0.bin` md5 `eb359671…`, 384.5 MB, "loaded a slot taken in
another session"). That is a cross-session transplant with MesHook already
gone.

```
  held hash before the copy: heap 00150000  64 KB  742BF3D1  process heap
  held hash before the copy: heap 0B400000  1088 KB  0574B16B  … this wrapper's own heap
  held hash after the copy, still frozen: … 742BF3D1 -> 742BF3D1  UNCHANGED (process heap)
  held hash after the copy, still frozen: … 0574B16B -> 0574B16B  UNCHANGED
  VERDICT 2 held heap(s) UNCHANGED across the copy - we did not paint them;
          a later death is foreign handles
load: slot 0, 131 restored, 0 skipped, 0 by block, 34 threads, 0 newer than save
fault: C0000005 at 00000160 … 0 frame(s) since the last restore
       VERDICT: call through a bad function pointer … return address of 00000000
```

Audio, GPU, pads, **and** MesHook are now all excised and all still on the
wrong side of this death. The copy completed. The held heaps did not change
while frozen. The crash is the window/WNDPROC chain, not a heap we claimed to
hold.

**3. Cross-session is a native CONTINUE plus eight bytes, and that path
lives.**
F11 in A wrote `d3d9sw_pos.bin` (`pos: marked x=26044 y=8193 on world 1`).
A died on the slotfile load. B CONTINUE AUTO SAVE landed mapid 1 (load
boundary `8 -> 4 -> 8 -> 1`). Shift-F11:

```
pos: restored x=26044 y=8193 (was x=26174 y=8257), 8 byte(s), world 1
```

B (linux **60678**) is still up, still without MesHook, still on the beach.

**4. POS_SPAN 32 also restores.** In B, after CONTINUE: marked 32 bytes at
`x=26086 y=8213`, walked, restored to those coords (`was x=26103 y=8221`).
No crash. Did not run the ladder to failure.

## What this is not

Excision cannot make the transplant work. The irreducible set — a window, a
message pump, a process heap — is where the slotfile death still lives, and
the hash says we are not the ones painting that heap. In-session memory
snapshot (PARTHOLD) remains the frame-exact tool. Cross-session is CONTINUE
plus a pos-style patch.

## Left on disk

Game dir: new `d3d11.dll` (693248, md5 `849a4284…`), `NOMESHOOK=1`
`HELDHASH=1` `POSFILE=1` `POS_SPAN=8`. Slotfile moved aside to the pack so
KEY_2 cannot repeat the transplant by accident. B still running.

## Paths

- Report: `/home/fernserver/Downloads/frierenserver-heldhash-meshook.md`
- Pack: `/home/fernserver/Downloads/frierenserver-heldhash-meshook/`
- Harness (Proton wine, synth, 2 save/2 restore, 0 bad): `logs/ss_harness32-heldhash.txt`
