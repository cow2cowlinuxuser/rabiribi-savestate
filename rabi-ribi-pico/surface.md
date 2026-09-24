# Surface

Phase 0 measurement. Windows, `rabiribi.exe` only. The question is whether a
picoprocess boundary is viable, not whether every later phase is worth building.

## Verdict

Viable in user mode, as a narrowing of the wrappers that already run the game.
Not viable as a custom loader that maps the exe, and not viable as a boundary
that stubs USER32 or removes Steam.

The running image's static host surface is **245 imports across 5 DLLs**.
There is no delay-load directory, no CRT DLL, no `advapi32`, and no
`CreateProcess`. `ws2_32` is not in that table; DxLib loads it by name after
startup, and the call list is in the dynamic section below. `.text` on disk is
ciphertext (zero standard prologues in the section), and a Steamless pass over
the same file left those bytes unchanged. The static CRT reaches the heap
through `HeapAlloc` / `HeapFree` in the live IAT. A kernel pico provider is
not required for this binary.

What has to stay connected is the window (74 USER32 + 26 GDI32 calls), the
file and memory calls the static CRT actually makes, and `steam_api.dll`.
What can be left unconnected is a short list already in the import table
(`ShellExecuteA`, the clipboard, drag-and-drop, a handful of foreign-window
calls) plus DxLib's `ws2_32` loader. Those calls are recovered in
`surface_dynamic.txt`. `GetProcAddress` in a captured live image already
points into our `d3d11.dll`.

The GPU valve is not blanket-safe. `d3d11_sw.log` records 43 readbacks, every
one a `CopyResource` of backbuffer `#1` at 1280×720 into a staging texture,
then a `Map` for READ, served with the rendered pixels. No other resource is
read back. Synthesize everything else; keep that one path real. Descriptor-
carried stand-ins, because the read is size-sensitive.

## How it was measured

The file on disk is not the game. Steam's `.bind` stub leaves `.rdata` and
`.data` in the clear and encrypts `.text`, and it only decrypts after the
process has started. Checked again on this machine:

| Image | Size | Bytes at `_malloc` RVA `0x36E6B2` | `push ebp; mov ebp, esp` in `.text` |
|---|---|---|---|
| `rabiribi.exe` in the Steam directory | 6,062,736 | `3D C3 56 02 …` | 0 |
| Steamless copy of that same file | 6,062,736 | the same ciphertext | 0 |
| `rabiribi.exe.dump_00850000.exe`, written out of a running process | 17,620,992 | `55 8B EC 56 8B 75 08 …` | 4,728 |

The prologue in the dump is the one the static-CRT census already uses. The
names in `surface_static.txt` match the running image only because the import
descriptors live in plaintext `.rdata`. They are not evidence that the code
was read. Every function body this project already trusts — the five CRT
allocators, the heap co-tenants, the call sites in the crash chain — was
taken from the decrypted dump or from the process, which is also where
`d3d11_sw.log` and the savestate logs beside the exe come from.

| Source | What it is |
|---|---|
| `rabiribi.exe.dump_00850000.txt` | The decrypted image at `0x00850000`. Same 245 names, bound. `GetProcAddress`, `HeapFree`, and `CreateEventA` already point into our `d3d11.dll`. Module list is the process Steam actually built. |
| `d3d11_sw.log` beside the exe | 43 `READBACK` lines. All `backbuffer=1`, 1280×720, pixels served. Zero readbacks of any other resource. |
| On-disk import directory, RVA `0x435D04`, size `0x78` | The plaintext name list only. Useful as a checklist against the dump. Useless as a disassembly. |

`pe_imports.ps1` used to read data directory 0 (the export table) and walk it
as imports. It now reads directory 1, which is why a name list falls out of
the encrypted file at all. That fix does not decrypt `.text`.

No play session was launched for this pass. The dynamic module list below is
from the existing dump, not from a new `GetProcAddress` trace. A trace is
still the way to split "the game asked for this" from "Steam or SHELL32 pulled
it in." It does not reopen the name list, which the dump already bound.

## Static classification

Every name in `surface_static.txt` is in exactly one bucket.

### WIRED — forward

