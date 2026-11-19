"""Package reader, Python twin of src/caff.c (to-do 43): reads Conker's
packages (Rare's CAFF28 archives, the .rbm / .rba files under dvddata\\aid).

A second reader written apart from the C one, so each checks the other
(tests/caff). It never unpacks: a packed package's sections need the game's
own decompressor (conker.exe --unpack-all writes unpacked copies). Output
made from game data stays in _work (SPEC 3.3).

  python scripts/caff.py raw <dvddata\\aid dir> <out.json>    headers of all, TOC of the unpacked ones
  python scripts/caff.py dump <unpacked dir> <out.json>       TOC of every package
  python scripts/caff.py compare <a.json> <b.json>            the C test's JSON against this one's
  python scripts/caff.py checkloose <unpacked package> <dir>  the C writer's loose files (caff_test loose)
  python scripts/caff.py toc <unpacked package>               files, parts, directory
  python scripts/caff.py res <unpacked package> <name> [n]    one resource: files, pointers, hex
  python scripts/caff.py inventory <unpacked dir> [out.json]  every package's directory with sizes
  python scripts/caff.py invcompare <a.json> <b.json>         two inventories (assets\\inventory.json, caff_test's)

Format (header offsets): +0x14 checksum (ELF hash of the 0x180 header, bytes
signed, its own 4 as 0); +0x18 files, +0x1C parts, +0x20 / +0x24 two tables
(empty), +0x28 relocation records, +0x2C relocation offsets, +0x34 TOC offset
in .data, +0x38 first section's file offset, +0x3D sections, +0x3E packed
flag; per section (0x28 bytes from +0x40) name, +0x10 unpacked size, +0x24
stored size. Packed = the flag set AND stored != unpacked (two packages carry
the flag over a section stored as it is). A packed section is blocks
{u32 unpacked, u32 stored, bytes}. TOC: u32 name block size k; if k, a u32
offset per file, then k bytes of names; parts (14 bytes: u32 file, u32 offset
in its section, u32 size, u8 section 1 / 2, u8 log2 alignment); the two empty
tables (12 / 4 bytes each); relocation records {u32 source part, u32 target
part, u32 count}; their offsets (u32 each, in the source part). A zpackage's
part 1 is its directory: +0x20 count, then {name offset, pointer} pairs, both
relocated (the layout after the Berri research's scripts/berri_pkg.py and
Txukie's conker_caff.c, MIT).
"""
import json
import os
import struct
import sys
from collections import Counter, defaultdict

MAGIC = b"CAFF28.01.05.0031"
HEADER = 0x180
MAX_SECTIONS = 4
MAX_SIZE = 1024 << 20
MAX_ALIGN = 12


class CaffError(Exception):
    def __init__(self, kind, why=""):
        super().__init__("%s: %s" % (kind, why) if why else kind)
        self.kind = kind


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def checksum(h):
    c = 0
    for i in range(HEADER):
        b = 0 if 0x14 <= i < 0x18 else (h[i] - 256 if h[i] > 127 else h[i])
        c = ((c << 4) + b) & 0xFFFFFFFF
        t = c & 0xF0000000
        if t:
            c ^= t | (t >> 24)
    return c


def cstr(b, o, end):
    e = b.find(b"\0", o, end)
    if e < 0:
        raise CaffError("TOC", "a string runs past its part")
    return b[o:e].decode("latin-1")


