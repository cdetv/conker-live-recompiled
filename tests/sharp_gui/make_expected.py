"""Writes tests/sharp_gui/expected.h: made-up pictures (no game data) and what the
prototype scripts (scripts/make_font_sheets.py, scripts/make_button_pictures.py) make
of them, so the C port (src/sharp_gui.c) is checked against the scripts' own maths.
Run again only when the scripts' maths change:
  .venv\\Scripts\\python tests\\sharp_gui\\make_expected.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'scripts'))
import numpy as np  # noqa: E402
import make_font_sheets as mf  # noqa: E402
import make_button_pictures as mb  # noqa: E402

SCALE, EDGE, SHARPEN = 4, 1.25, 1.0


def font_in():
    """12 x 10 grey: a slanted stroke with soft edges, a thin faint hairline, a dot."""
    h, w = 10, 12
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    d = np.abs((x - 2.0) - 0.6 * y)                  # a slanted stroke
    v = np.clip(1.6 - d, 0.0, 1.0)
    v = np.maximum(v, np.where(np.abs(x - 9.0) < 0.5, 0.45, 0.0) * (y > 1) * (y < 9))
    v = np.maximum(v, np.where((x - 6) ** 2 + (y - 7) ** 2 <= 1.0, 0.8, 0.0))
    return (np.round(v * 15) * 17).astype(np.uint8)  # 16 grey levels, as the sheets


def picture_in(binary):
    """10 x 10 RGBA: a shaded disc with a letter-like bar; soft or on/off alpha."""
    h = w = 10
    y, x = np.mgrid[0:h, 0:w].astype(np.float64)
    r = np.hypot(x - 4.6, y - 4.4)
    a = np.clip(4.4 - r, 0.0, 1.0)
    if binary:
        a = (a >= 0.5).astype(np.float64)
    rgb = np.stack([0.85 - 0.05 * y, 0.25 + 0.04 * x, 0.1 + 0.0 * x], axis=2)
    bar = (np.abs(x - 4.5) < 1.2) & (y > 2) & (y < 7)
    rgb[bar] = (0.95, 0.8, 0.6)
    rgb[a == 0] = (0.0, 0.0, 0.0)                    # DXT1's clear pixels are black
    out = np.zeros((h, w, 4), np.uint8)
    out[..., :3] = np.round(rgb * 255)
    out[..., 3] = np.round(a * 255)
    return out


def c_array(name, a):
    flat = a.reshape(-1).tolist()
    lines = []
    for i in range(0, len(flat), 24):
        lines.append('    ' + ','.join('%d' % v for v in flat[i:i + 24]) + ',')
    return 'static const unsigned char %s[%d] = {\n%s\n};\n' % (name, len(flat), '\n'.join(lines))


def main():
    f = font_in()
    f_out = mf.to_u8(mf.clean(mf.enlarge(f, SCALE), SCALE, EDGE))
    parts = ['/* Made by make_expected.py from the prototype scripts: do not edit. */\n',
             '#define EXP_SCALE %d\n#define EXP_EDGE %.2ff\n#define EXP_SHARPEN %.2ff\n'
             % (SCALE, EDGE, SHARPEN),
             '#define FONT_W %d\n#define FONT_H %d\n' % (f.shape[1], f.shape[0]),
             c_array('font_in', f), c_array('font_out', f_out)]
    for tag, binary in (('soft', False), ('bin', True)):
        p = picture_in(binary)
        out = mb.to_u8(mb.clean(p, SCALE, EDGE, SHARPEN))
        parts += ['#define %s_W %d\n#define %s_H %d\n' % (tag.upper(), p.shape[1], tag.upper(),
                                                         p.shape[0]),
                  c_array('%s_in' % tag, p), c_array('%s_out' % tag, out)]
    with open(os.path.join(HERE, 'expected.h'), 'w', newline='\n') as fh:
        fh.write(''.join(parts))
    print('expected.h written')


if __name__ == '__main__':
    main()
