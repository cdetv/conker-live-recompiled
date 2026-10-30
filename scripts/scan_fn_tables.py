"""Find function starts the disassembler missed because they are only reached through
a table of function pointers (an indirect call to one fails as [ICALL] Failed to resolve).

Reads every dword in the XBE's data sections. A candidate is a value that points into a
code section, is 16-byte aligned, lies outside every function in functions.json (a
"gap", so seeding it cannot cut a known function short), is not already a hand seed,
and sits next to at least one known function start (it is in a table).

With --inside, also looks inside known functions for a pointed-to address that follows
a ret / int3 padding (the disassembler ran one function on into the next, so the second
start was never seen; 2026-10-16, the fan blades' handler 0x00167530 inside sub_001674E0).

With --code, also reads the constants in every known function's instructions (mov [x], imm,
mov reg, imm, push imm): a pointer that code stores into a callback table at run time is
never in a data section (2026-10-21, 0x00359E00 stored by `mov [0x0084D338], imm` at
0x004805B6; unlifted, every multiplayer match crashed after its opening movie).

Prints each candidate with its table address and first instructions. Judge them by eye:
real ones open like a function, right after a ret/jmp/int3 padding/switch table; text
or number tables that happen to look like addresses do not. Add real ones to
seeds/hand_seeds.json with a reason.

Usage (repo root):
  .venv\\Scripts\\python scripts\\scan_fn_tables.py <default.xbe> _work\\pipeline\\disasm\\functions.json seeds\\hand_seeds.json
"""
import bisect
import json
import struct
import sys

import capstone

CODE = {'.text', 'D3DX', 'XNET', 'XONLINE', 'DSOUND', 'XMV', 'WMADEC', 'XGRPH', 'D3D',
        'PAGELK', 'XACTENG', 'XPP'}
DATA = {'.rdata', '.data', 'XON_RD', '.data1'}  # section flags are unreliable in this XBE


def main(xbe_path, funcs_path, seeds_path, inside=False, in_code=False):
    b = open(xbe_path, 'rb').read()
    base = struct.unpack_from('<I', b, 0x104)[0]
    nsec = struct.unpack_from('<I', b, 0x11C)[0]
    sh = struct.unpack_from('<I', b, 0x120)[0] - base
    secs = []
    for i in range(nsec):
        fl, va, vs, ra, rs, name_va = struct.unpack_from('<6I', b, sh + i * 56)
        no = name_va - base
        secs.append((b[no:b.index(b'\0', no)].decode(), va, ra, rs))
    code = [(va, va + rs, ra) for n, va, ra, rs in secs if n in CODE]

    def read(va, k):
        for a, e, ra in code:
            if a <= va < e:
                return b[ra + va - a:ra + va - a + k]

    def to_int(x):
        return int(x, 16) if isinstance(x, str) else x

    ranges = sorted((to_int(f['start']), to_int(f['end'])) for f in json.load(open(funcs_path)))
    starts = {s for s, _ in ranges}
    first = [s for s, _ in ranges]
    seeds = {int(s['start'], 16) for s in json.load(open(seeds_path))}

    def in_gap(v):
        i = bisect.bisect_right(first, v) - 1
        return i < 0 or v >= ranges[i][1]

    def after_end(v):
        """v follows a ret, ret n or int3 padding: a plausible start inside a function."""
        prev = read(v - 3, 3)
        return prev is not None and (prev[2] in (0xC3, 0xCC) or prev[0] == 0xC2)

    def wanted(v):
        return in_gap(v) or (inside and after_end(v))

    hits = {}
    for n, va, ra, rs in secs:
        if n not in DATA:
            continue
        blob = b[ra:ra + rs]
        for off in range(0, len(blob) - 3, 4):
            v = struct.unpack_from('<I', blob, off)[0]
            if v % 16 or v in starts or v in seeds or read(v, 1) is None or not wanted(v):
                continue
            near = [struct.unpack_from('<I', blob, off + 4 * k)[0] for k in (-2, -1, 1, 2)
                    if 0 <= off + 4 * k <= len(blob) - 4]
            if any(x in starts for x in near):
                hits.setdefault(v, []).append(va + off)

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    if in_code:
        md.detail = True
        for s, e in ranges:
            blob = read(s, e - s)
            if not blob:
                continue
            for ins in md.disasm(blob, s):
                if ins.mnemonic not in ('mov', 'push'):
                    continue
                for op in ins.operands:
                    if op.type != capstone.x86.X86_OP_IMM:
                        continue
                    v = op.imm & 0xFFFFFFFF
                    if v % 16 or v in starts or v in seeds or read(v, 1) is None or not wanted(v):
                        continue
                    hits.setdefault(v, []).append(ins.address)

    for v, refs in sorted(hits.items()):
        print('0x%08X  referenced at %s  previous bytes %s%s' % (
            v, ' '.join('0x%08X' % r for r in refs[:4]), read(v - 6, 6).hex(),
            '' if in_gap(v) else '  (inside a known function)'))
        for i, ins in enumerate(md.disasm(read(v, 32), v)):
            if i >= 4:
                break
            print('    0x%08X %s %s' % (ins.address, ins.mnemonic, ins.op_str))
    print('%d candidates' % len(hits))


if __name__ == '__main__':
    main(*sys.argv[1:4], inside='--inside' in sys.argv[4:], in_code='--code' in sys.argv[4:])
