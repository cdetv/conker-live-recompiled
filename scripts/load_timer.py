"""Time Conker's loads the same way in the .exe and in xemu.

Polls D3D's count of flips done (miniport 0x00549168 + 0x1BC; see
docs/conker-notes.md, d3d_flip_if_due) and reports every stretch where no new
frame reached the screen for --gap seconds or more. Both sides read the same
guest dword, so the numbers compare directly:

    .exe:  the running conker.exe, read from outside with
           ReadProcessMemory (guest VA + 0x10000 = host address; no switch needed)
    xemu:  halts xemu over its GDB stub for a moment each poll (xemu_gdb.py launch first)

    .venv\\Scripts\\python scripts\\load_timer.py exe  [--stop-file F] [--duration S]
    .venv\\Scripts\\python scripts\\load_timer.py xemu [--stop-file F] [--duration S]

Every poll goes to _work/loadtime/<side>_<stamp>.csv (seconds, flips, vblanks);
the gaps and a per-second fps line are printed and logged next to it.
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xemu_gdb import Gdb, _rle_decode  # noqa: E402

FLIPS_VA = 0x00549168 + 0x1BC
VBLANKS_VA = 0x00549168 + 0x1C0
HOST_OFFSET = 0x10000          # xbox_MemoryLayoutInit maps guest VA v at host v + 0x10000
EXE_NAME = "conker.exe"
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(REPO, "_work", "loadtime")

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
PROCESS_VM_READ = 0x0010
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


def find_exe_pid(timeout: float) -> int:
    psapi = ctypes.WinDLL("psapi")
    deadline = time.monotonic() + timeout
    while True:
        pids = (wt.DWORD * 4096)()
        got = wt.DWORD()
        psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(got))
        for pid in pids[:got.value // 4]:
            h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not h:
                continue
            buf = ctypes.create_unicode_buffer(1024)
            n = wt.DWORD(1024)
            ok = k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
            k32.CloseHandle(h)
            if ok and buf.value.lower().endswith("\\" + EXE_NAME):
                return pid
        if time.monotonic() > deadline:
            raise SystemExit(f"no {EXE_NAME} running")
        time.sleep(0.5)


class ExeSide:
    def __init__(self, timeout: float):
        self.pid = find_exe_pid(timeout)
        self.h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_LIMITED_INFORMATION,
                                 False, self.pid)
        if not self.h:
            raise SystemExit(f"OpenProcess({self.pid}) failed: {ctypes.get_last_error()}")

    def read(self):
        buf = ctypes.create_string_buffer(8)
        got = ctypes.c_size_t()
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(FLIPS_VA + HOST_OFFSET), buf, 8,
                                   ctypes.byref(got))
        if not ok:
            return None                  # process gone or memory not mapped yet
        return struct.unpack("<II", buf.raw)

    def close(self):
        k32.CloseHandle(self.h)


class XemuSide:
    # Connecting to the stub pauses xemu by itself; an interrupt sent to a paused
    # xemu gets no reply. So the first read follows the connect's own stop, and
    # only later reads interrupt (each reply then comes at once).
    # A stop reply can also arrive late (a stop left over from an earlier
    # connection), so replies that are stops are skipped where data is expected.
    def __init__(self, timeout: float):
        self.gdb = Gdb(timeout=10)
        self.halted = True               # the connect itself paused xemu

    def _reply(self, packet: str) -> str:
        self.gdb._send_packet(packet)
        deadline = time.monotonic() + 5
        while True:
            r = self.gdb._recv_packet(deadline)
            if not r.startswith(("T", "S")):
                return r

    def read(self):
        if not self.halted:
            self.gdb.sock.sendall(b"\x03")   # its stop reply is skipped by _reply
        self.halted = False
        try:
            r = self._reply(f"m{FLIPS_VA:x},8")
        finally:
            self.gdb._send_packet("c")   # run on; no reply until the next stop
        try:
            return struct.unpack("<II", bytes.fromhex(_rle_decode(r)))
        except ValueError:
            return None

    def close(self):
        try:
            self.gdb.sock.close()
        except OSError:
            pass


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("side", choices=["exe", "xemu"])
    ap.add_argument("--interval", type=float, help="seconds between polls "
                    "(default 0.05 exe, 0.25 xemu)")
    ap.add_argument("--gap", type=float, default=0.5,
                    help="report stretches with no new frame this long or longer (default 0.5)")
    ap.add_argument("--duration", type=float, default=0, help="stop after S seconds (0 = no limit)")
    ap.add_argument("--stop-file", help="stop when this file exists")
    ap.add_argument("--wait", type=float, default=120, help="seconds to wait for the .exe to start")
    args = ap.parse_args()
    interval = args.interval or (0.05 if args.side == "exe" else 0.25)

    os.makedirs(OUT_DIR, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    base = os.path.join(OUT_DIR, f"{args.side}_{stamp}")
    csv = open(base + ".csv", "w")
    txt = open(base + ".txt", "w")
    csv.write("seconds,flips,vblanks\n")

    def out(line=""):
        print(line, flush=True)
        txt.write(line + "\n")
        txt.flush()

    side = ExeSide(args.wait) if args.side == "exe" else XemuSide(args.wait)
    out(f"load_timer {args.side}  poll {interval}s  gap >= {args.gap}s  -> {base}.csv")
    t0 = time.monotonic()
    last_flips = None
    last_change = None          # time the flip count last moved
    sec_start, sec_flips = 0.0, None
    misses = 0
    try:
        while True:
            now = time.monotonic() - t0
            if args.duration and now > args.duration:
                break
            if args.stop_file and os.path.exists(args.stop_file):
                break
            v = side.read()
            if v is None:
                misses += 1
                if args.side == "exe" and misses > 40:
                    out(f"{now:8.2f}  process gone")
                    break
                time.sleep(interval)
                continue
            misses = 0
            flips, vbl = v
            csv.write(f"{now:.3f},{flips},{vbl}\n")
            if last_flips is None:
                last_flips, last_change, sec_flips = flips, now, flips
            elif flips != last_flips:
                stalled = now - last_change
                if stalled >= args.gap:
                    out(f"{last_change:8.2f}  no new frame for {stalled:6.2f} s "
                        f"(flips {last_flips} -> {flips})")
                last_flips, last_change = flips, now
            if now - sec_start >= 1.0:
                out(f"{now:8.2f}  {(flips - sec_flips) / (now - sec_start):5.1f} fps")
                sec_start, sec_flips = now, flips
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    finally:
        now = time.monotonic() - t0
        if last_change is not None and now - last_change >= args.gap:
            out(f"{last_change:8.2f}  no new frame for {now - last_change:6.2f} s (still, at end)")
        side.close()
        csv.close()
        txt.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