These are how the game and its static CRT actually run. Stubbing them does not
create a death boundary; it creates a game that never starts. Owning them
means the pico is the implementation the IAT points at, even when the body of
the call is the real host.

**Memory.** `HeapAlloc`, `HeapFree`, `HeapReAlloc`, `HeapSize`, `GetProcessHeap`,
`VirtualAlloc`, `VirtualFree`, `VirtualQuery`, `LocalAlloc`, `LocalFree`,
`GlobalAlloc`, `GlobalSize`, `GlobalLock`, `GlobalUnlock`, `GlobalFree`,
`GlobalMemoryStatus`.

**Threads and TLS.** `CreateThread`, `ExitThread`, `ResumeThread`,
`SuspendThread`, `GetCurrentThread`, `GetCurrentThreadId`, `GetThreadPriority`,
`SetThreadPriority`, `GetExitCodeThread`, `TlsAlloc`, `TlsGetValue`,
`TlsSetValue`, `TlsFree`.

**Sync.** `InitializeCriticalSection`, `InitializeCriticalSectionAndSpinCount`,
`EnterCriticalSection`, `LeaveCriticalSection`, `DeleteCriticalSection`,
`CreateEventA`, `SetEvent`, `ResetEvent`, `CreateSemaphoreA`,
`ReleaseSemaphore`, `WaitForSingleObject`, `WaitForMultipleObjects`, `Sleep`.

**Clock and calendar the CRT reads.** `QueryPerformanceCounter`,
`QueryPerformanceFrequency`, `GetTickCount`, `GetLocalTime`,
`GetSystemTimeAsFileTime`, `FileTimeToLocalFileTime`, `FileTimeToSystemTime`,
`GetTimeZoneInformation`.

**Files.** `CreateFileW`, `ReadFile`, `WriteFile`, `SetFilePointer`,
`SetFilePointerEx`, `GetFileSize`, `FlushFileBuffers`, `SetEndOfFile`,
`DeleteFileW`, `FindFirstFileW`, `FindNextFileW`, `FindClose`, `GetFileType`,
`SetCurrentDirectoryW`, `GetCurrentDirectoryW`, `GetTempPathA`, `GetTempPathW`,
`GetTempFileNameA`, `GetTempFileNameW`, `GetModuleFileNameA`,
`GetModuleFileNameW`. This is the VFS. A path outside the game directory and
the save area fails because the name is not in the namespace, which is the
same rule as [ABI.md](ABI.md). Temp paths have to land inside that namespace
or the static CRT's temp files become a hole.

**Loader, the dynamic door.** `LoadLibraryA`, `LoadLibraryW`, `LoadLibraryExW`,
`FreeLibrary`, `GetProcAddress`, `GetModuleHandleW`, `GetModuleHandleExW`.
Allow-list. Anything else returns null. This is the whole of Phase 2 that the
static table does not already enumerate.

**Process lifetime, ours.** `ExitProcess`, `TerminateProcess`, `GetCurrentProcess`,
`GetCurrentProcessId`, `GetStartupInfoW`, `GetCommandLineA`.

**CRT support that must succeed or startup dies.** `EncodePointer`,
`DecodePointer`, `IsDebuggerPresent`, `IsProcessorFeaturePresent`,
`GetSystemInfo`, `GetVersionExA`, `GetVersionExW`, `MulDiv`,
`WideCharToMultiByte`, `MultiByteToWideChar`, `GetStringTypeW`, `GetCPInfo`,
`GetOEMCP`, `GetACP`, `AreFileApisANSI`, `IsValidCodePage`, `CompareStringW`,
`LCMapStringW`, `GetLocaleInfoW`, `IsValidLocale`, `GetUserDefaultLCID`,
`EnumSystemLocalesW`, `lstrcmpW`, `lstrcpynW`, `lstrcpyW`, `lstrlenW`,
`GetStdHandle`, `SetStdHandle`, `GetConsoleCP`, `GetConsoleMode`,
`ReadConsoleW`, `WriteConsoleW`, `OutputDebugStringW`, `CloseHandle`,
`GetLastError`, `SetLastError`, `RtlUnwind`, `RaiseException`,
`UnhandledExceptionFilter`, `SetUnhandledExceptionFilter`, `GetEnvironmentStringsW`,
`FreeEnvironmentStringsW`. Environment reads return a frozen block.

