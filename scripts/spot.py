"""Named spots: save where Conker stands, teleport him back there later.

The user walks to a spot (e.g. just before a trigger) and runs `mark`; a
scripted run in the same level then uses `go` and walks on from there.
Spots live in scripts/spots.json (positions only: no game data).

  py -3 scripts/spot.py mark <name> [note...]   save Conker's position + level
  py -3 scripts/spot.py go <name>               teleport him there (same level only)
  py -3 scripts/spot.py list                    the saved spots
  py -3 scripts/spot.py where                   print his position now

Conker = character 0 ([0x0084A5F8]); position +0x78 (x, y, z), copy at +0x30;
level [0x007FA1F8]. `go` writes both for ~0.5 s so the movement code takes
it. Facing is not set yet (unknown field): "forward" after a jump may differ.
A trigger he lands on fires at once (the Sunny Days money did).
"""

from __future__ import annotations

import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gmem  # noqa: E402

SPOTS = Path(__file__).with_name("spots.json")
CHARS = 0x0084A5F8
LEVEL = 0x007FA1F8


def load() -> dict:
    return json.loads(SPOTS.read_text(encoding="utf-8")) if SPOTS.exists() else {}


def conker(g: gmem.Game) -> int:
    c = g.u32(CHARS)
    if not c:
        raise SystemExit("no characters (not in a level?)")
    return c


def pos(g: gmem.Game, c: int) -> tuple[float, float, float]:
    return struct.unpack("<3f", g.read(c + 0x78, 12))


def main(argv: list[str]) -> int:
    if len(argv) < 2 or argv[1] not in ("mark", "go", "list", "where"):
        print(__doc__)
        return 1
    cmd = argv[1]
    spots = load()
    if cmd == "list":
        for name, s in spots.items():
            print(f"{name}: level 0x{s['level']:02X} pos {s['pos']}  {s.get('note', '')}")
        return 0
    g = gmem.Game()
    c = conker(g)
    level = g.u32(LEVEL) & 0xFF
    if cmd == "where":
        print(f"level 0x{level:02X} pos {[round(v, 1) for v in pos(g, c)]}")
        return 0
    if len(argv) < 3:
        raise SystemExit(f"{cmd} needs a name")
    name = argv[2]
    if cmd == "mark":
        p = [round(v, 1) for v in pos(g, c)]
        spots[name] = {"level": level, "pos": p, "note": " ".join(argv[3:]),
                       "date": time.strftime("%Y-%m-%d")}
        SPOTS.write_text(json.dumps(spots, indent=2) + "\n", encoding="utf-8")
        print(f"marked {name}: level 0x{level:02X} pos {p}")
        return 0
    if name not in spots:
        raise SystemExit(f"no spot {name!r} (list shows them)")
    s = spots[name]
    if s["level"] != level:
        raise SystemExit(f"{name} is in level 0x{s['level']:02X}, game is in 0x{level:02X}")
    data = struct.pack("<3f", *s["pos"])
    for _ in range(25):
        g.write(c + 0x78, data)
        g.write(c + 0x30, data)
        time.sleep(0.02)
    time.sleep(0.3)
    print(f"went to {name}; now {[round(v, 1) for v in pos(g, c)]}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
