r"""Sharper button pictures for to-do 53 step 4: the game's own controller button pictures
(A, B, X, Y, the triggers, the moving stick ...), enlarged and cleaned up automatically on
this PC (prototype of what conker.exe will do on each player's PC, as make_font_sheets.py
does for the fonts).

The pictures are small painted images (32x32 or 64x64, DXT1 / DXT5) with see-through
edges: soft ones (DXT5) or on/off ones (DXT1, stairs when enlarged). The game finds a
replacement by the hash of the picture's bytes and draws it over the same rectangle, so a
bigger picture only makes it sharper. Per picture:
 1. reads it from the texture export (conker.exe --texture-export, the launcher's
    textures\dump) and its hash from the export's textures.tsv;
 2. enlarges colour (weighted by its see-through value, so no dark rim comes in from the
    clear pixels around it) and see-through value SCALE times with the fonts' smooth curve
    (cubic B-spline: no blocky steps, no ringing);
 3. cleans up the outline as the fonts' edges: the soft border (about one original pixel)
    made about EDGE new pixels wide round the same middle line (50 % see-through), so the
    shape stays and its edge is crisp and smooth; an on/off outline is smoothed about half
    an original pixel more first (its stairs would stay as waves);
 4. sharpens the colour inside a little (unsharp mask, SHARPEN, over about one original
    pixel) so painted details (the letter on the button) don't look soft;
 5. carries the edge colours on into the clear area, so the game's own filtering and the
    smaller mip levels don't pull in black round the edge;
 6. writes <out>\<HASH>_<name>.png (RGBA).
Nothing derived from the game goes into the repo: write into _work\ or next to the .exe.

Usage (numpy only):
  .venv\Scripts\python scripts\make_button_pictures.py <out dir> --export <dir>
      [--scale 4] [--edge 1.25] [--sharpen 1.0] [--names A_BUTT,...] [--compare <dir>]
<dir> = the export's folder (textures.tsv and zpackage\...). --compare also writes, per
picture, original enlarged pixel by pixel | smooth only | cleaned up, over dark and light.
"""
import argparse
import csv
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_font_sheets import bspline_weights  # noqa: E402
from make_title_logo import read_png, write_png  # noqa: E402

# The game's controller button pictures (package, name), r2-swing-hud.md / r3-looks.md C1
# plus the multiplayer and training ones the texture list shows (2026-11-27).
PICTURES = [
    ('General_Boot', n) for n in (
        'A_BUTT', 'B_BUTT', 'X_BUTT', 'Y_BUTT', 'L_BUTT', 'R_BUTT', 'D_BUTT', 'G_BUTT', 'I_BUTT',
        'S_BUTT', 'joystick100', 'joystick101', 'joystick102', 'joystick103', 'joystick104',
        'joystick105')
] + [
    ('General_Multiplayer', n) for n in (
        'A_BUTT', 'B_BUTT', 'BLACK_BUTT', 'triggerG', 'triggerI', 'triggerL', 'triggerS')
] + [('Level_TrainingLevel', 'l_but_1')]


def enlarge(img, scale):
    """Each channel of a float HxWxC picture enlarged with the cubic B-spline."""
    wy = bspline_weights(img.shape[0] * scale, img.shape[0])
    wx = bspline_weights(img.shape[1] * scale, img.shape[1])
    return np.stack([wy @ img[..., c] @ wx.T for c in range(img.shape[2])], axis=2)


def box_blur(img, r):
    """Mean over a (2r+1) square, one axis at a time, edges repeated."""
    out = img
    for axis in (0, 1):
        pad = [(0, 0)] * out.ndim
        pad[axis] = (r, r)
        p = np.pad(out, pad, mode='edge')
        acc = np.zeros_like(out)
        for i in range(2 * r + 1):
            sl = [slice(None)] * out.ndim
            sl[axis] = slice(i, i + out.shape[axis])
            acc += p[tuple(sl)]
        out = acc / (2 * r + 1)
    return out


def soft_blur(img, r):
    """Three box passes: close to a Gaussian of about r / 1.7 pixels."""
    for _ in range(3):
        img = box_blur(img, r)
    return img


