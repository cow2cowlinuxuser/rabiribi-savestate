# Where our code starts and the game ends

Linux/Proton on frierenserver. Follow-up launch 2026-09-18: `steam://rungameid/400910` only, no extra exe argv, no save/load, no `/proc/<pid>/mem`, no exe dump. Maps and X11 properties only. Game left running.

Tree: `linux/main` is **not** present. Used `/tmp/rabi-laptop/rabiribi-savestate` `cursor/belongs-elsewhere-coalesce-bd2d` `e8bb9e1` (code at `4ab6262`). Sitting logs: `/home/fernserver/Downloads/frierenserver-crosssession-belong/`. Death naming: `user32-ntdll-crosssession.md`.

**Demonstrated** = this tree + those logs + COFF symbols in Proton Experimental `i386-windows/user32.dll` + live `/proc/56286/maps` and X11. **Inferred** = fits, not shown.

The cross-session question is not “can we copy more bytes.” It is **which of three agents owns each object after resume**. Same-PID restore works because Wine’s USER/GDI objects stay the ones the snapshot was taken against. Cross-PID copies the game’s *names* for those objects into a process whose wineserver tables are new.

**Product:** the drop-in wrappers (`d3d11.dll`, `dxgi.dll`, `xinput1_4.dll`, `xaudio2_9.dll`, optional `dsound.dll`). A player copies them next to the exe. No cdb, no injector, no extra helper. Runtime IAT/GPA/`gameheap` patches are sausage the DLLs make for themselves; the repo keeps the source. `disas_at.ps1` / `cdb -pv` stay developer workflow, not part of the shipped artifact.

Two futures, same boundary — both still ship as those DLLs:

1. Treat USER32/GDI/win32u/wineserver as outside the game. Live with within-session rewind. Cross-PID is a refuse, not a bug to patch by writing more of the process.
2. Peel until the game no longer *is* a USER32 client at restore time: recreate the window after load, or stop rewinding the HWND/HDC/WNDPROC words, or stop pumping messages until that is done. That is conforming the game, not completing the snapshot. It still happens *inside* the wrappers.

---

## 0. Live process (2026-09-18, title, no restore)

linux **56286** `rabiribi.exe -nosound` (exe symlink is `wine-preloader`). wineserver **56199** (sibling under pressure-vessel, not a child of 56286). 40 threads. VmSize ~1.8 GB.

| VA (first page / PE ImageBase) | File | Agent |
| --- | --- | --- |
| `00400000` | `…/Rabi-Ribi/rabiribi.exe` | game |
| `60000000` | `…/Rabi-Ribi/d3d11.dll` | us |
| `61000000` | `…/Rabi-Ribi/dxgi.dll` | us |
| `63000000` | `…/Rabi-Ribi/xinput1_4.dll` | us |
| `78c20000` | `ddxx_MesHoooooook.dll` (DxLib, `%TEMP%`) | game helper, **held** by `module_excluded` |
| `7b690000` | Proton `i386-windows/win32u.dll` + unix `win32u.so` at `f1506000` | Wine |
| `7b6e0000` | `gdi32.dll` | Wine |
| `7b9d0000` | `user32.dll` | Wine |
| `7bf10000` | `ntdll.dll` + unix `ntdll.so` at `f171c000` | Wine |
| `78d50000` | `winex11.drv` + unix `winex11.so` at `ede37000` | Wine |
| `7ac50000` | `steam_api.dll` (game dir) | Steam, **held** |

`xaudio2_9.dll` and our `dsound.dll` are **not mapped** (`-nosound`). Pins still reserved: dxgi’s image is followed by a 32 MB `---` hole through `63000000` (xinput). Wrapper/exe PE **bodies are anonymous** after the named first page — Wine’s usual PE map. `rabiribi.exe` `.text` is `00401000-007a1000 r-xp` **anon**; `.data`-ish `00838000-013d3000 rw-p` **anon**. A `/proc/maps` grep for the filename under-counts (exe showed 4 KB named vs ~19 MB contiguous). Savestate’s module roster is the honest size.

`_WINPROC_wrapper` this process: `7b9d0000+A5CC = 7b9da5cc`, inside live `user32` `r-xp` `7b9d1000-7ba6e000`. Belong death return `7B9DA5E8` is the same preferred base, not a new Proton layout.

**HWND is two numbers:**

