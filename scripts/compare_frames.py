"""Compare two folders of frame dumps (RECOMP_FB_DUMP + RECOMP_FB_DUMP_FLIPS) frame by frame.

Used to prove a GPU change draws the same pictures as before: dump the presented frames
of a run with the old build and with the new one, then compare them by sequence number.

    .venv\\Scripts\\python scripts\\compare_frames.py <dirA> <dirB> [--prefix f_] [--diff-out <dir>]

One run with RECOMP_GPU=both writes both sets into one folder: the software frames as
<prefix>NNNNN.bmp and the GPU's as <prefix>gpu_NNNNN.bmp, so pass the folder twice:

    ... compare_frames.py <dir> <dir> --prefix f_ --prefix-b f_gpu_

Prints how many frames are byte-identical, and for the rest the share of pixels that
differ and the largest channel difference. With --diff-out, writes a side-by-side
picture (A | B | difference) for the first few differing frames.

Only the standard library: the dumps are 24-bit uncompressed BMPs.
"""
import argparse
import hashlib
import os
import struct
import sys


def read_bmp(path):
    """Width, height and the raw bottom-up BGR rows (with row padding) of a 24-bit BMP."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError(f"{path}: {bpp} bpp, expected 24")
    return w, h, data[off:]


def write_bmp(path, w, h, rows):
    row_bytes = w * 3
    pad = (4 - row_bytes % 4) % 4
    size = 54 + (row_bytes + pad) * h
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    struct.pack_into("<I", hdr, 2, size)
    struct.pack_into("<I", hdr, 10, 54)
    struct.pack_into("<IiiHH", hdr, 14, 40, w, h, 1, 24)
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(rows)


def frames(folder, prefix):
    out = {}
    for name in os.listdir(folder):
        if name.startswith(prefix) and name.endswith(".bmp"):
            num = name[len(prefix):-4]
            if num.isdigit():
                out[int(num)] = os.path.join(folder, name)
    return out


def side_by_side(a, b):
    """A | B | |A-B|*4, all w x h, as BMP rows."""
    w, h, ra = a
    _, _, rb = b
    row_bytes = w * 3
    pad = (4 - row_bytes % 4) % 4
    stride = row_bytes + pad
    out_w = w * 3
    out_pad = (4 - (out_w * 3) % 4) % 4
    out = bytearray()
    for y in range(h):
        la = ra[y * stride:y * stride + row_bytes]
        lb = rb[y * stride:y * stride + row_bytes]
        diff = bytes(min(255, abs(p - q) * 4) for p, q in zip(la, lb))
        out += la + lb + diff + b"\0" * out_pad
    return out_w, h, bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--prefix", default="f_")
    ap.add_argument("--prefix-b", help="file prefix in <dirB> (default: --prefix)")
    ap.add_argument("--diff-out", help="folder for side-by-side pictures of differing frames")
    ap.add_argument("--max-diff-pics", type=int, default=5)
    ap.add_argument("--list", type=int, default=40,
                    help="how many differing frames to list")
    ap.add_argument("--tolerance", type=int, default=0,
                    help="frames whose largest channel difference is at most this"
                         " count as matching (rounding between two drawers)")
    args = ap.parse_args()

    fa = frames(args.a, args.prefix)
    fb = frames(args.b, args.prefix_b or args.prefix)
    common = sorted(set(fa) & set(fb))
    print(f"A: {len(fa)} frames, B: {len(fb)} frames, compared: {len(common)}")
    if not common:
        return 1

    same = close = 0
    differing = []
    for n in common:
        da, db = read_bmp(fa[n]), read_bmp(fb[n])
        if da[:2] != db[:2]:
            differing.append((n, f"size {da[0]}x{da[1]} vs {db[0]}x{db[1]}", da, db))
            continue
        # A run stopped by its timeout can leave the last dump half written.
        need = da[1] * (da[0] * 3 + (4 - da[0] * 3 % 4) % 4)
        if len(da[2]) < need or len(db[2]) < need:
            print(f"  frame {n:5d}: truncated file, skipped")
            continue
        if hashlib.sha1(da[2]).digest() == hashlib.sha1(db[2]).digest():
            same += 1
            continue
        w, h = da[0], da[1]
        row_bytes = w * 3
        stride = row_bytes + (4 - row_bytes % 4) % 4
        px_diff = 0
        max_ch = 0
        for y in range(h):
            la = da[2][y * stride:y * stride + row_bytes]
            lb = db[2][y * stride:y * stride + row_bytes]
            if la == lb:
                continue
            for x in range(0, row_bytes, 3):
                d = max(abs(la[x] - lb[x]), abs(la[x + 1] - lb[x + 1]), abs(la[x + 2] - lb[x + 2]))
                if d:
                    px_diff += 1
                    max_ch = max(max_ch, d)
        if max_ch <= args.tolerance:
            close += 1
            continue
        differing.append((n, f"{100.0 * px_diff / (w * h):.2f}% of pixels differ, max channel diff {max_ch}", da, db))

    print(f"identical: {same}/{len(common)}")
    if args.tolerance:
        print(f"within {args.tolerance} per channel: {close}; beyond: {len(differing)}")
    for n, why, _, _ in differing[:args.list]:
        print(f"  frame {n:5d}: {why}")
    if len(differing) > args.list:
        print(f"  ... and {len(differing) - args.list} more")
    if args.diff_out and differing:
        os.makedirs(args.diff_out, exist_ok=True)
        for n, _, da, db in differing[:args.max_diff_pics]:
            if da[:2] == db[:2]:
                path = os.path.join(args.diff_out, f"diff_{n:05d}.bmp")
                write_bmp(path, *side_by_side(da, db))
                print(f"  wrote {path}")
    return 0 if not differing else 2


if __name__ == "__main__":
    sys.exit(main())
