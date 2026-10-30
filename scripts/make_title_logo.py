r"""Build the replacement for Conker's boot title picture (the "Live &
Reloaded" logo on a dark blue vignette, shown while the game first loads)
from a logo PNG with transparency, e.g. the user's "Live & Recompiled".

The game's picture is one 1024x512 DXT1 texture, the screen in its top-left
640x480, hash E761496A53AD4678 (the toolkit's tex_replace.h). This script:
 1. finds where the old logo sits, as scale + offset of the new logo, by
    matching the "Conker" word (the new logo's top part) against the old;
 2. fits the background vignette (an elliptical radial profile) from the
    pixels away from the old logo, so the new picture has no trace of it;
 3. draws both at SCALE x the original size and writes
    <out dir>\E761496A53AD4678_title_logo.png (3072x1536, opaque).
The game stretches the picture to the window as it did the original.

Usage (~3 min; numpy only):
  .venv\Scripts\python scripts\make_title_logo.py <dumped picture> <logo png> <out dir>
<dumped picture>: the texture as RECOMP_TEX_HASH_DUMP writes it
(E761496A53AD4678_1024x512_f0C.png). Nothing derived from the game goes into
the repo: the dump and the result stay in _work\ and next to the .exe.
"""
import os
import struct
import sys
import zlib

import numpy as np

HASH = 'E761496A53AD4678'
SCALE = 3


# ── PNG in and out (8-bit RGB/RGBA, no PIL) ──────────────────────────────

def read_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', path
    pos, idat = 8, b''
    while pos < len(data):
        n, typ = struct.unpack('>I4s', data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if typ == b'IHDR':
            w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', body)
            assert bd == 8 and ct in (2, 6) and il == 0, (path, bd, ct, il)
            ch = 4 if ct == 6 else 3
        elif typ == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * ch
    out = np.zeros((h, stride), np.uint8)
    prev = np.zeros(stride, np.int32)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = np.frombuffer(raw, np.uint8, stride,
                             y * (stride + 1) + 1).astype(np.int32)
        if f == 0:
            cur = line
        elif f == 2:
            cur = (line + prev) & 255
        else:
            cur = np.zeros(stride, np.int32)
            for x in range(stride):
                a = cur[x - ch] if x >= ch else 0
                b = prev[x]
                c = prev[x - ch] if x >= ch else 0
                if f == 1:
                    p = a
                elif f == 3:
                    p = (a + b) >> 1
                else:
                    pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                    p = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                cur[x] = (line[x] + p) & 255
        out[y] = cur
        prev = cur
    img = out.reshape(h, w, ch)
    if ch == 3:
        img = np.concatenate([img, np.full((h, w, 1), 255, np.uint8)], 2)
    return img


def write_png(path, rgba):
    h, w, _ = rgba.shape
    raw = b''.join(b'\x00' + rgba[y].tobytes() for y in range(h))

    def chunk(t, b):
        return (struct.pack('>I', len(b)) + t + b
                + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 9)))
        f.write(chunk(b'IEND', b''))


# ── Resampling (Lanczos-3) ───────────────────────────────────────────────

def weights(n_out, n_in, s, off):
    """Matrix (n_out x n_in) for out = in * s + off."""
    sup = 3.0 / min(s, 1.0)
    W = np.zeros((n_out, n_in))
    for o in range(n_out):
        c = (o + 0.5 - off) / s - 0.5
        i = np.arange(max(int(np.floor(c - sup)), 0),
                      min(int(np.ceil(c + sup)) + 1, n_in))
        if not len(i):
            continue
        x = (i - c) * min(s, 1.0)
        w = np.sinc(x) * np.sinc(x / 3.0)
        w[np.abs(x) >= 3] = 0
        if w.sum() != 0:
            W[o, i] = w / w.sum()
    return W


def place(img, s, ox, oy, out_w, out_h):
    """img (H,W,C, premultiplied) drawn at scale s, offset ox,oy."""
    Wy = weights(out_h, img.shape[0], s, oy)
    Wx = weights(out_w, img.shape[1], s, ox)
    return np.einsum('yh,hwc,xw->yxc', Wy, img, Wx, optimize=True)


# ── 1. Where the old logo is ─────────────────────────────────────────────