def clean(rgba, scale, edge, sharpen):
    """rgba: uint8 HxWx4 -> float (H*scale)x(W*scale)x4, colour not premultiplied."""
    src = rgba.astype(np.float64) / 255.0
    a = src[..., 3:4]
    pm = np.concatenate([src[..., :3] * a, a], axis=2)          # premultiplied
    big = enlarge(pm, scale)
    big_a = np.clip(big[..., 3:4], 0.0, 1.0)
    col = np.clip(big[..., :3] / np.maximum(big_a, 1e-4), 0.0, 1.0)
    # The outline: crisp round the 50 % line, as the fonts' edges. An on/off outline
    # (DXT1: only clear or solid) is smoothed by about another half original pixel first,
    # or its stairs stay as waves.
    shape = big_a
    if len(np.unique(rgba[..., 3])) <= 2:
        shape = soft_blur(big_a, max(1, scale // 2))
    out_a = np.clip((shape - 0.5) * (scale / edge) + 0.5, 0.0, 1.0)
    # Colour inside sharpened a little (unsharp mask over about one original pixel), the
    # blur weighted by coverage so the clear area doesn't darken the rim.
    if sharpen > 0:
        r = max(1, scale // 2)
        w = soft_blur(big_a, r)
        blur = soft_blur(col * big_a, r) / np.maximum(w, 1e-4)
        col = np.clip(col + sharpen * (col - blur), 0.0, 1.0)
    # Carry the edge colours on into the clear area (filtering and mips read them).
    known = (big_a > 0.02).astype(np.float64)
    acc_c, acc_w = col * known, known
    fill = col.copy()
    hole = known[..., 0] < 0.5
    for _ in range(6 * scale):
        if not hole.any():
            break
        acc_c, acc_w = box_blur(acc_c, 1), box_blur(acc_w, 1)
        got = hole & (acc_w[..., 0] > 1e-6)
        fill[got] = acc_c[got] / acc_w[got]
        acc_c[got] = fill[got]
        acc_w[got] = 1.0
        hole &= ~got
    col = np.where(known > 0.5, col, fill)
    return np.concatenate([col, out_a], axis=2)


def smooth_only(rgba, scale):
    src = rgba.astype(np.float64) / 255.0
    a = src[..., 3:4]
    big = enlarge(np.concatenate([src[..., :3] * a, a], axis=2), scale)
    big_a = np.clip(big[..., 3:4], 0.0, 1.0)
    return np.concatenate([np.clip(big[..., :3] / np.maximum(big_a, 1e-4), 0, 1), big_a], axis=2)


def to_u8(img):
    return (np.clip(img, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def over(img, bg):
    a = img[..., 3:4]
    return img[..., :3] * a + np.array(bg, np.float64) / 255.0 * (1.0 - a)


def read_list(export):
    """(package, name) -> (hash, png path) from the export's textures.tsv."""
    rows = {}
    with open(os.path.join(export, 'textures.tsv'), encoding='utf-8', newline='') as f:
        for r in csv.DictReader(f, delimiter='\t'):
            key = (r['package'], r['name'])
            if key not in rows and r['hash'] != '-' and r['png']:
                rows[key] = (r['hash'], os.path.join(export, r['png']))
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--export', required=True)
    ap.add_argument('--scale', type=int, default=4)
    ap.add_argument('--edge', type=float, default=1.25)
    ap.add_argument('--sharpen', type=float, default=1.0)
    ap.add_argument('--names', default='')
    ap.add_argument('--compare', default='')
    a = ap.parse_args()
    rows = read_list(a.export)
    want = set(a.names.split(',')) if a.names else None
    os.makedirs(a.out, exist_ok=True)
    for pkg, name in PICTURES:
        if want and name not in want:
            continue
        if (pkg, name) not in rows:
            print('%s %s: not in the export' % (pkg, name))
            continue
        h, png = rows[(pkg, name)]
        img = read_png(png)
        if img.shape[2] == 3:
            img = np.concatenate([img, np.full(img.shape[:2] + (1,), 255, np.uint8)], axis=2)
        out = clean(img, a.scale, a.edge, a.sharpen)
        path = os.path.join(a.out, '%s_%s.png' % (h, name))
        write_png(path, to_u8(out))
        print('%s %s: %dx%d -> %dx%d, %s' % (pkg, name, img.shape[1], img.shape[0],
                                             out.shape[1], out.shape[0], path))
        if a.compare:
            os.makedirs(a.compare, exist_ok=True)
            pix = np.repeat(np.repeat(img.astype(np.float64) / 255.0, a.scale, 0), a.scale, 1)
            sm = smooth_only(img, a.scale)
            gap = np.ones((out.shape[0], 8, 3))
            rows_out = []
            for bg in ((40, 40, 48), (200, 200, 200)):
                rows_out.append(np.hstack([over(pix, bg), gap, over(sm, bg), gap, over(out, bg)]))
            side = np.vstack([rows_out[0], np.ones((8, rows_out[0].shape[1], 3)), rows_out[1]])
            rgba = np.concatenate([side, np.ones(side.shape[:2] + (1,))], axis=2)
            write_png(os.path.join(a.compare, '%s_%s.png' % (pkg, name)), to_u8(rgba))


if __name__ == '__main__':
    main()
