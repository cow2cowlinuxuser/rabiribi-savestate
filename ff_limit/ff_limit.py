"""
Keep Firefox from ballooning, using only the kernel's own supervisory API.

This is a "lite sandbox": a Windows job object. It is the same primitive
Chrome and Firefox use to build their own sandboxes, it is enforced by the
memory manager rather than by us, and it involves no injection, no remote
threads, no writes into Firefox, and no debugger attach. There is nothing here
for an AV heuristic to bite on.

Three tiers, weakest to strongest:

  1. Working set cap (JOB_OBJECT_LIMIT_WORKINGSET)
     Per process. The kernel continuously trims resident pages above the cap.
     Firefox may still *commit* memory, it just cannot keep it all resident.
     Nothing dies. This is the "stops growing" tier, and it costs pagefile I/O
     if Firefox genuinely needs more than the cap.

  2. Per-process commit cap (JOB_OBJECT_LIMIT_PROCESS_MEMORY)
     VirtualAlloc fails past the cap. Firefox's infallible allocator turns that
     into an OOM abort, so one gluttonous content process dies and you get a
     "tab crashed" page. The browser survives. This is the real puke.

  3. Job-wide commit cap (JOB_OBJECT_LIMIT_JOB_MEMORY)
     Backstop across every firefox.exe together. Hitting this can take the
     whole browser down, which is why tier 2 is set lower.

The job is destroyed when this process exits, and Firefox is NOT killed with it
(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE is deliberately not set). To make the cap
survive a reboot, run this at logon from Task Scheduler with pythonw.exe.

Caps count private commit, not reserved address space. Firefox reserves tens of
GB of VA it never commits; that is normal and is not what makes it fat.

  python ff_limit.py                          # defaults, report + enforce
  python ff_limit.py --watch-only             # measure first, set nothing
  python ff_limit.py --max-ws-mb 768 --max-process-gb 3

--puke is opt-in and mostly a placebo: it calls mozglue!jemalloc_free_dirty_pages
via CreateRemoteThread, which skips every main-thread-only arena (that is where
the DOM/JS heap lives), so it typically returns tens of MB out of many GB. It is
also the only part of this tool that looks like injection. Left off by default.
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as w
import struct
import sys
import time

# Process access rights
PROCESS_TERMINATE = 0x0001
PROCESS_CREATE_THREAD = 0x0002
PROCESS_VM_OPERATION = 0x0008
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
PROCESS_SET_QUOTA = 0x0100
SYNCHRONIZE = 0x00100000

# JOBOBJECT_BASIC_LIMIT_INFORMATION::LimitFlags (winnt.h)
JOB_OBJECT_LIMIT_WORKINGSET = 0x00000001
JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 0x00000008
JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100
JOB_OBJECT_LIMIT_JOB_MEMORY = 0x00000200
JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION = 0x00000400
JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000  # deliberately never set

JobObjectExtendedLimitInformation = 9

TH32CS_SNAPPROCESS = 0x2
WAIT_TIMEOUT = 0x102
ERROR_PRIVILEGE_NOT_HELD = 1314
GB = 1024 * 1024 * 1024
MB = 1024 * 1024

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
SIZE_T = ctypes.c_size_t
HMODULE = ctypes.c_void_p

k32.OpenProcess.restype = w.HANDLE
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.CreateJobObjectW.restype = w.HANDLE
k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
k32.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
k32.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
k32.QueryInformationJobObject.argtypes = [
    w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD)
]
k32.IsProcessInJob.argtypes = [w.HANDLE, w.HANDLE, ctypes.POINTER(w.BOOL)]
k32.CloseHandle.argtypes = [w.HANDLE]
k32.CreateRemoteThread.restype = w.HANDLE
k32.CreateRemoteThread.argtypes = [
    w.HANDLE, ctypes.c_void_p, SIZE_T, ctypes.c_void_p,
    ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD),
]
psapi.EnumProcessModulesEx.argtypes = [
    w.HANDLE, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD), w.DWORD
]
psapi.GetModuleFileNameExW.argtypes = [w.HANDLE, HMODULE, w.LPWSTR, w.DWORD]
psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, ctypes.c_void_p, w.DWORD]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", w.DWORD), ("cntThreads", w.DWORD),
        ("th32ParentProcessID", w.DWORD), ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", w.DWORD), ("szExeFile", w.WCHAR * 260),
    ]


class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", w.DWORD), ("PageFaultCount", w.DWORD),
        ("PeakWorkingSetSize", SIZE_T), ("WorkingSetSize", SIZE_T),
        ("QuotaPeakPagedPoolUsage", SIZE_T), ("QuotaPagedPoolUsage", SIZE_T),
        ("QuotaPeakNonPagedPoolUsage", SIZE_T), ("QuotaNonPagedPoolUsage", SIZE_T),
        ("PagefileUsage", SIZE_T), ("PeakPagefileUsage", SIZE_T),
        ("PrivateUsage", SIZE_T),
    ]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64),
        ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", w.DWORD),
        ("MinimumWorkingSetSize", SIZE_T),
        ("MaximumWorkingSetSize", SIZE_T),
        ("ActiveProcessLimit", w.DWORD),
        ("Affinity", SIZE_T),
        ("PriorityClass", w.DWORD),
        ("SchedulingClass", w.DWORD),
    ]


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("ReadOperationCount", ctypes.c_uint64), ("WriteOperationCount", ctypes.c_uint64),
        ("OtherOperationCount", ctypes.c_uint64), ("ReadTransferCount", ctypes.c_uint64),
        ("WriteTransferCount", ctypes.c_uint64), ("OtherTransferCount", ctypes.c_uint64),
    ]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", IO_COUNTERS),
        ("ProcessMemoryLimit", SIZE_T),
        ("JobMemoryLimit", SIZE_T),
        ("PeakProcessMemoryUsed", SIZE_T),
        ("PeakJobMemoryUsed", SIZE_T),
    ]


def log(msg: str) -> None:
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


def firefox_pids() -> list[tuple[int, int]]:
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    pe = PROCESSENTRY32W()
    pe.dwSize = ctypes.sizeof(pe)
    out = []
    ok = k32.Process32FirstW(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.lower() == "firefox.exe":
            out.append((pe.th32ProcessID, pe.th32ParentProcessID))
        ok = k32.Process32NextW(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return out


def meminfo(h) -> tuple[int, int] | None:
    c = PROCESS_MEMORY_COUNTERS_EX()
    c.cb = ctypes.sizeof(c)
    if not psapi.GetProcessMemoryInfo(h, ctypes.byref(c), ctypes.sizeof(c)):
        return None
    return int(c.WorkingSetSize), int(c.PrivateUsage)


def pe_export_rva(path: str, want: str) -> int:
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    coff = pe + 4
    nsec = struct.unpack_from("<H", d, coff + 2)[0]
    soh = struct.unpack_from("<H", d, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", d, opt)[0]
    export_rva = struct.unpack_from("<I", d, opt + (112 if magic == 0x20B else 96))[0]
    sec = opt + soh
    secs = []
    for i in range(nsec):
        vsz, va, rawsz, raw = struct.unpack_from("<IIII", d, sec + i * 40 + 8)
        secs.append((va, vsz, raw, rawsz))

    def to_off(rva: int) -> int:
        for va, vsz, raw, rawsz in secs:
            if va <= rva < va + max(vsz, rawsz):
                return raw + (rva - va)
        raise ValueError(hex(rva))

    off = to_off(export_rva)
    nnames = struct.unpack_from("<I", d, off + 24)[0]
    names_off = to_off(struct.unpack_from("<I", d, off + 32)[0])
    ords_off = to_off(struct.unpack_from("<I", d, off + 36)[0])
    funcs_off = to_off(struct.unpack_from("<I", d, off + 28)[0])
    for i in range(nnames):
        no = to_off(struct.unpack_from("<I", d, names_off + i * 4)[0])
        if d[no : d.find(b"\x00", no)].decode("ascii") == want:
            oi = struct.unpack_from("<H", d, ords_off + i * 2)[0]
            return struct.unpack_from("<I", d, funcs_off + oi * 4)[0]
    raise LookupError(want)


class Limiter:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.max_proc = int(args.max_process_gb * GB) if args.max_process_gb else 0
        self.max_job = int(args.max_job_gb * GB) if args.max_job_gb else 0
        self.max_ws = int(args.max_ws_mb * MB) if args.max_ws_mb else 0
        self.min_ws = int(args.min_ws_mb * MB) if args.max_ws_mb else 0
        self.assigned: set[int] = set()
        self.failed: dict[int, int] = {}
        self.job = None
        self.puke_rva = None
        if args.puke:
            self.puke_rva = pe_export_rva(
                r"C:\Program Files\Mozilla Firefox\mozglue.dll", "jemalloc_free_dirty_pages"
            )
        if not args.watch_only:
            self._make_job()

    def _make_job(self) -> None:
        self.job = k32.CreateJobObjectW(None, None)
        if not self.job:
            raise OSError(ctypes.get_last_error(), "CreateJobObject")
        info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        flags = 0
        desc = []
        if self.max_proc:
            flags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY
            info.ProcessMemoryLimit = self.max_proc
            desc.append(f"per-process commit {self.max_proc/GB:.1f} GB")
        if self.max_job:
            flags |= JOB_OBJECT_LIMIT_JOB_MEMORY
            info.JobMemoryLimit = self.max_job
            desc.append(f"job-wide commit {self.max_job/GB:.1f} GB")
        if self.max_ws:
            # Both bounds must be nonzero together, per winnt.h.
            flags |= JOB_OBJECT_LIMIT_WORKINGSET
            info.BasicLimitInformation.MinimumWorkingSetSize = self.min_ws
            info.BasicLimitInformation.MaximumWorkingSetSize = self.max_ws
            desc.append(f"per-process working set {self.min_ws/MB:.0f}-{self.max_ws/MB:.0f} MB")
        if self.args.quiet_crash:
            flags |= JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION
            desc.append("no crash dialog")
        info.BasicLimitInformation.LimitFlags = flags
        ok = k32.SetInformationJobObject(
            self.job, JobObjectExtendedLimitInformation, ctypes.byref(info), ctypes.sizeof(info)
        )
        if not ok and ctypes.get_last_error() == ERROR_PRIVILEGE_NOT_HELD and self.max_ws:
            # JOB_OBJECT_LIMIT_WORKINGSET needs elevation. The commit caps do
            # not, and they are the tier that actually stops growth, so drop
            # the working set tier and keep going rather than failing outright.
            log("working set cap needs an elevated process; skipping that tier")
            flags &= ~JOB_OBJECT_LIMIT_WORKINGSET
            info.BasicLimitInformation.LimitFlags = flags
            info.BasicLimitInformation.MinimumWorkingSetSize = 0
            info.BasicLimitInformation.MaximumWorkingSetSize = 0
            desc = [d for d in desc if not d.startswith("per-process working set")]
            self.max_ws = 0
            ok = k32.SetInformationJobObject(
                self.job, JobObjectExtendedLimitInformation,
                ctypes.byref(info), ctypes.sizeof(info),
            )
        if not ok:
            raise OSError(ctypes.get_last_error(), "SetInformationJobObject")
        log("job limits: " + "; ".join(desc))

    def peak(self) -> tuple[int, int]:
        if not self.job:
            return 0, 0
        info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        ret = w.DWORD()
        if not k32.QueryInformationJobObject(
            self.job, JobObjectExtendedLimitInformation,
            ctypes.byref(info), ctypes.sizeof(info), ctypes.byref(ret)
        ):
            return 0, 0
        return int(info.PeakProcessMemoryUsed), int(info.PeakJobMemoryUsed)

    def attach(self) -> None:
        if not self.job:
            return
        for pid, ppid in firefox_pids():
            if pid in self.assigned:
                continue
            if self.failed.get(pid, 0) >= 3:
                continue
            h = k32.OpenProcess(
                PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                False, pid,
            )
            if not h:
                self.failed[pid] = self.failed.get(pid, 0) + 1
                continue
            in_any = w.BOOL()
            k32.IsProcessInJob(h, None, ctypes.byref(in_any))
            ok = k32.AssignProcessToJobObject(self.job, h)
            err = 0 if ok else ctypes.get_last_error()
            k32.CloseHandle(h)
            if ok:
                self.assigned.add(pid)
                log(f"capped pid {pid} (ppid {ppid}, was already in a job: {bool(in_any.value)})")
            else:
                # Retry a couple of times: a process can refuse right at spawn.
                # Never mark it assigned, or coverage silently erodes.
                n = self.failed.get(pid, 0) + 1
                self.failed[pid] = n
                if n >= 3:
                    log(f"pid {pid} will not join the job (err {err}); it stays uncapped")

    def snapshot(self) -> tuple[int, int, int]:
        tws = tpr = 0
        live = set()
        for pid, _ppid in firefox_pids():
            live.add(pid)
            h = k32.OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                False, pid,
            )
            if not h:
                continue
            mi = meminfo(h)
            k32.CloseHandle(h)
            if mi:
                tws += mi[0]
                tpr += mi[1]
        self.assigned &= live
        for pid in list(self.failed):
            if pid not in live:
                del self.failed[pid]
        return tws, tpr, len(live)

    def puke(self) -> int:
        """Opt-in jemalloc purge. Only reaches thread-safe arenas; see module docstring."""
        if self.puke_rva is None:
            return 0
        n = 0
        for pid, _ppid in firefox_pids():
            h = k32.OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_READ
                | PROCESS_QUERY_INFORMATION,
                False, pid,
            )
            if not h:
                continue
            base = self._mozglue(h)
            if base:
                tid = w.DWORD()
                th = k32.CreateRemoteThread(
                    h, None, 0, ctypes.c_void_p(base + self.puke_rva), None, 0, ctypes.byref(tid)
                )
                if th:
                    # Never TerminateThread here. jemalloc_free_dirty_pages holds
                    # the arena-collection lock for its whole loop; killing it
                    # mid-purge wedges every future malloc in that process.
                    if k32.WaitForSingleObject(th, 20000) != WAIT_TIMEOUT:
                        n += 1
                    else:
                        log(f"  pid {pid}: purge thread still running, leaving it alone")
                    k32.CloseHandle(th)
            k32.CloseHandle(h)
        return n

    def _mozglue(self, h) -> int | None:
        needed = w.DWORD()
        psapi.EnumProcessModulesEx(h, None, 0, ctypes.byref(needed), 0x03)
        n = max(needed.value // ctypes.sizeof(ctypes.c_void_p), 1)
        arr = (ctypes.c_void_p * n)()
        if not psapi.EnumProcessModulesEx(
            h, ctypes.byref(arr), ctypes.sizeof(arr), ctypes.byref(needed), 0x03
        ):
            return None
        buf = ctypes.create_unicode_buffer(32768)
        for i in range(n):
            if arr[i] and psapi.GetModuleFileNameExW(h, HMODULE(arr[i]), buf, len(buf)):
                if buf.value.lower().endswith("\\mozglue.dll"):
                    return int(arr[i])
        return None

    def run(self) -> None:
        mode = "watch-only (no limits set)" if self.args.watch_only else "enforcing"
        log(f"ff_limit {mode}; job dies with this process, Firefox does not")
        last_status = 0.0
        last_puke = 0.0
        while True:
            self.attach()
            ws, priv, n = self.snapshot()
            if n == 0:
                if time.time() - last_status >= 30:
                    log("no firefox.exe running")
                    last_status = time.time()
                time.sleep(self.args.interval)
                continue
            if (
                self.args.puke
                and priv >= self.args.puke_gb * GB
                and time.time() - last_puke >= self.args.cooldown
            ):
                last_puke = time.time()
                got = self.puke()
                ws2, priv2, _ = self.snapshot()
                log(
                    f"purge on {got} processes: private {priv/GB:.2f} -> {priv2/GB:.2f} GB "
                    f"({(priv2-priv)/GB:+.2f})"
                )
                ws, priv = ws2, priv2
            if time.time() - last_status >= self.args.status:
                pk_proc, pk_job = self.peak()
                extra = ""
                if pk_job:
                    extra = f"  peak/proc={pk_proc/GB:.2f} peak/job={pk_job/GB:.2f}"
                log(
                    f"n={n} capped={len(self.assigned)}  private={priv/GB:.2f} GB  "
                    f"ws={ws/GB:.2f} GB{extra}"
                )
                last_status = time.time()
            time.sleep(self.args.interval)


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Cap Firefox with a Windows job object (no injection)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--max-process-gb", type=float, default=4.0,
                   help="per-process commit cap; a runaway tab dies alone. 0 disables")
    p.add_argument("--max-job-gb", type=float, default=24.0,
                   help="job-wide commit cap across all firefox.exe. 0 disables")
    p.add_argument("--max-ws-mb", type=float, default=1024.0,
                   help="per-process resident cap; kernel trims above it, nothing dies. 0 disables")
    p.add_argument("--min-ws-mb", type=float, default=32.0,
                   help="per-process working set floor (must be nonzero if max is)")
    p.add_argument("--no-quiet-crash", dest="quiet_crash", action="store_false",
                   help="allow the Windows crash dialog on unhandled exceptions")
    p.add_argument("--watch-only", action="store_true",
                   help="measure and report, set no limits at all")
    p.add_argument("--puke", action="store_true",
                   help="also inject a jemalloc purge (opt-in; low yield, injection-shaped)")
    p.add_argument("--puke-gb", type=float, default=12.0, help="private commit that triggers --puke")
    p.add_argument("--cooldown", type=float, default=60.0, help="min seconds between purges")
    p.add_argument("--interval", type=float, default=2.0, help="poll seconds")
    p.add_argument("--status", type=float, default=30.0, help="status line seconds")
    p.set_defaults(quiet_crash=True)
    args = p.parse_args()
    if args.max_process_gb and args.max_job_gb and args.max_process_gb > args.max_job_gb:
        p.error("--max-process-gb cannot exceed --max-job-gb")
    if args.max_ws_mb and not args.min_ws_mb:
        p.error("--min-ws-mb must be nonzero when --max-ws-mb is set")
    return args


def main() -> int:
    try:
        Limiter(parse_args()).run()
    except KeyboardInterrupt:
        log("stopped; job released, Firefox left running and uncapped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