def read_header(b):
    """Header of a package as stored: dict, or CaffError."""
    if len(b) < len(MAGIC) or b[:len(MAGIC)] != MAGIC:
        raise CaffError("MAGIC")
    if len(b) < HEADER:
        raise CaffError("HEADER", "file shorter than its header")
    if checksum(b) != u32(b, 0x14):
        raise CaffError("HEADER", "checksum")
    nsec, flag, data_off = b[0x3D], b[0x3E] != 0, u32(b, 0x38)
    if not 1 <= nsec <= MAX_SECTIONS:
        raise CaffError("HEADER", "section count")
    if not HEADER <= data_off <= len(b):
        raise CaffError("HEADER", "first section outside the file")
    h = {"flag": int(flag), "data_off": data_off, "sec": []}
    at = data_off
    for i in range(nsec):
        s = 0x40 + 0x28 * i
        name = b[s:s + 8].split(b"\0")[0].decode("latin-1")
        unp, sto = u32(b, s + 0x10), u32(b, s + 0x24)
        if unp > MAX_SIZE or sto > MAX_SIZE:
            raise CaffError("HEADER", "section size")
        packed = flag and sto != unp
        if not packed and sto != unp:
            raise CaffError("HEADER", "sizes differ, not packed")
        if at + sto > len(b):
            raise CaffError("HEADER", "file shorter than its sections")
        blocks = 0
        if packed:
            p, end, total = at, at + sto, 0
            while end - p >= 8:
                bu, bs = struct.unpack_from("<II", b, p)
                p += 8
                if bs > end - p:
                    break
                p += bs
                total += bu
                blocks += 1
            if p != end or total != unp:
                raise CaffError("HEADER", "blocks do not add up")
        h["sec"].append({"name": name, "unpacked": unp, "stored": sto,
                         "packed": int(packed), "blocks": blocks})
        at += sto
    if h["sec"][0]["name"] != ".data":
        raise CaffError("HEADER", "first section is not .data")
    h["packed"] = int(any(s["packed"] for s in h["sec"]))
    return h