**Resources in the exe.** `FindResourceA`, `LoadResource`, `LockResource`.

**Window, present, input.** The game is a DxLib Win32 client. These stay
wired, and the pico owns the one window they talk to:

`DefWindowProcW`, `RegisterClassExW`, `UnregisterClassW`, `CreateWindowExW`,
`DestroyWindow`, `ShowWindow`, `MoveWindow`, `SetWindowPos`, `UpdateWindow`,
`SetWindowTextW`, `GetClientRect`, `GetWindowRect`, `AdjustWindowRectEx`,
`GetWindowLongW`, `SetWindowLongW`, `SetClassLongW`, `SetWindowRgn`,
`BeginPaint`, `EndPaint`, `GetDC`, `ReleaseDC`, `FillRect`, `ShowCursor`,
`SetCursor`, `LoadCursorW`, `LoadIconW`, `GetCursorPos`, `ClientToScreen`,
`ClipCursor`, `GetKeyboardState`, `PeekMessageW`, `GetMessage` is not
imported; the pump is `PeekMessageW`, `TranslateMessage`, `DispatchMessageW`,
`TranslateAcceleratorW`, `IsDialogMessageW`, `MsgWaitForMultipleObjects`,
`GetQueueStatus`, `PostMessageW`, `SendMessageW`, `PostQuitMessage`,
`GetSystemMetrics`, `GetMonitorInfoA`, `GetMonitorInfoW`, `EnumDisplayMonitors`,
`EnumDisplaySettingsW`, `SystemParametersInfoW`, `SetTimer`, `KillTimer`,
`GetFocus`, `SetMenu`, `DrawMenuBar`, `DestroyMenu`, `GetMenuItemCount`,
`GetMenuItemInfoW`, `MessageBoxA`, `MessageBoxW`, `RegisterWindowMessageA`,
`GetWindowThreadProcessId`.

`ChangeDisplaySettingsA` is wired narrow: it may only confirm the mode the
pico window already has. A mode switch that would retarget the adapter is a
failure return, not a host call.

**GDI, because software present and DxLib text use it.** `GetDeviceCaps`,
`CombineRgn`, `CreateCompatibleDC`, `CreateRectRgn`, `CreateDCW`,
`CreateDIBSection`, `SelectObject`, `GetStockObject`, `CreateSolidBrush`,
`DeleteDC`, `DeleteObject`, `GetObjectA`, `StretchDIBits`, `SetDIBitsToDevice`,
`Rectangle`, `TextOutW`, `GetTextMetricsA`, `SetTextColor`, `SetBkMode`,
`SetBkColor`, `AddFontResourceExA`, `GetCharacterPlacementW`,
`GetTextExtentPoint32W`, `GetGlyphOutlineW`, `EnumFontFamiliesExW`,
`CreateFontW`.

### UNWIRED — resolve to a failing stub

Present in the import table today, and not required for the game to draw a
frame. Connecting them is the hole.

| Name | Why it is a hole |
|---|---|
| `ShellExecuteA` | The only spawn import. `CreateProcess` is absent. |
| `OpenClipboard`, `CloseClipboard`, `SetClipboardData`, `GetClipboardData`, `EmptyClipboard`, `IsClipboardFormatAvailable` | Clipboard is a foreign-process channel. |
| `DragAcceptFiles`, `DragQueryFileA`, `DragQueryFileW`, `DragFinish` | A path handed in from outside the VFS. |
| `FindWindowW`, `GetDesktopWindow`, `GetForegroundWindow`, `SetForegroundWindow`, `SetActiveWindow`, `BringWindowToTop`, `AttachThreadInput`, `PostThreadMessageA` | Other windows, other threads, other processes. |
| `UnhookWindowsHookEx` | No `SetWindowsHookEx` in the static table. Return success and install nothing. A later `GetProcAddress` of the setter is refused. |
| `SetEnvironmentVariableA` | Persistence that outlives the call. Reads stay wired, frozen. |

### PEER — connected, not ours

