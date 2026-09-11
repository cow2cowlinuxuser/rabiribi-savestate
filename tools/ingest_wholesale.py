#!/usr/bin/env python3
"""Copy one Ghidra C function into wholesale/lifted and emit DAT BSS + FUN stubs.

Missing callees halt on screen via host_trap so the Cube names the next ingest.
Never maps the PE's 33 MB .data hole; each DAT_* is a packed named symbol.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GHIDRA = Path(
    r"F:\Ragnarok Battle Offline + Ex1-3\EXTRACTION\re\RBO_Ex3\09_functions_c"
)
DEFAULT_SYMS = Path(
    r"F:\Ragnarok Battle Offline + Ex1-3\EXTRACTION\re\RBO_Ex3\03_symbols.txt"
)
DEFAULT_STRS = Path(
    r"F:\Ragnarok Battle Offline + Ex1-3\EXTRACTION\re\RBO_Ex3\06_strings.txt"
)

FUN_USE = re.compile(r"\b((?:thunk_)?FUN_[0-9a-fA-F]+|LAB_[0-9a-fA-F]+)\b")
DAT_USE = re.compile(r"\b(_?DAT_[0-9a-fA-F]+)\b")
DAT_ASSIGN = re.compile(r"(?<!&)\b(_?DAT_[0-9a-fA-F]+)\s*=")
STR_USE = re.compile(r"\b(s_[A-Za-z0-9_]+)\b")
DEF_FUN_FULL = re.compile(
    r"^(?:(?:static|const)\s+)*"
    r"((?:undefined\d*|void|int|char|byte|uint|ushort|short|long|bool|"
    r"float|double|undefined|DWORD|UINT|HANDLE)(?:\s*\*)*)"
    r"\s+((?:thunk_)?FUN_[0-9a-fA-F]+|LAB_[0-9a-fA-F]+)\s*\(",
    re.M,
)
VA_SUFFIX = re.compile(r"_([0-9a-fA-F]{6,8})$")
SKIP_C = {"dat_bss.c", "fun_stubs.c"}
DAT_SIZE_OVERRIDE = {
    "DAT_004926d0": 0x30C,  # 3 x 0x104 path slots; Ghidra loop uses PE end 0x4929dc
    "DAT_00bc8cc0": 0x80,  # HWND lives at +0x78
    "DAT_00497568": 0xC,  # 3 player AssignController slots (Ghidra also named +4 +8)
    "DAT_00bc8cb0": 0xC,  # 3 player device indexes
    "DAT_00aadc90": 0x24,  # PAC manager scratch (9 dwords)
    "DAT_00adebe8": 0xAFC,  # ArenaInfo blob; 0x2bf dwords then 700 x 0xffffffff
    "DAT_00490a30": 0xD98,  # AreaRecordTime.rv4 payload; next Ghidra label is +4
    "DAT_008080f8": 0x200,  # 8 x 4x4 camera matrices; next Ghidra label is +0x40
    "DAT_00bc7b10": 0xA8,  # 3 x 0x38 timer slots; next label 00bc7bb8
    "DAT_02540238": 0x38,  # sprite-drain timer blob; next Ghidra label is +0x38
    "DAT_022e84ec": 0x3858,  # 6 x 0x974 overlay slots; next label is inside slot 0
    "DAT_007662d0": 0xC00,  # 0x100 x 3-dword free-list; next label is 12 bytes short
    "DAT_00bc7f30": 0xC,  # camera triple; Ghidra also named +0xc +0x10
    "DAT_00bc7f48": 0xC,  # camera triple restore
    "DAT_00add7e0": 0xC,  # camera xyz; Ghidra also named +4 +8
    "DAT_00add800": 0xC,  # camera xyz backup
    "DAT_022e830c": 0x140,  # 2 x 0xA0 projectile/effect heads
    "DAT_004d7e08": 256 * 0x10c,  # draw-packet pool (retail 10000)
    "DAT_0048d530": 16,  # DrawIndexedPrimitive WORD indices 0,1,2,3
    "DAT_00805da0": 4,  # IDirect3D7 pointer (next PE label is +16)
    "DAT_004a63e4": 4,  # Lobby.Img surface
    "DAT_0048a230": 0x370,  # 0x16 * 0x28 Lobby.Img sprite bank
    "DAT_0049ceac": 64,  # stsel unlock flags (2 bytes/stage)
}
HOST_SYMS = {
    "CoInitialize",
    "CoUninitialize",
    "CoCreateInstance",
    "LoadIconA",
    "MessageBoxA",
    "DestroyWindow",
    "CreateWindowExA",
    "PeekMessageA",
    "DispatchMessageA",
    "wsprintfA",
    "CreateFileA",
    "ReadFile",
    "WriteFile",
    "CloseHandle",
    "GetFileSize",
    "DirectDrawCreateEx",
    "DirectSoundCreate8",
    "DirectInput8Create",
    "SendMessageA",
    "ImmGetContext",
    "ImmSetOpenStatus",
    "ImmAssociateContext",
    "ImmGetDefaultIMEWnd",
    "ImmReleaseContext",
    "PathFileExistsA",
}


def parse_hex_addrs(symbols: Path) -> list[int]:
    addrs: set[int] = set()
    if not symbols.is_file():
        return []
    for line in symbols.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith("#") or "+" in line.split("\t")[0]:
            continue
        parts = line.split("\t")
        if not parts:
            continue
        if len(parts) > 1 and "+" in parts[1]:
            continue
        tok = parts[0].strip()
        tok = tok.split(":")[-1]
        try:
            addrs.add(int(tok, 16))
        except ValueError:
            continue
    return sorted(addrs)


def dat_size(va: int, addrs: list[int]) -> int:
    import bisect

    i = bisect.bisect_right(addrs, va)
    if i < len(addrs):
        n = addrs[i] - va
        if 1 <= n <= 4096:
            return n
    return 4


def parse_strings(path: Path) -> dict[int, str]:
    out: dict[int, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        parts = line.split("\t")
        if len(parts) < 3 or parts[1] != "string":
            continue
        try:
            va = int(parts[0], 16)
        except ValueError:
            continue
        raw = parts[2]
        if raw.startswith('"') and raw.endswith('"'):
            raw = raw[1:-1]
        try:
            text = raw.encode("utf-8").decode("unicode_escape")
        except Exception:
            text = raw.replace("\\r", "\r").replace("\\n", "\n").replace("\\\\", "\\")
        out[va] = text
    return out


def c_string(s: str) -> str:
    b = s.encode("latin-1", errors="replace")
    parts = ['"']
    for ch in b:
        if ch == ord("\\"):
            parts.append("\\\\")
        elif ch == ord('"'):
            parts.append('\\"')
        elif ch == ord("\n"):
            parts.append("\\n")
        elif ch == ord("\r"):
            parts.append("\\r")
        elif ch == ord("\t"):
            parts.append("\\t")
        elif 32 <= ch < 127:
            parts.append(chr(ch))
        else:
            parts.append(f"\\x{ch:02x}")
    parts.append('"')
    return "".join(parts)


def find_ghidra(cdir: Path, name: str) -> Path | None:
    hits = sorted(cdir.glob(f"{name}_*.c")) + sorted(cdir.glob(f"{name}.c"))
    return hits[0] if hits else None


def wrap_function(src: Path, dest: Path) -> None:
    if dest.is_file():
        head = dest.read_text(encoding="utf-8", errors="replace")[:240]
        if "wholesale-host" in head:
            print(f"keep handwritten {dest.name}")
            return
    body = src.read_text(encoding="utf-8", errors="replace")
    if body.lstrip().startswith('#include "ghidra_preamble.h"'):
        dest.write_text(body, encoding="utf-8")
        return
    dest.write_text(
        f'#include "ghidra_preamble.h"\n\n'
        f"/* ingested from {src.name} */\n"
        f"{body.lstrip()}",
        encoding="utf-8",
        newline="\n",
    )


def scan_lifted(
    lifted: Path,
) -> tuple[dict[str, str], set[str], set[str], set[str], set[str]]:
    defined_ret: dict[str, str] = {}
    funs: set[str] = set()
    dats: set[str] = set()
    assigned: set[str] = set()
    strs: set[str] = set()
    for p in sorted(lifted.glob("*.c")):
        if p.name in SKIP_C:
            continue
        t = p.read_text(encoding="utf-8", errors="replace")
        for ret, name in DEF_FUN_FULL.findall(t):
            defined_ret[name] = ret
        funs.update(FUN_USE.findall(t))
        dats.update(DAT_USE.findall(t))
        assigned.update(DAT_ASSIGN.findall(t))
        strs.update(STR_USE.findall(t))
    funs -= HOST_SYMS
    return defined_ret, funs, dats, assigned, strs


def dat_ctype(size: int, name: str) -> tuple[str, str]:
    if size <= 1:
        return "undefined", f"{name}"
    if size == 2:
        return "undefined2", f"{name}"
    if size <= 4:
        return "undefined4", f"{name}"
    return "undefined", f"{name}[{size}]"


def va_of_dat(name: str) -> int | None:
    m = VA_SUFFIX.search(name)
    if not m:
        return None
    return int(m.group(1), 16)


def emit_files(
    lifted: Path,
    defined_ret: dict[str, str],
    funs: set[str],
    dats: set[str],
    assigned: set[str],
    strs: set[str],
    addrs: list[int],
    strings: dict[int, str],
) -> None:
    defined = set(defined_ret)
    stubs = sorted(funs - defined)
    dat_lines_h: list[str] = []
    dat_lines_c: list[str] = []
    for name in sorted(dats):
        va = va_of_dat(name)
        size = dat_size(va, addrs) if va is not None else 4
        if name in assigned:
            size = min(size, 4)
        if name in DAT_SIZE_OVERRIDE:
            size = DAT_SIZE_OVERRIDE[name]
        ty, decl = dat_ctype(size, name)
        dat_lines_h.append(f"extern {ty} {decl};")
        dat_lines_c.append(f"{ty} {decl};")

    str_h: list[str] = []
    str_c: list[str] = []
    for name in sorted(strs):
        va = va_of_dat(name)
        val = strings.get(va, "") if va is not None else ""
        str_h.append(f"extern char {name}[];")
        str_c.append(f"char {name}[] = {c_string(val)};")

    proto: list[str] = []
    for name in sorted(defined):
        proto.append(f"{defined_ret[name]} {name}();")
    for name in stubs:
        proto.append(f"undefined4 {name}();")

    header = "\n".join(
        [
            "/* Generated by tools/ingest_wholesale.py. Do not edit. */",
            "#ifndef LIFTED_SYMS_H",
            "#define LIFTED_SYMS_H",
            "",
            '#include "ghidra_types.h"',
            "",
            *proto,
            "",
            *dat_lines_h,
            *str_h,
            "",
            "#endif",
            "",
        ]
    )
    (lifted / "lifted_syms.h").write_text(header, encoding="utf-8", newline="\n")

    dat_c = "\n".join(
        [
            "/* Generated by tools/ingest_wholesale.py. Do not edit. */",
            '#include "ghidra_preamble.h"',
            "",
            *dat_lines_c,
            *str_c,
            "",
        ]
    )
    (lifted / "dat_bss.c").write_text(dat_c, encoding="utf-8", newline="\n")

    stub_fn = []
    for name in stubs:
        stub_fn.append(
            f"undefined4 {name}()\n"
            f"{{\n"
            f'\tstub_halt("{name}");\n'
            f"\treturn 0;\n"
            f"}}\n"
        )
    stubs_c = "\n".join(
        [
            "/* Generated by tools/ingest_wholesale.py. Do not edit. */",
            '#include "ghidra_preamble.h"',
            '#include "host.h"',
            '#include "rbo_input.h"',
            '#include "escape.h"',
            "#include <gccore.h>",
            "",
            "static void stub_halt(const char *name)",
            "{",
            "	host_trap(name);",
            "	for (;;) {",
            "		host_log_draw();",
            "		host_present_efb();",
            "		rbo_input_poll();",
            "		if (rbo_input_held(PAD_BUTTON_START) && escape_available())",
            "			escape_now();",
            "	}",
            "}",
            "",
            *stub_fn,
        ]
    )
    (lifted / "fun_stubs.c").write_text(stubs_c, encoding="utf-8", newline="\n")
    print(
        f"lifted defined={len(defined_ret)} stubs={len(stubs)} "
        f"dat={len(dats)} str={len(strs)}"
    )
    if stubs:
        print("next trap (first callee stub):", stubs[0] if False else "")
        # Print in source order of FUN_00440690: FUN_00440b50 is the first call.
        prefer = "FUN_00440b50"
        print("stubs:", ", ".join(stubs[:12]) + (" ..." if len(stubs) > 12 else ""))
        if prefer in stubs:
            print(f"first runtime halt should be {prefer}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("funcs", nargs="+", help="FUN_00440690 or Ghidra filename")
    ap.add_argument("--cdir", type=Path, default=DEFAULT_GHIDRA)
    ap.add_argument("--symbols", type=Path, default=DEFAULT_SYMS)
    ap.add_argument("--strings", type=Path, default=DEFAULT_STRS)
    ap.add_argument("--out", type=Path, default=ROOT / "wholesale" / "lifted")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    addrs = parse_hex_addrs(args.symbols)
    strings = parse_strings(args.strings)

    for spec in args.funcs:
        name = Path(spec).stem
        if re.match(r".*_[0-9a-fA-F]{6,8}$", name) and name.startswith("FUN_"):
            # FUN_00440690_00440690 -> FUN_00440690
            parts = name.rsplit("_", 1)
            if len(parts[1]) >= 6:
                name = parts[0] if parts[0].startswith("FUN_") else name
        src = Path(spec) if Path(spec).is_file() else find_ghidra(args.cdir, name)
        if src is None:
            print(f"error: no Ghidra C for {spec}", file=sys.stderr)
            return 1
        dest_name = name if name.endswith(".c") else f"{name}.c"
        wrap_function(src, args.out / dest_name)
        print(f"ingested {src.name} -> {dest_name}")

    defined_ret, funs, dats, assigned, strs = scan_lifted(args.out)
    emit_files(args.out, defined_ret, funs, dats, assigned, strs, addrs, strings)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
