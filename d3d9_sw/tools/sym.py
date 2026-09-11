"""Turn a 'module+0xRVA' from a fault log into a function and source line.

The wrapper logs faults as an offset into its own DLL because that is all it
can know from inside a vectored handler. Reading that back needs the PDB the
build already writes next to the DLL, so hand it here rather than reaching for
a debugger and a live process.

    python tools/sym.py x86/d3d11.dll 0x26f6b [more rvas...]
"""

import ctypes
import ctypes.wintypes as w
import sys
import os

dbghelp = ctypes.windll.dbghelp
BASE = 0x10000000
SYMOPT_LOAD_LINES = 0x10
SYMOPT_UNDNAME = 0x02


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", w.ULONG), ("TypeIndex", w.ULONG),
        ("Reserved", ctypes.c_ulonglong * 2), ("Index", w.ULONG),
        ("Size", w.ULONG), ("ModBase", ctypes.c_ulonglong),
        ("Flags", w.ULONG), ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong), ("Register", w.ULONG),
        ("Scope", w.ULONG), ("Tag", w.ULONG), ("NameLen", w.ULONG),
        ("MaxNameLen", w.ULONG), ("Name", ctypes.c_char * 2048),
    ]


class LINE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", w.DWORD), ("Key", ctypes.c_void_p),
        ("LineNumber", w.DWORD), ("FileName", ctypes.c_char_p),
        ("Address", ctypes.c_ulonglong),
    ]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dll = os.path.abspath(sys.argv[1])
    if not os.path.exists(dll):
        sys.exit("no such file: %s" % dll)

    dbghelp.SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME)
    if not dbghelp.SymInitialize(ctypes.c_void_p(-1), None, False):
        sys.exit("SymInitialize failed")
    dbghelp.SymLoadModuleEx.restype = ctypes.c_ulonglong
    base = dbghelp.SymLoadModuleEx(
        ctypes.c_void_p(-1), None, dll.encode(), None,
        ctypes.c_ulonglong(BASE), 0, None, 0)
    if not base:
        sys.exit("SymLoadModuleEx failed (is the .pdb beside the .dll?)")

    for arg in sys.argv[2:]:
        rva = int(arg, 16 if arg.lower().startswith("0x") else 10)
        addr = ctypes.c_ulonglong(base + rva)

        si = SYMBOL_INFO()
        si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 2048
        si.MaxNameLen = 2047
        disp = ctypes.c_ulonglong(0)
        where = "?"
        if dbghelp.SymFromAddr(ctypes.c_void_p(-1), addr, ctypes.byref(disp),
                               ctypes.byref(si)):
            where = "%s+0x%x" % (si.Name.decode(errors="replace"), disp.value)

        line = LINE64()
        line.SizeOfStruct = ctypes.sizeof(LINE64)
        ldisp = w.DWORD(0)
        src = ""
        if dbghelp.SymGetLineFromAddr64(ctypes.c_void_p(-1), addr,
                                        ctypes.byref(ldisp), ctypes.byref(line)):
            src = "  %s:%d" % (line.FileName.decode(errors="replace"),
                               line.LineNumber)
        print("+0x%-8x %s%s" % (rva, where, src))


main()