class Package:
    """An unpacked package (or one never packed)."""

    def __init__(self, raw, path=""):
        self.path = path
        b = self.raw = raw
        h = read_header(b)
        if h["packed"]:
            raise CaffError("NEED_GUEST")
        self.flag = h["flag"]
        self.nfiles, self.nparts, self.ntab1, self.ntab2, self.nrel, self.nroff = \
            struct.unpack_from("<6I", b, 0x18)
        self.toc = u32(b, 0x34)
        self.sec_name = [s["name"] for s in h["sec"]]
        self.sec_size = [s["unpacked"] for s in h["sec"]]
        self.sec_base = []
        at = h["data_off"]
        for s in self.sec_size:
            self.sec_base.append(at)
            at += s
        d0, dsize = self.sec_base[0], self.sec_size[0]
        if self.toc + 4 > dsize:
            raise CaffError("TOC", "outside .data")
        if self.nfiles > self.nparts:
            raise CaffError("TOC", "more files than parts")
        p = self.toc
        k = u32(b, d0 + p)
        p += 4
        self.names = []
        if k:
            if 4 * self.nfiles + k > dsize - p:
                raise CaffError("TOC", "name block")
            offs = struct.unpack_from("<%dI" % self.nfiles, b, d0 + p)
            nb = d0 + p + 4 * self.nfiles
            for o in offs:
                if o >= k:
                    raise CaffError("TOC", "file name")
                self.names.append(cstr(b, nb + o, nb + k))
            p += 4 * self.nfiles + k
        need = 14 * self.nparts + 12 * self.ntab1 + 4 * self.ntab2 + 12 * self.nrel + 4 * self.nroff
        if need > dsize - p:
            raise CaffError("TOC", "tables")
        self.parts = []
        for i in range(self.nparts):
            f, o, s, sec, al = struct.unpack_from("<3IBB", b, d0 + p + 14 * i)
            if not 1 <= f <= self.nfiles:
                raise CaffError("TOC", "part file")
            if not 1 <= sec <= len(self.sec_size):
                raise CaffError("TOC", "part section")
            if al > MAX_ALIGN:
                raise CaffError("TOC", "part alignment")
            if o + s > self.sec_size[sec - 1]:
                raise CaffError("TOC", "part past its section")
            self.parts.append({"file": f, "off": o, "size": s, "sec": sec, "align": al})
        p += 14 * self.nparts + 12 * self.ntab1 + 4 * self.ntab2
        self.rels = [struct.unpack_from("<3I", b, d0 + p + 12 * i) for i in range(self.nrel)]
        p += 12 * self.nrel
        if sum(r[2] for r in self.rels) != self.nroff:
            raise CaffError("TOC", "relocation counts")
        self.roffs = list(struct.unpack_from("<%dI" % self.nroff, b, d0 + p))
        self.reloc = {}          # (source part, offset) -> target part
        at = 0
        for src, dst, cnt in self.rels:
            if not (1 <= src <= self.nparts and 1 <= dst <= self.nparts):
                raise CaffError("TOC", "relocation part")
            sp, tp = self.parts[src - 1], self.parts[dst - 1]
            for j in range(cnt):
                o = self.roffs[at + j]
                if o + 4 > sp["size"]:
                    raise CaffError("TOC", "relocation past its part")
                if u32(b, self.part_addr(src) + o) > tp["size"]:
                    raise CaffError("TOC", "pointer past its target")
                if (src, o) in self.reloc:
                    raise CaffError("TOC", "dword relocated twice")
                self.reloc[(src, o)] = dst
            at += cnt
        self.tag = ""
        if self.nparts:
            a = self.part_addr(1)
            self.tag = b[a:a + min(self.parts[0]["size"], 15)].split(b"\0")[0].decode("latin-1")
        self.dir = []
        if self.tag == "package":
            base, size = self.part_addr(1), self.parts[0]["size"]
            if size < 0x24:
                raise CaffError("TOC", "directory part")
            n = u32(b, base + 0x20)
            if 0x24 + 8 * n > size:
                raise CaffError("TOC", "directory")
            for i in range(n):
                slot = 0x24 + 8 * i
                no = u32(b, base + slot)
                np = self.reloc.get((1, slot), 1)
                if no >= self.parts[np - 1]["size"]:
                    raise CaffError("TOC", "directory name")
                a = self.part_addr(np)
                name = cstr(b, a + no, a + self.parts[np - 1]["size"])
                self.dir.append((name, self.reloc.get((1, slot + 4), 0), u32(b, base + slot + 4)))

    @classmethod
    def load(cls, path):
        with open(path, "rb") as f:
            return cls(f.read(), path)

    def part_addr(self, n):
        """File offset of part n (from 1)."""
        pt = self.parts[n - 1]
        return self.sec_base[pt["sec"] - 1] + pt["off"]

    def file_parts(self, f):
        if hasattr(self, "_file_parts"):
            return self._file_parts.get(f, [])
        return [i + 1 for i, pt in enumerate(self.parts) if pt["file"] == f]

    def directory(self):
        """A zpackage's resources: (name, target part, offset in it)."""
        return self.dir

    def by_src(self):
        """Source part -> [(offset, target part)], built once."""
        if not hasattr(self, "_by_src"):
            self._by_src = defaultdict(list)
            for (src, off), dst in self.reloc.items():
                self._by_src[src].append((off, dst))
        return self._by_src

    def resource_files(self, start_part):
        """Every file reached from a part through relocations."""
        seen, todo, files = set(), [start_part], set()
        if not hasattr(self, "_targets"):
            self._targets = {s: {d for _, d in v} for s, v in self.by_src().items()}
            self._file_parts = defaultdict(list)
            for i, pt in enumerate(self.parts, 1):
                self._file_parts[pt["file"]].append(i)
        by_src = self._targets
        while todo:
            p = todo.pop()
            if p in seen or not 1 <= p <= self.nparts:
                continue
            seen.add(p)
            f = self.parts[p - 1]["file"]
            if f not in files:
                files.add(f)
                todo.extend(self.file_parts(f))
            todo.extend(by_src.get(p, ()))
        return files

    def file_size(self, f):
        s = [0, 0]
        for i in self.file_parts(f):
            pt = self.parts[i - 1]
            s[pt["sec"] - 1 if pt["sec"] in (1, 2) else 0] += pt["size"]
        return s

    def dump(self):
        """The parsed TOC, in the shape tests/caff/caff_test.c writes."""
        h = 0
        for v in self.roffs:
            h = (h * 31 + v) & 0xFFFFFFFF
        return {"files": self.nfiles, "parts": self.nparts, "relocs": self.nrel,
                "roffs": self.nroff, "tag": self.tag,
                "sections": [[n, s] for n, s in zip(self.sec_name, self.sec_size)],
                "names": self.names,
                "part_list": [[p["file"], p["off"], p["size"], p["sec"], p["align"]] for p in self.parts],
                "rel_list": [list(r) for r in self.rels], "roff_hash": h,
                "dir": [list(d) for d in self.dir]}


