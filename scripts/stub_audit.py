r"""
Audit recomp_stubs_unresolved.c against the game's real code.

Every "sub_X(void) { g_esp += N; }" stub stands for an address the lifted code
calls or tail-jumps to that was never made a function. A stub only pops the
return address (plus a ret N's bytes), so it is right only when the code at X
is a bare ret / ret N; anything else (an epilogue, a loop head, a jump table)
is skipped whenever that path runs. 2026-10-16: 4 of 313 were right; the rare
actor-update crash was 0x00305D9C (pop ebp; pop esi; pop ebx; ret).

  .venv\Scripts\python scripts\stub_audit.py <default.xbe> src\recomp\gen\recomp_stubs_unresolved.c

Prints the counts and, per wrong stub, the first instructions at its address.
"""
import re, struct, sys
import capstone

xbe, stubs = sys.argv[1], sys.argv[2]
b = open(xbe, 'rb').read()
base = struct.unpack_from('<I', b, 0x104)[0]
nsec = struct.unpack_from('<I', b, 0x11C)[0]
sh = struct.unpack_from('<I', b, 0x120)[0] - base
secs = []
for i in range(nsec):
    fl, va, vs, ra, rs, name_va = struct.unpack_from('<6I', b, sh + i * 56)
    secs.append((va, va + rs, ra))

def read(va, k):
    for a, e, ra in secs:
        if a <= va < e:
            return b[ra + va - a:ra + va - a + k]
    return None

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
ok, bad = [], []
for line in open(stubs):
    m = re.match(r'void sub_([0-9A-F]{8})\(void\) \{ g_esp \+= (\d+);', line)
    if not m:
        continue
    va, pop = int(m.group(1), 16), int(m.group(2))
    code = read(va, 64)
    if code is None:
        bad.append((va, pop, 'not in a section'))
        continue
    ins = []
    for i in md.disasm(code, va):
        ins.append(f'{i.mnemonic} {i.op_str}'.strip())
        if i.mnemonic in ('ret', 'jmp') or len(ins) >= 8:
            break
    first = ins[0] if ins else '?'
    if first == 'ret' and pop == 4 or first.startswith('ret ') and pop == 4 + int(first.split()[1], 16):
        ok.append(va)
    else:
        bad.append((va, pop, '; '.join(ins)))
print(f'{len(ok)} stubs are a bare ret (correct), {len(bad)} are not:')
for va, pop, s in bad:
    print(f'  0x{va:08X} (stub pops {pop}): {s}')
