"""Disassemble an Xbox D3D vertex shader's program from the running game.

The game keeps a vertex shader as a handle (object address | 1); the object
holds the program as a push-buffer block for NV097_SET_TRANSFORM_PROGRAM
(method 0x0B00), 4 words an instruction. The decoding follows the toolkit's
dump_program (xboxrecomp/src/kernel/nv2a_vsh_interp.c). Constant numbers are
the hardware's (D3D c0 = c[96]). Example (grass waving program, 2026-11-03):
  .venv\\Scripts\\python scripts\\gmem.py u32 0x006B89A4      -> 74031C91
  .venv\\Scripts\\python scripts\\vshdis.py 0x74031C90
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gmem  # noqa: E402

MAC = "nop mov mul add mad dp3 dph dp4 dst min max slt sge arl mac14 mac15".split()
ILU = "nop mov rcp rcc rsq exp log lit".split()


def f(ins, dw, lo, w):
    return (ins[dw] >> lo) & ((1 << w) - 1)


def src(ins, which):
    if which == 0:
        neg, swz, mux, reg = f(ins, 1, 8, 1), f(ins, 1, 0, 8), f(ins, 2, 26, 2), f(ins, 2, 28, 4)
    elif which == 1:
        neg, swz, mux, reg = f(ins, 2, 25, 1), f(ins, 2, 17, 8), f(ins, 2, 11, 2), f(ins, 2, 13, 4)
    else:
        neg, swz, mux = f(ins, 2, 10, 1), f(ins, 2, 2, 8), f(ins, 3, 28, 2)
        reg = (f(ins, 2, 0, 2) << 2) | f(ins, 3, 30, 2)
    s = "-" if neg else ""
    if mux == 1:
        s += "R%u" % reg
    elif mux == 2:
        s += "v%u" % f(ins, 1, 9, 4)
    else:
        s += "c[%s%u]" % ("a0.x+" if f(ins, 3, 1, 1) else "", f(ins, 1, 13, 8))
    return s + "." + "".join("xyzw"[(swz >> k) & 3] for k in (6, 4, 2, 0))


def mask(m):
    return "".join(c for c, b in zip("xyzw", (8, 4, 2, 1)) if m & b)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    obj = int(sys.argv[1], 0)
    raw = gmem.Game().read(obj, 0x400)
    words = [int.from_bytes(raw[i:i + 4], "little") for i in range(0, len(raw), 4)]
    start = next((i for i, w in enumerate(words)
                  if (w & 0x1FFC) == 0x0B00 and (w >> 18) & 0x7FF
                  and ((w >> 18) & 0x7FF) % 4 == 0), None)
    if start is None:
        print("no program block (method 0x0B00) in the first 0x400 bytes")
        return 1
    print("block at +0x%X: %08X" % (start * 4, words[start]))
    # Longer programs come as several blocks (8 instructions each), each
    # behind its own 0x0B00 header word.
    i, n, left = start + 1, 0, (words[start] >> 18) & 0x7FF
    while i + 3 < len(words):
        if left == 0:
            if (words[i] & 0x1FFC) != 0x0B00:
                print("no block header at +0x%X: %08X" % (i * 4, words[i]))
                break
            left = (words[i] >> 18) & 0x7FF
            i += 1
            continue
        left -= 4
        ins = words[i:i + 4]
        print("%3u %-5s %-4s A=%-16s B=%-16s C=%-16s R%u mac %-4s ilu %-4s out %s%u %-4s%s" % (
            n, MAC[f(ins, 1, 21, 4)], ILU[f(ins, 1, 25, 3)],
            src(ins, 0), src(ins, 1), src(ins, 2),
            f(ins, 3, 20, 4), mask(f(ins, 3, 24, 4)), mask(f(ins, 3, 16, 4)),
            "o" if f(ins, 3, 11, 1) else "c", f(ins, 3, 3, 8), mask(f(ins, 3, 12, 4)),
            " FINAL" if f(ins, 3, 0, 1) else ""))
        if f(ins, 3, 0, 1):
            break
        i, n = i + 4, n + 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
