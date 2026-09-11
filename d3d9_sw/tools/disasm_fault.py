"""Disassemble the code the fault handler dumped out of the running process.

The game executable is encrypted on disk by the Steam wrapper, so it cannot be
disassembled from the file. The handler dumps the decrypted bytes around the
faulting instruction instead, as 'code +0xRVA: XX XX ...' lines in the log.
This reassembles those into one block and disassembles it, marking the
instruction that faulted.

    python tools/disasm_fault.py <log> [faulting_rva_hex]
"""

import re
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_32


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "r", encoding="utf-8", errors="replace") as fh:
        lines = fh.read().splitlines()

    fault_rva = int(sys.argv[2], 16) if len(sys.argv) > 2 else None
    if fault_rva is None:
        for ln in lines:
            m = re.search(r"at pc [0-9A-Fa-f]+ \(\+(0x[0-9a-fA-F]+) in .*\.exe", ln)
            if m:
                fault_rva = int(m.group(1), 16)
    chunks = {}
    for ln in lines:
        m = re.match(r"\s*code \+(0x[0-9a-fA-F]+): ((?:[0-9A-F]{2} )+)", ln)
        if m:
            chunks[int(m.group(1), 16)] = bytes(
                int(b, 16) for b in m.group(2).split())
    if not chunks:
        sys.exit("no 'code +0x...' dump lines in that log")

    start = min(chunks)
    blob = bytearray()
    for rva in sorted(chunks):
        # Rows are contiguous by construction; pad if one was unreadable.
        gap = rva - (start + len(blob))
        if gap > 0:
            blob += b"\x90" * gap
        blob += chunks[rva]

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    print("disassembly of %d bytes from +%#x  (fault at +%#x)\n"
          % (len(blob), start, fault_rva or 0))
    for ins in md.disasm(bytes(blob), start):
        mark = "  <== FAULT" if ins.address == fault_rva else ""
        print("  +%06x  %-24s %s %s%s"
              % (ins.address, ins.bytes.hex(" "), ins.mnemonic, ins.op_str, mark))


main()