| Namespace | Value | Where |
| --- | --- | --- |
| Win32 / wineserver | `00020098` | `d3d11_sw.log` `swapchain … hwnd=` (same digit string as the 2026-09-17 A/B sittings) |
| X11 | `0x7a00003` | `WM_CLASS=steam_app_400910`, `_NET_WM_PID=56286`, 1286×752, `_WINE_HWND_STYLE=0x94c80000` (visible + caption + popup + clip), `_WINE_HWND_EXSTYLE=0x100` (`WS_EX_WINDOWEDGE`) |

No `_WINE_HWND` atom on the X11 window — the Win32 handle is **not** host-visible. Cross-PID restore copies `00020098` as game bytes; the host window `0x7a00003` is a new wineserver object that happens to reuse the same Win32 handle value on sequential launches. **Demonstrated** reuse of `00020098` across 17th and 18th; **not** identity of the server object.

Heaps at the belong addresses still exist as anon rw: `00150000` (process), `0b400000` (wrapper-sized), `0b630000` (game-runtime-sized). Live confirmation of the table in §3, not a restore.

---

## 1. Three agents, not two

| Agent | What it is in this process | How we treat it |
| --- | --- | --- |
| **Us** | Game-dir wrappers (`d3d11.dll` 60000000, `dxgi.dll` 61000000, `xaudio2_9` 62000000, `xinput1_4` 63000000, `dsound.dll`), control block, VEH, IAT/GPA patches, `gameheap` trampolines planted in the exe | Split: `d3d11` **held** (`REWIND_SWHEAP=0`); `dxgi` **rewound** (game-dir, not `self`) |
| **The game** | `rabiribi.exe` at 00400000, DxLib state, entity table on heap `0B630000`, HWND numbers in `.data`, WNDPROC in `.text` | Image + game runtime heap **rewound** |
| **Wine** | `USER32` / `gdi32` / `win32u` / `ntdll` / `winex11.drv`, wineserver (other Linux process), process heap `00150000`, TEBs | Images **held**. Wineserver **never in the slot**. Process heap **policy hold** (`PARTHOLD`) that still failed `HeapValidate` after the belong load |

`module_excluded` (`savestate.c` ~5480): `%windir%` → hold; Steam client tree → hold; `xinput*`/`dinput*`/`xaudio*` by name → hold; anything **not** in the game directory → hold. Positive rule: rewind what shipped next to the exe — except `d3d11.dll` itself, forced held when `D3D9SW_REWIND_SWHEAP=0` (`is_self && !sw_heap_rewound()`, ~10002).

`region_wanted` (~5128): committed `MEM_PRIVATE` or `MEM_IMAGE`, writable, not guard/noaccess, not private-executable, not write-combine/no-cache, not on the exclusion list. **`MEM_MAPPED` dropped.** Device apertures dropped. TEBs/helper stack excluded in `build_exclusions`.

GPU park: `do_load` calls `park_audio_gpu()`; under Wine that logs `not parking xa2/gpu` and returns. **Demonstrated** on both A save and B load.

---

## 2. How our code enters (call graph of *our* hooks)

No USER32 `DispatchMessage` hook. The path that died is Wine’s, calling into the game.

```
DxLib / exe
  LoadLibrary(d3d11.dll)     ← game dir wins
    DllMain PROCESS_ATTACH     d3d11_sw.c
      AddVectoredExceptionHandler   (our VEH)
      hook_getprocaddress           exe IAT KERNEL32!GetProcAddress → hook_gpa
      dsound_claim                  optional LoadLibrary of our dsound.dll
  LoadLibrary(dxgi.dll)
    CreateDXGIFactory* → LoadLibrary(d3d11) GetProcAddress  (dxgi_fwd.c trampoline)
  LoadLibrary(xinput1_4.dll)
    XInputGetState/SetState        pin or forward (xinput_sw.c; this sitting XINPUT=0)
  D3D11CreateDevice / CreateDXGIFactory2
    savestate_hooks_install        exe IAT: CreateEventA, HeapFree, HeapReAlloc,
                                   RtlFreeHeap, Terminate/ExitProcess, QPC/tick/timeGetTime
    software factory/device        our heap 0B400000
  Present (first)
    gameheap_install               trampolines in exe .text for malloc/free/_msize
                                   + KERNEL32!HeapFree IAT floor
    savestate_set_gpu_park         installed; Wine park is still a no-op
    savestate_guard / hotkeys
```

**Our code inside the game image (demonstrated in source, not by dumping the exe this turn):** `gameheap.c` wires five static-CRT sites and patches `HeapFree`. `hook_getprocaddress` rewrites one IAT slot. After a cross-PID restore we **overwrite those patches with A’s copy**. Same-boot, same wrapper pin (60000000), same exe base (00400000) → the trampolines still point at us. **Inferred** harmless. A different wrapper build or base would turn our own hooks into Class A.

