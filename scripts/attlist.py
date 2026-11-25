"""Walk the character attachment list (0x00847200 head, next +0xE0; recomp_manual.c
"character attachments") of a running --debug game through its command pipe and print each
record: character id, model, bone, type, per-frame callback, animation filter, frames left.
  .venv\\Scripts\\python -P scripts\\attlist.py [pipe]   (default conker-test)
Reads only (peek). Found the rack scene's frying pan left on Conker's hand (Extras, Bat mode)."""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import conker_cmd  # noqa: E402

PIPE = sys.argv[1] if len(sys.argv) > 1 else 'conker-test'
p = conker_cmd.Pipe(PIPE)


def read(a, n):
    out = b''
    while n > 0:
        k = min(256, n)
        ok, lines = p.run('peek 0x%08X %d' % (a, k))
        if not ok:
            raise SystemExit('peek failed at 0x%08X: %s' % (a, lines))
        for ln in lines:
            m = re.match(r'\s*0x([0-9A-Fa-f]{8})((?: [0-9A-Fa-f]{2})+)', ln)
            if m:
                out += bytes(int(x, 16) for x in m.group(2).split())
        a += k
        n -= k
    return out


def u32(a):
    return struct.unpack('<I', read(a, 4))[0]


head = u32(0x00847200)
print('head 0x%08X' % head)
a, n = head, 0
while a and n < 200:
    r = read(a, 0xF0)
    cid, model = r[0], struct.unpack_from('<H', r, 2)[0]
    bone = r[4]
    typ = struct.unpack_from('<H', r, 0x0A)[0]
    cb = r[0x0C]
    anims = struct.unpack_from('<4H', r, 0x10)
    frames = struct.unpack_from('<H', r, 0x26)[0]
    print('0x%08X char %02X model %02X bone %u type %u cb %u anims %s frames %04X' %
          (a, cid, model, bone, typ, cb, ','.join(str(x) for x in anims), frames))
    a = struct.unpack_from('<I', r, 0xE0)[0]
    n += 1