def packages(root):
    for dp, _, fns in os.walk(root):
        for fn in fns:
            if fn.lower().endswith((".rbm", ".rba")):
                p = os.path.join(dp, fn)
                yield p, os.path.relpath(p, root)


def raw(root, out_json):
    """Every package as stored: the header, and the TOC of the unpacked ones."""
    result, n, bad, packed, flag_only = {}, 0, 0, 0, []
    for p, rel in packages(root):
        n += 1
        b = open(p, "rb").read()
        try:
            h = read_header(b)
        except CaffError as e:
            result[rel] = {"error": e.kind}
            bad += 1
            continue
        e = {"flag": h["flag"], "packed": h["packed"],
             "stored": [[s["unpacked"], s["stored"], s["packed"], s["blocks"]] for s in h["sec"]]}
        packed += h["packed"]
        if h["flag"] and not h["packed"]:
            flag_only.append(rel)
        if not h["packed"]:
            try:
                e.update(Package(b, p).dump())
            except CaffError as x:
                e["error"] = x.kind
                bad += 1
        result[rel] = e
    with open(out_json, "w") as f:
        json.dump(result, f)
    for r in flag_only:
        print("  flag set, not packed:", r)
    print("raw: %d packages, %d refused, %d packed, %d flag set but not packed" % (n, bad, packed, len(flag_only)))


def dump(root, out_json):
    result, n, bad = {}, 0, 0
    for p, rel in packages(root):
        n += 1
        try:
            result[rel] = Package.load(p).dump()
        except CaffError as e:
            print("  %s: %s" % (rel, e))
            result[rel] = {"error": e.kind}
            bad += 1
    with open(out_json, "w") as f:
        json.dump(result, f)
    print("dump: %d packages, %d refused" % (n, bad))


def compare(a_json, b_json):
    a, b = json.load(open(a_json)), json.load(open(b_json))
    diff = 0
    for k in sorted(set(a) | set(b)):
        if k not in a or k not in b:
            print("only in %s: %s" % (a_json if k in a else b_json, k))
            diff += 1
            continue
        if a[k] != b[k]:
            diff += 1
            fields = sorted(f for f in set(a[k]) | set(b[k]) if a[k].get(f) != b[k].get(f))
            print("differs: %s (%s)" % (k, ", ".join(fields)))
    print("compare: %d packages, %d differ" % (len(set(a) | set(b)), diff))
    return diff


