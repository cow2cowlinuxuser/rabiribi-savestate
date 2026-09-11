"""Why does AssignProcessToJobObject refuse Firefox content processes?

Reports, per firefox.exe: integrity level, which access rights OpenProcess will
grant, whether it is already in a job, and the exact assign error.
"""
import ctypes
import ctypes.wintypes as w

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
adv = ctypes.WinDLL("advapi32", use_last_error=True)
SIZE_T = ctypes.c_size_t

k32.OpenProcess.restype = w.HANDLE
k32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
k32.CreateJobObjectW.restype = w.HANDLE
k32.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
k32.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
k32.IsProcessInJob.argtypes = [w.HANDLE, w.HANDLE, ctypes.POINTER(w.BOOL)]
k32.CloseHandle.argtypes = [w.HANDLE]
adv.OpenProcessToken.argtypes = [w.HANDLE, w.DWORD, ctypes.POINTER(w.HANDLE)]
adv.GetTokenInformation.argtypes = [
    w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.POINTER(w.DWORD)
]
adv.ConvertSidToStringSidW.argtypes = [ctypes.c_void_p, ctypes.POINTER(w.LPWSTR)]

RIGHTS = {
    "TERMINATE": 0x0001,
    "SET_QUOTA": 0x0100,
    "QUERY_LIMITED": 0x1000,
    "QUERY_INFO": 0x0400,
    "SET_INFO": 0x0200,
    "ALL_ACCESS": 0x1FFFFF,
}
IL_NAMES = {
    0x0000: "Untrusted", 0x1000: "Low", 0x2000: "Medium",
    0x2100: "Medium+", 0x3000: "High", 0x4000: "System",
}
TH32CS_SNAPPROCESS = 0x2
TokenIntegrityLevel = 25


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", w.DWORD), ("cntThreads", w.DWORD),
        ("th32ParentProcessID", w.DWORD), ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", w.DWORD), ("szExeFile", w.WCHAR * 260),
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


def integrity(pid):
    h = k32.OpenProcess(0x1000, False, pid)
    if not h:
        return "?"
    tok = w.HANDLE()
    if not adv.OpenProcessToken(h, 0x0008, ctypes.byref(tok)):  # TOKEN_QUERY
        k32.CloseHandle(h)
        return "?"
    need = w.DWORD()
    adv.GetTokenInformation(tok, TokenIntegrityLevel, None, 0, ctypes.byref(need))
    buf = ctypes.create_string_buffer(need.value)
    lvl = "?"
    if adv.GetTokenInformation(tok, TokenIntegrityLevel, buf, need, ctypes.byref(need)):
        sid = ctypes.c_void_p.from_buffer(buf).value
        s = w.LPWSTR()
        if adv.ConvertSidToStringSidW(sid, ctypes.byref(s)):
            rid = int(s.value.rsplit("-", 1)[1])
            lvl = IL_NAMES.get(rid, hex(rid))
    k32.CloseHandle(tok)
    k32.CloseHandle(h)
    return lvl


job = k32.CreateJobObjectW(None, None)
print(f"probe job handle: {job}\n")
print(f"{'pid':>7} {'ppid':>7} {'integrity':>10}  {'in job':>6}  rights denied / assign result")
print("-" * 90)

for pid, ppid in sorted(firefox_pids()):
    il = integrity(pid)
    denied = [n for n, r in RIGHTS.items() if not k32.OpenProcess(r, False, pid)]
    h = k32.OpenProcess(RIGHTS["SET_QUOTA"] | RIGHTS["TERMINATE"], False, pid)
    if not h:
        print(f"{pid:>7} {ppid:>7} {il:>10}  {'-':>6}  OpenProcess(SET_QUOTA|TERMINATE) "
              f"failed err={ctypes.get_last_error()}")
        continue
    injob = w.BOOL()
    k32.IsProcessInJob(h, None, ctypes.byref(injob))
    ok = k32.AssignProcessToJobObject(job, h)
    err = 0 if ok else ctypes.get_last_error()
    k32.CloseHandle(h)
    res = "ASSIGNED" if ok else f"err={err}"
    print(f"{pid:>7} {ppid:>7} {il:>10}  {str(bool(injob.value)):>6}  "
          f"denied={denied or 'none'}  {res}")
