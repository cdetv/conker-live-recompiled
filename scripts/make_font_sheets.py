r"""Sharper fonts for to-do 53: the game's own font sheets, enlarged and cleaned up
automatically on this PC (prototype of what conker.exe will do on each player's PC).

Each of the game's four fonts (aid\font\<Font>\default.bin) holds its letters in one
greyscale picture (linear L8, white on black: the "sheet"). The game finds a replacement
picture by the hash of the sheet's bytes, and it keeps the letters' boxes in the 0..1
space of the sheet, so a bigger sheet with the same layout only makes the text sharper.
This script, per font:
 1. reads the sheet and its replacement hash (hash_bytes over the bytes, as the
    toolkit's texture_stage does);
 2. enlarges it SCALE times with a smooth curve (cubic B-spline: it evens out the steps
    the 16 grey levels leave along slanted edges, where a curve through the pixels,
    Catmull-Rom, made them wavy);
 3. scales each spot by the brightest value within one original pixel (at least LOW), so
    a hairline too thin to reach full white (the title font's thin strokes) counts as a
    whole stroke instead of breaking up; full-white strokes are unchanged;
 4. cleans up the edges: the soft grey border around each letter (about one original
    pixel wide) is made about EDGE new pixels wide around the same middle line
    (coverage 0.5), so the letters keep their shapes and weight but get crisp edges;
 5. writes <out>\<HASH>_<Font>.png (greyscale; the game reads it as the original's
    grey, alpha 1).
Tuned 2026-11-26 on crops of all four fonts (_work\t53\): corners come out a little
rounder than a curve through the pixels gives; slanted edges and hairlines much cleaner.
Nothing derived from the game goes into the repo: write into _work\ or next to the .exe.

Usage (numpy only):
  .venv\Scripts\python scripts\make_font_sheets.py <out dir> --game-dir <dir> [--scale 4]
      [--edge 1.25] [--fonts ConkerFont,FrontendTitle] [--compare <dir>]
<dir> holds dvddata\aid\font or aid\font (the extracted game, or the launcher's game folder).
--compare also writes side-by-side crops (original enlarged pixel by pixel | smooth
enlargement only | cleaned up) for looking at the result.
"""
import argparse
import os
import struct
import sys
import zlib

import numpy as np

FONTS = ('ConkerFont', 'FrontendTitle', 'ConkerFontJapanese', 'FrontendTitleJapanese')
M64 = (1 << 64) - 1


def hash_bytes(b, h=0xCBF29CE484222325):
    """The toolkit's replacement hash (nv2a_draw_d3d11.c hash_bytes)."""
    n = len(b)
    words = np.frombuffer(b[:n - n % 8], dtype='<u8')
    for w in words.tolist():
        h = ((h ^ w) * 0x100000001B3) & M64
        h ^= h >> 29
    for j in range(n - n % 8, n):
        h = ((h ^ b[j]) * 0x100000001B3) & M64
    return h


def read_sheet(path):
    """(sheet as uint8 HxW, hash) from a font's default.bin (r3-looks.md C1)."""
    b = open(path, 'rb').read()
    data_off = struct.unpack_from('<I', b, 0x38)[0]
    data_size = struct.unpack_from('<I', b, 0x50)[0]
    w, h = struct.unpack_from('<HH', b, 0x500)
    off = data_off + data_size
    px = b[off:off + w * h]
    if len(px) != w * h or w % 64:
        raise ValueError('%s: unexpected sheet layout (%dx%d)' % (path, w, h))
    return np.frombuffer(px, dtype=np.uint8).reshape(h, w), hash_bytes(px)


def bspline_weights(n_out, n_in):
    """(n_out, n_in) cubic B-spline matrix enlarging one axis; pixel centres line up
    as the GPU's sampling does (new pixel o sits at (o + 0.5) / s - 0.5)."""
    s = n_out / n_in
    m = np.zeros((n_out, n_in))
    for o in range(n_out):
        c = (o + 0.5) / s - 0.5
        i0 = int(np.floor(c))
        for i in range(i0 - 1, i0 + 3):
            x = abs(i - c)
            if x < 1.0:
                w = (3.0 * x ** 3 - 6.0 * x ** 2 + 4.0) / 6.0
            elif x < 2.0:
                w = (2.0 - x) ** 3 / 6.0
            else:
                w = 0.0
            m[o, min(max(i, 0), n_in - 1)] += w
    return m


def enlarge(sheet, scale):
    src = sheet.astype(np.float64) / 255.0
    wy = bspline_weights(src.shape[0] * scale, src.shape[0])
    wx = bspline_weights(src.shape[1] * scale, src.shape[1])
    return wy @ src @ wx.T


def local_max(img, r):
    """Brightest value within r pixels (a square), one axis at a time."""
    out = img
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode='edge')
        acc = out.copy()
        for i in range(2 * r + 1):
            sl = [slice(None), slice(None)]
            sl[axis] = slice(i, i + out.shape[axis])
            np.maximum(acc, p[tuple(sl)], out=acc)
        out = acc
    return out


def clean(big, scale, edge, low=0.3):
    """Each spot over the brightest value within one original pixel (at least `low`),
    then crisp edges round the 0.5 line: an edge that took one original pixel (scale
    new pixels) now takes `edge` new pixels."""
    peak = np.clip(local_max(big, scale), low, 1.0)
    gain = scale / edge
    return np.clip((big / peak - 0.5) * gain + 0.5, 0.0, 1.0)


def to_u8(img):
    return (np.clip(img, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def write_png_grey(path, img):
    h, w = img.shape
    raw = b''.join(b'\x00' + img[y].tobytes() for y in range(h))

    def chunk(t, b):
        return (struct.pack('>I', len(b)) + t + b
                + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff))
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 0, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 9)))
        f.write(chunk(b'IEND', b''))
    os.replace(tmp, path)


def find_font_dir(game):
    for sub in (('dvddata', 'aid', 'font'), ('aid', 'font')):
        d = os.path.join(game, *sub)
        if os.path.isdir(d):
            return d
    raise SystemExit('no aid\\font folder under %s' % game)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--game-dir', required=True)
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--edge', type=float, default=1.25)
    ap.add_argument('--fonts', default=','.join(FONTS))
    ap.add_argument('--compare', default='')
    a = ap.parse_args()
    font_dir = find_font_dir(a.game_dir)
    os.makedirs(a.out, exist_ok=True)
    for name in a.fonts.split(','):
        sheet, h = read_sheet(os.path.join(font_dir, name, 'default.bin'))
        big = enlarge(sheet, a.scale)
        out = to_u8(clean(big, a.scale, a.edge))
        path = os.path.join(a.out, '%016X_%s.png' % (h, name))
        write_png_grey(path, out)
        print('%s: %dx%d -> %dx%d, %s' % (name, sheet.shape[1], sheet.shape[0],
                                          out.shape[1], out.shape[0], path))
        if a.compare:
            os.makedirs(a.compare, exist_ok=True)
            pix = np.repeat(np.repeat(sheet, a.scale, 0), a.scale, 1)
            gap = np.full((out.shape[0], 8), 96, np.uint8)
            side = np.hstack([pix, gap, to_u8(big), gap, out])
            write_png_grey(os.path.join(a.compare, '%s_%dx.png' % (name, a.scale)), side)


if __name__ == '__main__':
    main()
