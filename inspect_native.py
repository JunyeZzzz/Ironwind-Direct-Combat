"""Temporary local disassembly helper for the installed Kenshi executable."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).with_name("analysis_deps")))
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

data = Path(sys.argv[1]).read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
count = struct.unpack_from("<H", data, pe + 6)[0]
opt_size = struct.unpack_from("<H", data, pe + 20)[0]
base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]
sec = pe + 24 + opt_size
sections = []
for i in range(count):
    off = sec + i * 40
    name = data[off:off+8].split(b"\0")[0].decode("ascii", "replace")
    vsize, va, rsize, raw = struct.unpack_from("<IIII", data, off + 8)
    sections.append((name, va, max(vsize, rsize), raw))

md = Cs(CS_ARCH_X86, CS_MODE_64)
for arg in sys.argv[2:]:
    if arg.startswith("calls:"):
        target_rva = int(arg.split(":", 1)[1], 0)
        target_va = base + target_rva
        print("\n direct callers of RVA", hex(target_rva), "VA", hex(target_va))
        for name, va, size, raw in sections:
            if name != ".text":
                continue
            for ins in md.disasm(data[raw:raw + size], base + va):
                if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
                    try:
                        destination = int(ins.op_str, 16)
                    except ValueError:
                        continue
                    if destination == target_va:
                        print(f"{ins.address:016X} RVA {hex(ins.address - base)}")
        continue
    rva, length = (int(x, 0) for x in arg.split(":"))
    match = next((s for s in sections if s[1] <= rva < s[1] + s[2]), None)
    if not match:
        print(hex(rva), "not mapped")
        continue
    offset = match[3] + rva - match[1]
    print("\n", match[0], "RVA", hex(rva), "VA", hex(base + rva))
    for ins in md.disasm(data[offset:offset+length], base + rva):
        print(f"{ins.address:016X}: {ins.mnemonic:7s} {ins.op_str}")
