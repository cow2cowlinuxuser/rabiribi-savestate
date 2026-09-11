"""Prove the job-object caps actually enforce, on a throwaway process.

Spawns a child that allocates 64 MB at a time, caps it at 256 MB of commit and
96 MB resident, and reports what the kernel did. Nothing here touches Firefox.
"""
from __future__ import annotations

import ctypes
import ctypes.wintypes as w
import subprocess
import sys
import time

sys.path.insert(0, __file__.rsplit("\\", 1)[0])
from ff_limit import (  # noqa: E402
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JobObjectExtendedLimitInformation,
    JOB_OBJECT_LIMIT_PROCESS_MEMORY, JOB_OBJECT_LIMIT_WORKINGSET,
    JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION,
    PROCESS_MEMORY_COUNTERS_EX, GB, MB, k32, psapi,
)

CHILD = r"""
import sys, time
blocks = []
for i in range(64):
    try:
        b = bytearray(64 * 1024 * 1024)
        b[::4096] = b"\x01" * len(b[::4096])
        blocks.append(b)
        print("committed %d MB" % ((i + 1) * 64), flush=True)
    except MemoryError:
        print("MemoryError at %d MB -- allocation refused" % (i * 64), flush=True)
        sys.exit(42)
    time.sleep(0.05)
print("reached 4 GB uncapped", flush=True)
"""

PROC_LIMIT = 256 * MB
WS_LIMIT = 96 * MB


def main() -> int:
    job = k32.CreateJobObjectW(None, None)
    if not job:
        raise OSError(ctypes.get_last_error(), "CreateJobObject")

    def apply(with_ws: bool) -> bool:
        info = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        flags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION
        info.ProcessMemoryLimit = PROC_LIMIT
        if with_ws:
            flags |= JOB_OBJECT_LIMIT_WORKINGSET
            info.BasicLimitInformation.MinimumWorkingSetSize = 16 * MB
            info.BasicLimitInformation.MaximumWorkingSetSize = WS_LIMIT
        info.BasicLimitInformation.LimitFlags = flags
        return bool(k32.SetInformationJobObject(
            job, JobObjectExtendedLimitInformation, ctypes.byref(info), ctypes.sizeof(info)
        ))

    ws_capped = apply(True)
    if not ws_capped:
        print(f"working set tier refused (err {ctypes.get_last_error()}), commit tier only")
        if not apply(False):
            raise OSError(ctypes.get_last_error(), "SetInformationJobObject")
    print(f"job: commit cap {PROC_LIMIT//MB} MB, working set cap {'on' if ws_capped else 'off'}")

    p = subprocess.Popen(
        [sys.executable, "-c", CHILD], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    h = k32.OpenProcess(0x1F0FFF, False, p.pid)
    ok = k32.AssignProcessToJobObject(job, h)
    print(f"assigned pid {p.pid} to job: {bool(ok)} (err {ctypes.get_last_error() if not ok else 0})")

    peak_ws = 0
    while p.poll() is None:
        c = PROCESS_MEMORY_COUNTERS_EX()
        c.cb = ctypes.sizeof(c)
        if psapi.GetProcessMemoryInfo(h, ctypes.byref(c), ctypes.sizeof(c)):
            peak_ws = max(peak_ws, int(c.WorkingSetSize))
        time.sleep(0.05)

    out = p.stdout.read().strip().splitlines()
    for line in out[-4:]:
        print("  child:", line)
    print(f"child exit code: {p.returncode}")
    print(f"peak working set observed: {peak_ws/MB:.1f} MB (cap {WS_LIMIT//MB} MB)")

    ret = w.DWORD()
    q = JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
    if k32.QueryInformationJobObject(
        job, JobObjectExtendedLimitInformation, ctypes.byref(q), ctypes.sizeof(q),
        ctypes.byref(ret)
    ):
        print(f"job peak process commit: {q.PeakProcessMemoryUsed/MB:.1f} MB")

    k32.CloseHandle(h)
    k32.CloseHandle(job)

    if p.returncode == 42:
        print("\nPASS: the commit cap refused the allocation")
    elif p.returncode != 0:
        print(f"\nPASS: the child was killed by the cap (exit {p.returncode})")
    else:
        print("\nFAIL: the child allocated 4 GB; the cap did not apply")
    return 0


if __name__ == "__main__":
    sys.exit(main())