`d3d11.dll` itself **imports USER32 and GDI32** (PE import table of the file in the game dir). We are a USER32 client too (`GetDC` in `swrast.c` when GPU=0; `SetWindowPos` / optional `SetWindowLongA(-4)` in `gpu.c` if `D3D11SW_BORDERLESS=1`). This sitting: `BORDERLESS=0`, `GPU=0` — software blit via `GetDC`, **no** WNDPROC subclass.

Wine `_WINPROC_wrapper` (`user32` RVA `A5CC`, still present in Proton Experimental) is **not** our symbol. We only named it from Wine’s COFF. The belong death’s frame 4–6 are `rabiribi.exe` — the game’s message pump, not `gpu_bl_proc`.

---

## 3. Object table

Cross-PID = slotfile from process A, `do_load` in process B, same boot. Belong sitting: 131 regions restored, then 0 presents.

| Object | Who allocates | We rewind? | Stays in the present | Cross-PID restore |
| --- | --- | --- | --- | --- |
| **HWND** (swapchain `00020098` in both A and B logs) | Wine `user32`/`win32u` + **wineserver** | The *number* in game `.data` / our `Sw11` swap desc, if those pages are wanted | The server object, class atom, owner thread | **Straddle.** Same numeric HWND on sequential launches is Wine reuse, not identity. B’s `00020098` is B’s window. Copying A’s bytes does not move A’s window into B. **Demonstrated** same log number; **inferred** that is why `_WINPROC_wrapper` got `eax=0` |
| **WNDPROC** | Game `.text` (DxLib pump). Optional our `gpu_bl_proc` if borderless | `.text` yes. Subclass pointer lives in wineserver / user32 extra, **held** | Wine’s stored proc for that HWND | Same-boot RVA at 00400000 still exists. Lookup is Wine-side. NULL `call eax` is **demonstrated**; which table entry was NULL is **inferred** |
| **HDC / HFONT / HBITMAP** | `gdi32` + wineserver. We `GetDC` around software present only | No GDI object snapshot in tree | GDI images held; some GDI bytes may sit on process heap | Game `.data` may name A’s handles. B’s GDI table is new. Windows analog: `gdi32full+4EC87` list walk. Proton sitting died in USER32 first, 0 frames — **not demonstrated** as an HDC fault this time |
| **USER lock** (`win32u` sysparams) | Wine, per-process | No | `win32u.dll` held | Belong Proton log: `user_check_not_lock` on thread 320 while in message dispatch. **Demonstrated** Wine invariant break after copy |
| **wineserver** | Separate Linux process | Never | Always | HWND/HDC/msg queue live here. Freeze split already leaves 1 wineserver-side thread running. Park skipped because Present waits on it |
| **Process heap `00150000`** | ntdll `RtlCreateHeap` / `GetProcessHeap`. Wine USER32/GDI + anyone who didn’t go through `gameheap` | Policy **hold** (`PARTHOLD`). Save still `PARTITION LEAK` 1.1 MB inside it | Intended hold | Belong: `HeapValidate` FAILED after resume; Wine `invalid block header` at `09687850` in the coalesced `0968xxxx` window. Held heap, still torn |
| **Game runtime heap `0B630000`** | `gameheap` / classified “game’s runtime” | **Rewound** wholesale (Wine `HEAPBLOCKS` off) | Allocator metadata if LFH pages held | Entity table (player `x=26044`) **demonstrated** written back. Game objects come from A; Wine USER objects do not |
| **Ownerless heaps** `0C950000`, `02070000`, `001D0000` | Unclear (Wine or game) | **Rewound** wholesale | — | May contain USER/GDI nodes. **Inferred** extra straddle fuel |
| **Wrapper heap `0B400000`** | Us (`swalloc` / `HeapCreate`) | Held (`REWIND_SWHEAP=0`) | Our D3D objects, factory, logs | Game pointers to our ID3D11* stay valid **within session** because we hold+retire. Cross-PID: B’s wrapper heap is B’s; we do not copy it. Game image then names **A’s** resource pointers into **B’s** heap — **inferred** second-order; death was 0-frame USER32, not a vtable in d3d11 |
| **Game `.data` / writable `MEM_IMAGE`** | Loader mapped `rabiribi.exe` | **Rewound** if `region_wanted` | `.text` RX not writable → not in slot; we still **patch** it live | HWND, DxLib window struct, last pad struct, RNG, etc. go back to A. Imports into USER32 stay as thunks into **held** USER32 (**73 IAT crossings** demonstrated) |
| **Game `.text` (RX)** | Loader | Not captured (not writable). We **mutate** it (`gameheap` trampolines, IAT) | Patches are present-tense until a writable alias is restored | Writable IAT/tramp pages that *are* captured get A’s patches. RX remainder stays B’s. Same-boot same bytes **inferred** |
| **Our snapshot window (control block)** | Us; `savestate_exclude`. A: `159F0000`, B: `15920000` | **Never.** Different VA each process | Helper thread, exclusions, slot mapping | Load reads **slotfile**, not A’s control VA. Safe **if** no rewound pointer into it. Inventory “0 HELD and WRITABLE pointer-shaped words” is an upper bound, not HWND-safe (HWND is not a pointer into `user32.dll`) |
| **Slotfile `d3d9sw_slot0.bin`** | Us, on disk | N/A | File bytes | The experiment. 384.5 MB, 142 regions, 33 threads. Cross-session banner already says unsafe |
| **TEB / live stacks that moved** | ntdll / Wine | EXCLSKIP skips if excluded-now | TEBs always excluded | Belong: 2 EXCLSKIP + 2 belongs-elsewhere skipped; copy continued |
| **Thread contexts** | Us copying CONTEXT/TLS | Rewound threads: 33 contexts, 31 TLS | dinput thread held | Thread 320 **newer than save**, same role as gone 316. Context from A on B’s thread. Park at save was `d3d11+222EB`, not USER32 |
| **Clock** | Hooked exe IAT | Offsets shifted (`REWIND_CLOCK=1`, −163.1 s) | Calendar APIs, Wine internals | Harmless for HWND. Does not recreate USER objects |
| **XInput recognition** | Our `xinput1_4` or system | Module **held** | Pin `XINPUT=0` is launch-time | Game `.data` pad struct rewinds; next poll is present. Not this death |
| **D3D swapchain HWND field** | Our `Sw11` object on wrapper heap | Wrapper heap held → **not** rewound | Our object | Held in B as B created it (`hwnd=00020098`). Game-side copy of the HWND in `.data` **is** rewound. Two copies of one handle, different sides |

