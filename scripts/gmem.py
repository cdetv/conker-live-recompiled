"""Read and poke the running game's guest memory from outside (no rebuild).

Guest VA v lives at host v + 0x10000 in conker.exe (heap 0x74xxxxxx
included). For finding which value a control moves: pause the game in the
spot, snap, move the control (RECOMP_PAD_LIVE), snap again, diff
(conker-notes 2026-10-27, Tankus; 2026-10-28, beehive gun).

  py -3 scripts/gmem.py u32 0x008470DC [count]      words (hex) and floats
  py -3 scripts/gmem.py chain 0x008470DC +4 +0x3E0  follow pointers, print each
  py -3 scripts/gmem.py snap <out.bin> <va> <len>   save a block
  py -3 scripts/gmem.py diff <a.bin> <b.bin> <va>   changed words (as floats too)
  py -3 scripts/gmem.py f32w <va> <value>           write a float
  py -3 scripts/gmem.py u32w <va> <value>           write a word
"""

from __future__ import annotations

import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys

HOST_OFFSET = 0x10000
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p,
                                  ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = k32.ReadProcessMemory.argtypes


def game_pid() -> int:
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq conker.exe",
                          "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.strip('"').split('","')
        if len(parts) > 1 and parts[0].lower() == "conker.exe":
            return int(parts[1])
    raise SystemExit("conker.exe is not running")


class Game:
    def __init__(self):
        self.h = k32.OpenProcess(0x0010 | 0x0020 | 0x0008 | 0x0400, False, game_pid())
        if not self.h:
            raise SystemExit(f"OpenProcess failed: {ctypes.get_last_error()}")

    def read(self, va: int, n: int) -> bytes:
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.h, ctypes.c_void_p(va + HOST_OFFSET), buf, n,
                                     ctypes.byref(got)):
            raise SystemExit(f"read 0x{va:08X} failed: {ctypes.get_last_error()}")
        return buf.raw

    def write(self, va: int, data: bytes) -> None:
        got = ctypes.c_size_t()
        if not k32.WriteProcessMemory(self.h, ctypes.c_void_p(va + HOST_OFFSET), data,
                                      len(data), ctypes.byref(got)):
            raise SystemExit(f"write 0x{va:08X} failed: {ctypes.get_last_error()}")

    def u32(self, va: int) -> int:
        return struct.unpack("<I", self.read(va, 4))[0]


def num(s: str) -> int:
    return int(s, 0)


def show_words(base: int, data: bytes) -> None:
    for i in range(0, len(data) - 3, 4):
        w, = struct.unpack_from("<I", data, i)
        f, = struct.unpack_from("<f", data, i)
        print(f"0x{base + i:08X} +0x{i:03X}: {w:08X}  {f:.6g}")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 1
    cmd, args = argv[1], argv[2:]
    if cmd == "diff":
        a, b = open(args[0], "rb").read(), open(args[1], "rb").read()
        base = num(args[2])
        for i in range(0, min(len(a), len(b)) - 3, 4):
            if a[i:i + 4] != b[i:i + 4]:
                wa, = struct.unpack_from("<I", a, i)
                wb, = struct.unpack_from("<I", b, i)
                fa, = struct.unpack_from("<f", a, i)
                fb, = struct.unpack_from("<f", b, i)
                print(f"0x{base + i:08X} +0x{i:03X}: {wa:08X} -> {wb:08X}"
                      f"  ({fa:.6g} -> {fb:.6g})")
        return 0
    g = Game()
    if cmd == "u32":
        va = num(args[0])
        show_words(va, g.read(va, 4 * (num(args[1]) if len(args) > 1 else 1)))
    elif cmd == "chain":
        va = num(args[0])
        for step in args[1:]:
            va = g.u32(va + num(step))
            print(f"{step} -> 0x{va:08X}")
            if not va:
                break
    elif cmd == "snap":
        open(args[0], "wb").write(g.read(num(args[1]), num(args[2])))
    elif cmd == "f32w":
        g.write(num(args[0]), struct.pack("<f", float(args[1])))
    elif cmd == "u32w":
        g.write(num(args[0]), struct.pack("<I", num(args[1]) & 0xFFFFFFFF))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