`steam_api.dll`: `SteamAPI_Init`, `SteamAPI_Shutdown`, `SteamAPI_RunCallbacks`,
`SteamAPI_RegisterCallResult`, `SteamAPI_UnregisterCallResult`, `SteamUtils`,
`SteamUser`, `SteamUserStats`, `SteamApps`, `SteamFriends`.

The game imports this and will not start without `SteamAPI_Init`. The client
then injects `steamclient.dll` and `gameoverlayrenderer.dll`, which import
`ws2_32`, `advapi32`, crypto, and the rest of the live module list. Mediating
the game's 245 slots does not unload those. In-session savestate already
treats them as peers. A claim that the process can do nothing outside itself
is false while Steam is in it. That is acceptable for a rewind substrate. It
is not a sandbox, which this project already said it is not.

## Dynamic surface

`surface_dynamic.txt` is every `LoadLibrary` / `GetProcAddress` argument
recovered from `rabiribi.exe.dump_00850000.exe` by `surface_calls.exe`. A
debugger is the wrong tool: the game imports `IsDebuggerPresent`, and the
instructions do not exist until the stub has decrypted. A play-session logger
in the shim would only see calls made after `d3d11.dll` attaches. The dump
contains the calls themselves.

Two shapes. Direct `FF 15 [IAT]` is the obvious one. DxLib's loader is the
other: it reads the `GetProcAddress` and `LoadLibraryW` slots into registers
and `call`s the register, which is why a scan that only looks at `FF 15`
misses `ws2_32`.

### WIRED — the game actually draws and plays through these

| DLL the call names | Exports it then asks for |
|---|---|
| `d3d11.dll`, `dxgi.dll` | `D3D11CreateDevice`, `CreateDXGIFactory` |
| `d3d9.dll` | `Direct3DCreate9`, `Direct3DCreate9Ex` |
| `DSound.DLL` | `DirectSoundEnumerateW` |
| `XAudio2_8.dll` | `XAudio2Create`, `CreateAudioVolumeMeter`, `CreateAudioReverb` |
| `X3DAudio1_7.dll` | `X3DAudioInitialize`, `X3DAudioCalculate` |
| `xinput1_4.dll`, `xinput1_3.dll`, `xinput9_1_0.dll` | `XInputGetState`, `XInputSetState` |
| `msacm32.dll` | `acmFormatSuggest`, `acmStreamOpen`, `acmStreamClose`, `acmMetrics`, `acmStreamPrepareHeader`, `acmStreamConvert`, `acmStreamUnprepareHeader`, `acmStreamSize` |
| `winmm.dll` | `timeGetTime`, `timeBeginPeriod`, `timeSetEvent`, `timeKillEvent`, `timeGetDevCaps`, `joyGetPosEx`, `joyGetDevCapsW`, `mciSendCommandW` |
| `Imm32.dll` | `ImmGetContext`, `ImmReleaseContext`, `ImmGetConversionStatus`, `ImmNotifyIME`, `ImmGetCandidateListW`, `ImmGetCandidateListCountW`, `ImmGetCompositionStringW`, `ImmSetCompositionStringW` |
| `ole32.dll` | `CoCreateInstance`, `CoInitializeEx`, `CoUninitialize`, `CoTaskMemAlloc`, `CoTaskMemFree`, `CoFreeUnusedLibraries` |
| `OleAut32.dll` | `LoadTypeLib`, `LoadRegTypeLib` |
| `comctl32.dll` | `InitCommonControls` |
| `dwmapi.dll` | `DwmEnableComposition` |
| `User32.dll` (again, by name) | `UpdateLayeredWindow`, `EnumDisplayDevicesW`, `WINNLSEnableIME`, `CreateWindowExW`, `CloseTouchInputHandle`, `GetTouchInputInfo`, `IsTouchWindow`, `RegisterTouchWindow`, `UnregisterTouchWindow` |
| `gdi32.dll` (again) | `AddFontMemResourceEx`, `RemoveFontMemResourceEx` |
| `%TEMP%\ddxx_MesHoooooook.dll` | `SetMSGHookDll` — DxLib's own helper |

`joyGetPosEx` is a second gamepad path. It can return "no device" and the
xinput path still works. The touch exports can fail. Neither is a hole.