---

## 4. What a successful *within-session* restore is actually restoring

Not “the process.” The snapshot is the **game-shaped slice**: writable exe image, game heap, rewound stacks/contexts, file seeks, clock offsets, optional shared-heap blocks that reachability claimed.

It is valid only while the **Wine-shaped slice** (HWND, HDC, USER lock, wineserver queues, most of `00150000`) is the same objects the slice was named against. Same PID keeps those objects. Cross-PID replaces them and keeps the names.

That is Class A in `restore_invariants.md`: something inside the snapshot refers to something outside it.

---

## 5. Where the line is, in one sentence each

**Our code starts** at game-dir DLLs, the VEH, the IAT/GPA patches, and the trampolines we plant in the exe — plus every USER32/GDI call *we* make (`GetDC`, optional `SetWindowLongA`).

**The game ends** where a value stops being bytes we can put back and becomes a **Wine kernel object**: HWND, HDC, USER lock, wineserver message queue, ntdll heap headers we refused to own.

**The belong death** sat on that line: copy finished, game image said “DispatchMessage,” Wine `_WINPROC_wrapper` called `eax=0`, `win32u` asserted USER lock, ntdll `_dispatch_exception` wrote 0, process heap `00150000` already failed validation. Not a Proton-only copier bug; the same straddle as Windows `gdi32full+4EC87`, USER32-first because Wine has no `gdi32full`.

---

## 6. If we continue vs if we stop

Stop: keep PARTHOLD/EXCLSKIP/gameheap; refuse slotfiles from another PID; treat `resume: done` + USER32 as the expected cross-session terminal. Within-session is what the DLLs do today.

Continue (conform, don’t snapshot Wine): after load, **do not** run the game’s pump until we have a live HWND whose WNDPROC is the current `.text`; or stop rewinding the window-handle words; or recreate the DxLib window as a new object. Still inside the same drop-in DLLs. Rewinding `user32`/`gdi32`/`win32u` is the experiment that already killed XInput-style OS-thread heaps and is forbidden for GDI on Windows. Completing the snapshot by writing wineserver is not in this tree and is the wrong side of the line.

No extra end-user tool. Game may still be the 2026-09-18 title process (linux 56286) if it was left running.