def checkloose(pkg_path, root):
    """The C writer's loose packages (tests/caff: caff_test loose) checked
    from here: for each directory entry of the source package, its loose file
    under <root>\\<resource name> must hold exactly the parts of the files its
    start part reaches, in the order caff.c promises (the resource's file
    first, then by lowest part; per file .data parts then .gpu), byte for byte,
    with every pointer of those parts renumbered and no other pointer."""
    src = Package.load(pkg_path)
    n = bad = 0
    for name, tgt, off in src.directory():
        n += 1
        try:
            lo = Package.load(os.path.join(root, name))
        except (OSError, CaffError) as e:
            print("  FAIL %s: %s" % (name, e))
            bad += 1
            continue
        files = src.resource_files(tgt)
        order = [src.parts[tgt - 1]["file"]]
        for i, p in enumerate(src.parts, 1):
            if p["file"] in files and p["file"] not in order:
                order.append(p["file"])
        parts = [i for f in order for sec in (1, 2) for i in src.file_parts(f)
                 if src.parts[i - 1]["sec"] == sec]
        new = {old: i + 1 for i, old in enumerate(parts)}
        why = None
        if (lo.nfiles, lo.nparts) != (len(order), len(parts)):
            why = "counts"
        elif new[tgt] != 1 or off != 0x20:
            why = "the resource is not part 1 +0x20"
        elif lo.names[:1] != [name]:
            why = "file 1's name"
        else:
            for old in parts:
                sp, lp = src.parts[old - 1], lo.parts[new[old] - 1]
                a, b = src.part_addr(old), lo.part_addr(new[old])
                if (sp["size"], sp["sec"], sp["align"]) != (lp["size"], lp["sec"], lp["align"]) \
                        or lp["file"] != order.index(sp["file"]) + 1 \
                        or src.raw[a:a + sp["size"]] != lo.raw[b:b + lp["size"]]:
                    why = "part %d" % old
                    break
            bs = src.by_src()
            want = {(new[s], o): new[d] for s in parts for o, d in bs.get(s, ())}
            if not why and want != lo.reloc:
                why = "pointers (%d expected, %d found)" % (len(want), len(lo.reloc))
            if not why and lo.resource_files(1) != set(range(1, lo.nfiles + 1)):
                why = "part 1 does not reach every file"
        if why:
            print("  FAIL %s: %s" % (name, why))
            bad += 1
    print("checkloose %s: %d resources, %d failed" % (os.path.basename(os.path.dirname(pkg_path)), n, bad))
    return bad


def toc(path):
    pk = Package.load(path)
    print("%s  tag=%s  files=%d parts=%d relocs=%d sections=%s" % (
        path, pk.tag, pk.nfiles, pk.nparts, pk.nrel,
        ",".join("%s:%d" % (n, s) for n, s in zip(pk.sec_name, pk.sec_size))))
    for f in range(1, pk.nfiles + 1):
        d, g = pk.file_size(f)
        print("  file %4d  data %9d  gpu %9d  %s" % (f, d, g, pk.names[f - 1] if pk.names else ""))
    for name, tgt, off in pk.directory():
        tf = pk.parts[tgt - 1]["file"] if tgt else None
        print("  dir  %-70s part %s file %s" % (name, tgt, tf))


def find_res(pk, name):
    for n, tgt, off in pk.directory():
        if n.lower() == name.lower() or n.lower().endswith(name.lower()):
            return n, tgt, off
    raise SystemExit("not in the package: " + name)


def res(path, name, nbytes=256):
    """A resource's files and parts, its relocated pointer slots, a hex dump."""
    pk = Package.load(path)
    n, tgt, off = find_res(pk, name)
    files = sorted(pk.resource_files(tgt))
    print("%s -> part %d (+0x%X), %d files" % (n, tgt, off, len(files)))
    for f in files:
        for p in pk.file_parts(f):
            pt = pk.parts[p - 1]
            outs = sorted((o, d) for (s, o), d in pk.reloc.items() if s == p)
            print("  file %d part %d sec %d size 0x%X align %d, %d pointers%s" % (
                f, p, pt["sec"], pt["size"], 1 << pt["align"], len(outs),
                ": " + " ".join("+0x%X->p%d" % od for od in outs[:24]) if outs else ""))
    a = pk.part_addr(tgt) + off
    b = pk.raw[a:a + nbytes]
    for i in range(0, len(b), 16):
        row = b[i:i + 16]
        print("  %05X  %-48s %s" % (i, row.hex(" "), "".join(chr(c) if 32 <= c < 127 else "." for c in row)))


