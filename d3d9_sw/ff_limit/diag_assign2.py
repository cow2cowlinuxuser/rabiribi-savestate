"""Test: one job object per Firefox process, instead of one shared job.

Hypothesis: Firefox sandboxes each child in its own job. Assigning a process
that is already in job J_old to our job J_new makes J_new a *child* of J_old.
A job can have only one parent, so a single shared J_new can nest under exactly
one sandbox job and every later assign fails with ERROR_ACCESS_DENIED (5).

If that is right, a dedicated job per process should assign all of them.
"""
import ctypes
import ctypes.wintypes as w

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
SIZE_T = ctypes.c_size_t
GB = 1024 ** 3

k32.OpenProcess.restype = w.HANDLE
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.CreateJobObjectW.restype = w.HANDLE
k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
k32.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
k32.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
k32.IsProcessInJob.argtypes = [w.HANDLE, w.HANDLE, ctypes.POINTER(w.BOOL)]
k32.CloseHandle.argtypes = [w.HANDLE]

PROCESS_TERMINATE = 0x0001
PROCESS_SET_QUOTA = 0x0100
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TH32CS_SNAPPROCESS = 0x2
JOB_OBJECT_LIMIT_WORKINGSET = 0x0001
JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x0100
JobObjectExtendedLimitInformation = 9


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", w.DWORD), ("cntThreads", w.DWORD),
        ("th32ParentProcessID", w.DWORD), ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", w.DWORD), ("szExeFile", w.WCHAR * 260),
    ]


class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
        ("LimitFlags", w.DWORD), ("MinimumWorkingSetSize", SIZE_T),
        ("MaximumWorkingSetSize", SIZE_T), ("ActiveProcessLimit", w.DWORD),
        ("Affinity", SIZE_T), ("PriorityClass", w.DWORD), ("SchedulingClass", w.DWORD),
    ]


class IO_COUNTERS(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint64) for n in (
        "ReadOperationCount", "WriteOperationCount", "OtherOperationCount",
        "ReadTransferCount", "WriteTransferCount", "OtherTransferCount")]


class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("BasicLimitInformation", JOBOBJECT_BASIC_LIMIT_INFORMATION),
        ("IoInfo", IO_COUNTERS), ("ProcessMemoryLimit", SIZE_T),
        ("JobMemoryLimit", SIZE_T), ("PeakProcessMemoryUsed", SIZE_T),
        ("PeakJobMemoryUsed", SIZE_T),
    ]


def firefox_pids():
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


print(f"{'pid':>7}  {'in job':>6}  {'assign':>10}  {'commit cap':>12}  {'ws cap':>10}")
print("-" * 60)
jobs = []
n_ok = n_cap = n_ws = 0
for pid, _ppid in sorted(firefox_pids()):
    h = k32.OpenProcess(
        PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, False, pid
    )
    if not h:
        print(f"{pid:>7}  OpenProcess failed err={ctypes.get_last_error()}")
        continue
    injob = w.BOOL()
    k32.IsProcessInJob(h, None, ctypes.byref(injob))

    job = k32.CreateJobObjectW(None, None)  # one dedicated job per process
    jobs.append(job)
    ok = k32.AssignProcessToJobObject(job, h)
    err = 0 if ok else ctypes.get_last_error()
    k32.CloseHandle(h)
    if not ok:
        print(f"{pid:>7}  {str(bool(injob.value)):>6}  {'err=%d' % err:>10}")
        continue
    n_ok += 1

    # Now that the process is in our job, can we actually set the limits?
    def try_limits(with_ws):
        info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        flags = JOB_OBJECT_LIMIT_PROCESS_MEMORY
        info.ProcessMemoryLimit = 4 * GB
        if with_ws:
            flags |= JOB_OBJECT_LIMIT_WORKINGSET
            info.BasicLimitInformation.MinimumWorkingSetSize = 32 * 1024 * 1024
            info.BasicLimitInformation.MaximumWorkingSetSize = 1024 * 1024 * 1024
        info.BasicLimitInformation.LimitFlags = flags
        return k32.SetInformationJobObject(
            job, JobObjectExtendedLimitInformation, ctypes.byref(info), ctypes.sizeof(info)
        ), ctypes.get_last_error()

    ws_ok, ws_err = try_limits(True)
    if ws_ok:
        n_cap += 1
        n_ws += 1
        cap_s, ws_s = "4 GB", "1024 MB"
    else:
        cap_ok, cap_err = try_limits(False)
        cap_s = "4 GB" if cap_ok else f"err={cap_err}"
        ws_s = f"err={ws_err}"
        if cap_ok:
            n_cap += 1
    print(f"{pid:>7}  {str(bool(injob.value)):>6}  {'ASSIGNED':>10}  {cap_s:>12}  {ws_s:>10}")

print(f"\nassigned {n_ok}, commit-capped {n_cap}, ws-capped {n_ws}")
print("(jobs are closed as this process exits; no limits persist)")
for j in jobs:
    k32.CloseHandle(j)