def fit_logo(orig, pm):
    lum_o = orig.mean(2)
    word = pm.copy()
    word[300:] = 0                      # "Conker" only: above the LIVE line
    ys, xs = np.where(word[..., 3] > 0.9)
    m = lum_o > 0.25
    m[:, 535:] = False                  # the TM
    m[245:] = False                     # LIVE & RELOADED and below
    ys2, xs2 = np.where(m)
    s0 = (xs2.max() - xs2.min()) / (xs.max() - xs.min())
    ox0 = xs2.min() - xs.min() * s0
    oy0 = ys2.min() - ys.min() * s0

    def err(s, ox, oy):
        p = place(word, s, ox, oy, 640, 480)
        msk = p[..., 3] > 0.97
        d = p[..., :3].mean(2)[msk] - lum_o[msk]
        return np.mean(d * d)

    for step, off in ((0.004, 1.0), (0.001, 0.25)):
        best = min((err(s, ox, oy), s, ox, oy)
                   for s in s0 + np.arange(-4, 5) * step
                   for ox in ox0 + np.arange(-4, 5) * off
                   for oy in oy0 + np.arange(-4, 5) * off)
        e, s0, ox0, oy0 = best
        print('logo fit: rms %.4f scale %.4f offset %.2f, %.2f'
              % (np.sqrt(e), s0, ox0, oy0))
    return s0, ox0, oy0


# ── 2. The background ────────────────────────────────────────────────────

NB = 400


def profile(orig, mask, cx, cy, k):
    yy, xx = np.mgrid[0:480, 0:640] + 0.5
    d = np.sqrt(((xx - cx) * k) ** 2 + (yy - cy) ** 2)
    b = np.minimum(d.astype(int), NB - 1)
    cnt = np.bincount(b[mask], minlength=NB)
    ok = cnt > 0
    idx = np.arange(NB)
    prof = np.zeros((NB, 3))
    for c in range(3):
        sums = np.bincount(b[mask], orig[..., c][mask], minlength=NB)
        prof[:, c] = np.interp(idx, idx[ok], sums[ok] / cnt[ok])
        prof[:, c] = np.convolve(np.pad(prof[:, c], 2, mode='edge'),
                                 np.ones(5) / 5, 'valid')
    r = orig[mask] - prof[b[mask]]
    return prof, np.mean(r * r)


def fit_background(orig):
    mask = np.ones((480, 640), bool)
    mask[100:325, 75:575] = False       # the old logo and its TM
    best = min((profile(orig, mask, cx, cy, k)[1], cx, cy, k)
               for cx in range(300, 341, 4) for cy in range(190, 261, 5)
               for k in np.arange(0.55, 1.01, 0.05))
    _, cx, cy, k = best
    best = min((profile(orig, mask, x, y, kk)[1], x, y, kk)
               for x in np.arange(cx - 4, cx + 4.1, 1)
               for y in np.arange(cy - 5, cy + 5.1, 1)
               for kk in np.arange(k - 0.05, k + 0.051, 0.01))
    e, cx, cy, k = best
    print('background fit: rms %.2f/255, centre %.0f,%.0f, x squash %.2f'
          % (np.sqrt(e), cx, cy, k))
    return cx, cy, k, profile(orig, mask, cx, cy, k)[0]


# ── 3. The picture ───────────────────────────────────────────────────────

def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    orig = read_png(sys.argv[1])[:480, :640, :3].astype(float)
    user = read_png(sys.argv[2]).astype(float) / 255
    a = user[..., 3:4]
    pm = np.concatenate([user[..., :3] * a, a], 2)

    s, ox, oy = fit_logo(orig / 255, pm)
    cx, cy, k, prof = fit_background(orig)

    TW, TH, SW, SH = 1024 * SCALE, 512 * SCALE, 640 * SCALE, 480 * SCALE
    yy, xx = (np.mgrid[0:SH, 0:SW] + 0.5) / SCALE
    d = np.sqrt(((xx - cx) * k) ** 2 + (yy - cy) ** 2)
    idx = np.arange(NB) + 0.5
    bg = np.stack([np.interp(d, idx, prof[:, c]) for c in range(3)], 2) / 255
    p = np.clip(place(pm, s * SCALE, ox * SCALE, oy * SCALE, SW, SH), 0, 1)
    p[..., :3] = np.minimum(p[..., :3], p[..., 3:4])
    comp = bg * (1 - p[..., 3:4]) + p[..., :3]
    rng = np.random.default_rng(1)       # dither against banding
    comp += (rng.random(comp.shape) - rng.random(comp.shape)) / 255
    scr = np.clip(np.round(comp * 255), 0, 255).astype(np.uint8)

    out = np.zeros((TH, TW, 4), np.uint8)
    out[:SH, :SW, :3] = scr
    out[:SH, SW:, :3] = scr[:, -1:, :]   # the unused part: edge repeated
    out[SH:, :, :3] = out[SH - 1:SH, :, :3]
    out[..., 3] = 255
    os.makedirs(sys.argv[3], exist_ok=True)
    path = os.path.join(sys.argv[3], HASH + '_title_logo.png')
    write_png(path, out)
    print('wrote', path)


if __name__ == '__main__':
    main()