The static CRT also probes newer kernel32/ntdll exports and lives with a NULL
(`FlsAlloc`, `GetTickCount64`, `CreateFile2`, `GlobalMemoryStatusEx`,
`InitializeCriticalSectionEx`, the threadpool family, `RtlGetVersion`,
`CreateSymbolicLinkW`, and the rest of that block in `surface_dynamic.txt`).
Returning NULL is the CRT's own fallback. `CorExitProcess` is the same probe
against `mscoree`, which this game does not have.

### UNWIRED — DxLib's network layer, present in this image

`LoadLibraryW("ws2_32.dll")` at RVA `0x70930`, then `GetProcAddress` for
`WSAStartup`, `WSACleanup`, `WSAGetLastError`, `socket`, `connect`, `bind`,
`listen`, `accept`, `send`, `sendto`, `recv`, `recvfrom`, `closesocket`,
`shutdown`, `inet_addr`, `htons`, `ntohs`, `getaddrinfo`, `gethostbyname`,
`gethostbyaddr`, `gethostname`. One slot in that run (`0x70973`) did not
decode; the string beside it in the table is `WSAAsyncSelect`.

That is the game's code asking, not Steam's import closure. Phase 2's
allow-list fails this `LoadLibraryW`. The rest of DxLib treats a failed
socket init as "no network," which is the death boundary doing what it was
for. `CreateSymbolicLinkW` fails the same way; the CRT already handles the NULL.

## Readback

Yes. The game reads the framebuffer back.

Evidence, `d3d11_sw.log`, 43 copies, all of this shape:

```
READBACK: CopyResource into staging #233 1280x720 from #1 1280x720 (backbuffer=1, render target=1): rendered pixels encoded into the staging plane
READBACK: the game mapped staging texture #233 1280x720 for READ (type=1). 1 readback copy(s) this session, 1 served with rendered pixels
```

Staging id `233` is the common one; a few sessions use another id (`725`,
`7788`). The source is always resource `#1`, the backbuffer, at 1280×720.
`gpu_readback` in `gpu.c` exists to stall the adapter and fill that staging
plane, and the log says the plane was served.

D3D9 `GetRenderTargetData` / `GetFrontBufferData` are `E_NOTIMPL` stubs. This
title's live path is D3D11. `ID3D11DeviceContext::GetData` returns zeros; the
game runs anyway, so occlusion queries are not a logic input.

Consequence for [VALVES.md](VALVES.md): display-only textures, static buffers,
and render targets the game does not map may be synthesized. The backbuffer,
and the staging texture it is copied into, may not. A restore that resumes
into a transition has to hand back the real pixels or the fade samples garbage.
That is one resource class, already implemented on the GPU path, not a reason
to carry every texture.

Stand-in shape: **descriptor-carried**. The shadow already stores `w/h/fmt`,
the readback is 1280×720, and a scaled `Map` with the wrong stride is the
failure `d3d11_sw.c` already logs. A 1×1 fault-in stand-in is the wrong size
the first time that path runs.

## What this does to the roadmap

| Phase | Viable? | As what |
|---|---|---|
| 0, this file | Done | Static names from the decrypted image, dynamic names from `surface_calls` on that dump, readback answered |
| 1, job cage | Yes, and independent | `KILL_ON_JOB_CLOSE` does not depend on the loader. Steam may already have the process in a job; nesting is allowed on this OS, and it should be checked once, not designed around |
| 2, own the loader | Yes, after the stub has decrypted | Patch the live 245-slot IAT. The image that contains the code is the one in memory; the file on disk has none of those bytes |
| 3, real GPU behind the shadow | Already the current GPU path | The new fact is the valve exception above, not the device |
| 4, kernel pico provider | Not justified | The static CRT calls `HeapAlloc` through the IAT. There is no hand-rolled `SYSCALL` surface to catch |

The project is successful when those 245 names have the verdicts above in the
running IAT, a `GetProcAddress` log adds nothing outside the allow-list, and a
restore still serves the 1280×720 backbuffer read. It does not need a library
OS, a driver, or a boundary that pretends Steam is not in the process.