def inventory(root, out_json=None):
    result = {}
    kinds = Counter()
    for p, rel in packages(root):
        try:
            pk = Package.load(p)
        except CaffError as e:
            result[rel] = {"error": str(e)}
            continue
        entry = {"tag": pk.tag, "files": pk.nfiles, "parts": pk.nparts,
                 "sections": dict(zip(pk.sec_name, pk.sec_size)), "dir": []}
        for name, tgt, _ in pk.directory():
            files = pk.resource_files(tgt) if tgt else set()
            d = sum(pk.file_size(f)[0] for f in files)
            g = sum(pk.file_size(f)[1] for f in files)
            entry["dir"].append({"name": name, "files": len(files), "data": d, "gpu": g})
            parts = name.lower().split("\\")
            kind = parts[1] if len(parts) > 2 else name
            kinds[(kind, os.path.splitext(parts[-1])[1] if parts else "")] += 1
        result[rel] = entry
    if out_json:
        with open(out_json, "w") as f:
            json.dump(result, f, indent=1)
    for (k, ext), n in sorted(kinds.items(), key=lambda x: -x[1]):
        print("%6d  %-20s %s" % (n, k, ext))
    print("%d packages" % len(result))


def invcompare(a_json, b_json, show=20):
    """Two inventories (caff.py inventory / conker.exe --carve's
    assets\\inventory.json / caff_test inventory), package by package, entry by
    entry. Paths compared case ignored; an error entry matches any error."""
    def load(p):
        with open(p, encoding="utf-8") as f:
            return {k.lower(): v for k, v in json.load(f).items()}
    a, b = load(a_json), load(b_json)
    bad = []
    for k in sorted(set(a) | set(b)):
        if k not in a or k not in b:
            bad.append("%s: only in %s" % (k, a_json if k in a else b_json))
            continue
        x, y = a[k], b[k]
        if ("error" in x) or ("error" in y):
            if ("error" in x) != ("error" in y):
                bad.append("%s: error %r vs %r" % (k, x.get("error"), y.get("error")))
            continue
        for f in ("tag", "files", "parts", "sections"):
            if x[f] != y[f]:
                bad.append("%s: %s %r vs %r" % (k, f, x[f], y[f]))
        if len(x["dir"]) != len(y["dir"]):
            bad.append("%s: %d vs %d resources" % (k, len(x["dir"]), len(y["dir"])))
            continue
        for i, (p, q) in enumerate(zip(x["dir"], y["dir"])):
            if p != q:
                bad.append("%s: resource %d %r vs %r" % (k, i, p, q))
    n_res = sum(len(v.get("dir", ())) for v in a.values())
    for s in bad[:show]:
        print("  " + s)
    print("invcompare: %d packages, %d resources, %d differences" % (len(a), n_res, len(bad)))
    return len(bad)


if __name__ == "__main__":
    a = sys.argv
    if len(a) >= 4 and a[1] in ("raw", "dump"):
        (raw if a[1] == "raw" else dump)(a[2], a[3])
    elif len(a) >= 4 and a[1] == "compare":
        sys.exit(1 if compare(a[2], a[3]) else 0)
    elif len(a) >= 4 and a[1] == "checkloose":
        sys.exit(1 if checkloose(a[2], a[3]) else 0)
    elif len(a) >= 3 and a[1] == "toc":
        toc(a[2])
    elif len(a) >= 4 and a[1] == "res":
        res(a[2], a[3], int(a[4], 0) if len(a) > 4 else 256)
    elif len(a) >= 3 and a[1] == "inventory":
        inventory(a[2], a[3] if len(a) > 3 else None)
    elif len(a) >= 4 and a[1] == "invcompare":
        sys.exit(1 if invcompare(a[2], a[3]) else 0)
    else:
        print(__doc__)
