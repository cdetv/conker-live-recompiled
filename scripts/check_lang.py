r"""check_lang.py: the menus' translation files (to-dos 229 and 289, lang\<code>.txt) against the code.

The English text in the code is the key: every string marked T("..."), TL("...") or N_("...") in src/*.c,
src/*.cpp (src/ui_text.h), then the launcher's (to-do 289, launcher/Services/Lang.cs): T("...") and S("...")
in launcher/**/*.cs (literals joined with +) and {l:T '...'} in launcher/**/*.axaml. Each lang\<code>.txt holds
"en: <English>" then "<code>: <text>" ("\\n" for a line break), grouped under "## <function>" lines (the
launcher's: "## launcher/<file>"); "#" lines are comments. The game and the launcher read the same files; a
text both use is one line. The game's texts keep their printf slots (%s, %d), the launcher's their {0} slots
(any order; the format after ":" kept).

  python scripts/check_lang.py            report: missing / unused lines, printf mismatches, letters the menu
                                          font lacks (Latin languages), translations much longer than English;
                                          exit 1 on a printf mismatch (package_release.ps1 runs it)
  python scripts/check_lang.py --update   rewrite every lang file in the code's order: translations kept,
                                          new lines added empty ("de: "), lines no longer in the code kept at
                                          the end under "## no longer in the code"
  python scripts/check_lang.py --list     the English strings, one a line (with their function)
"""
import html, io, os, re, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
LAUNCHER = os.path.join(ROOT, "launcher")
LANG = os.path.join(ROOT, "lang")
CODES = ["de", "fr", "es", "it", "ja"]
NAMES = {"de": "German (Deutsch)", "fr": "French (Français)", "es": "Spanish (Español)",
         "it": "Italian (Italiano)", "ja": "Japanese (日本語)"}

LIT = r'"(?:[^"\\\n]|\\.)*"'
MARK = re.compile(r'(?<![\w])(N_|TL|T)\(\s*((?:' + LIT + r'\s*)+)\)')
FUNC = re.compile(r'^(?:static\s+|extern\s+"C"\s+)?[\w:\*<>]+[\s\*]+(\w+)\s*\([^;]*$')
# No space flag: our formats never use it, and "14 % is" in a note is plain text.
SPEC = re.compile(r'%[-+#0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|z|j|t|I64|I32)?[diouxXeEfgGaAcspn%]')

# The launcher (C#): T("...") / S("...") with literals joined by +, never an interpolated $"..." (its words
# would change with every value); {l:T '...'} in .axaml (\\ a backslash, \' an apostrophe, XML entities).
CS_LIT = r'"(?:[^"\\\n]|\\.)*"'
CS_MARK = re.compile(r'(?<![\w.])(T|S)\(\s*(' + CS_LIT + r'(?:\s*\+\s*' + CS_LIT + r')*)')
CS_INTERP = re.compile(r'(?<![\w.])(T|S)\(\s*\$@?"')
AX_MARK = re.compile(r"\{l:T\s+'((?:[^'\\]|\\.)*)'\s*\}")
# C#'s composite format slots: {0}, {1:N0}, {2,5}; {{ and }} are plain braces.
SLOT = re.compile(r'(?<!\{)\{(\d+)(?:,-?\d+)?(?::[^{}]*)?\}')


def c_unescape(lits):
    """Adjacent C literals -> the UTF-8 text they make."""
    out = bytearray()
    for lit in re.findall(LIT, lits):
        s = lit[1:-1].encode("utf-8")
        i = 0
        while i < len(s):
            c = s[i]
            if c != 0x5C:
                out.append(c)
                i += 1
                continue
            e = chr(s[i + 1])
            if e == "x":
                j = i + 2
                while j < len(s) and chr(s[j]) in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(s[i + 2:j], 16) & 0xFF)
                i = j
            elif e in "01234567":
                j = i + 1
                while j < len(s) and j < i + 4 and chr(s[j]) in "01234567":
                    j += 1
                out.append(int(s[i + 1:j], 8) & 0xFF)
                i = j
            else:
                out += {"n": b"\n", "t": b"\t", "r": b"\r", "\\": b"\\", '"': b'"', "'": b"'",
                        "0": b"\0"}.get(e, e.encode())
                i += 2
    return out.decode("utf-8")


def strip_comments(text):
    """Comments out, line breaks kept (so line numbers stay)."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    return re.sub(r"//[^\n]*|/\*.*?\*/|'(?:[^'\\\n]|\\.)*'|" + LIT,
                  lambda m: m.group(0) if m.group(0)[0] in "\"'" else blank(m), text, flags=re.S)


def source_strings():
    """[(key, function)] in the code's order, first use each; keys without leading spaces."""
    seen, out = set(), []
    files = sorted(f for f in os.listdir(SRC) if f.endswith((".c", ".cpp")) and not f.startswith("ui_text"))
    for name in files:
        text = strip_comments(io.open(os.path.join(SRC, name), encoding="utf-8", errors="replace").read())
        lines = text.split("\n")
        starts, func = [], name                 # file-scope strings: the file's name
        for n, l in enumerate(lines):
            m = FUNC.match(l)
            if m and not l.startswith(("#", "}", " ")):
                func = m.group(1)
            starts.append(func)
        offs = [0]
        for l in lines:
            offs.append(offs[-1] + len(l) + 1)
        for m in MARK.finditer(text):
            key = c_unescape(m.group(2)).lstrip(" ")
            if not key or key in seen:
                continue
            seen.add(key)
            line = next(i for i in range(len(offs) - 1) if offs[i + 1] > m.start())
            out.append((key, starts[line]))
    return out


def cs_unescape(lits):
    """C# literals joined with + -> their text."""
    out = []
    for lit in re.findall(CS_LIT, lits):
        s, i = lit[1:-1], 0
        while i < len(s):
            if s[i] != "\\":
                out.append(s[i])
                i += 1
                continue
            e = s[i + 1]
            if e == "u":
                out.append(chr(int(s[i + 2:i + 6], 16)))
                i += 6
            elif e == "x":
                j = i + 2
                while j < len(s) and j < i + 6 and s[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(chr(int(s[i + 2:j], 16)))
                i = j
            else:
                out.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(e, e))
                i += 2
    return "".join(out)


def launcher_files():
    """The launcher's sources in reading order: windows (.axaml first), view models, services, the rest."""
    found = []
    for dirpath, dirs, files in os.walk(LAUNCHER):
        dirs[:] = [d for d in dirs if d not in ("bin", "obj")]
        found += [os.path.relpath(os.path.join(dirpath, f), LAUNCHER).replace(os.sep, "/")
                  for f in files if f.endswith((".cs", ".axaml"))]
    rank = {"Views": 0, "ViewModels": 1, "Services": 2}
    return sorted(found, key=lambda p: (rank.get(p.split("/")[0], 3), not p.endswith(".axaml"), p))


def launcher_strings():
    """([(key, "launcher/<file>")] in reading order, first use each; [(file, line)] of T($"...") / S($"...")."""
    seen, out, interp = set(), [], []
    for rel in launcher_files():
        text = io.open(os.path.join(LAUNCHER, rel), encoding="utf-8-sig", errors="replace").read()
        if rel.endswith(".axaml"):
            text = re.sub(r"<!--.*?-->", lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)
            found = [html.unescape(re.sub(r"\\(.)", r"\1", m.group(1))) for m in AX_MARK.finditer(text)]
        else:
            text = strip_comments(text)
            found = [cs_unescape(m.group(2)) for m in CS_MARK.finditer(text)]
            interp += [(rel, text.count("\n", 0, m.start()) + 1) for m in CS_INTERP.finditer(text)]
        for key in found:
            if key and key not in seen:
                seen.add(key)
                out.append((key, "launcher/" + rel))
    return out, interp


def slots(s):
    """A launcher text's {0} slots, in any order."""
    return sorted(m.group(0) for m in SLOT.finditer(s))


def esc(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n")


def unesc(s):
    return re.sub(r"\\(n|\\)", lambda m: "\n" if m.group(1) == "n" else "\\", s)


def read_lang(code):
    """(header comment lines, {key: text}, [keys in file order])"""
    path = os.path.join(LANG, code + ".txt")
    header, table, order = [], {}, []
    if not os.path.exists(path):
        return header, table, order
    key, body = None, False
    for raw in io.open(path, encoding="utf-8-sig").read().split("\n"):
        l = raw.rstrip("\r")
        if l.startswith("en: "):
            key, body = unesc(l[4:].lstrip(" ")), True
            if key not in table:
                order.append(key)
                table[key] = ""
        elif key is not None and l.startswith(code + ":"):
            v = l[len(code) + 1:]
            table[key] = unesc(v[1:] if v.startswith(" ") else v)
            key = None
        elif l.startswith("#") and not body and not l.startswith("##"):
            header.append(l)
    return header, table, order


def write_lang(code, keys, header, table):
    out = list(header) or ["# %s: the menus of this port and its launcher (to-dos 229, 289). \"en:\" is the English in "
                           "the code (the key, never change it), \"%s:\" the translation" % (NAMES[code], code),
                           "# (empty = English). \\n = a line break. Keep every %s, %d, %.0f etc. in the same order, and "
                           "every {0}, {1:N0} etc. (any order). scripts/check_lang.py checks it."]
    func = None
    code_keys = set()
    for key, f in keys:
        code_keys.add(key)
        if f != func:
            out += ["## " + f] if out and out[-1] == "" else ["", "## " + f]
            func = f
        out += ["en: " + esc(key), "%s: %s" % (code, esc(table.get(key, ""))), ""]
    gone = [k for k in table if k not in code_keys and table[k]]
    if gone:
        out += ["", "## no longer in the code (kept for their translations)"]
        for k in gone:
            out += ["en: " + esc(k), "%s: %s" % (code, esc(table[k])), ""]
    while out and out[-1] == "":
        out.pop()
    io.open(os.path.join(LANG, code + ".txt"), "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")


def default_font_chars():
    """Code points of ImGui's default font (ProggyForever minimal, compressed in imgui_draw.cpp)."""
    src = io.open(os.path.join(ROOT, "third_party", "imgui", "imgui_draw.cpp"), encoding="utf-8",
                  errors="replace").read()
    m = re.search(r"proggy_forever_minimal_ttf_compressed_data\[\d+\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        return None
    i = bytes(int(x) for x in re.findall(r"\d+", m.group(1)))

    def u32(b, k):
        return (b[k] << 24) | (b[k + 1] << 16) | (b[k + 2] << 8) | b[k + 3]
    out = bytearray(u32(i, 8))
    p = o = 0
    p = 16

    def match(dist, ln):
        nonlocal o
        for k in range(ln):
            out[o + k] = out[o - dist + k]
        o += ln

    def lit(at, ln):
        nonlocal o
        out[o:o + ln] = i[at:at + ln]
        o += ln
    while True:
        c = i[p]
        if c >= 0x80:
            match(i[p + 1] + 1, c - 0x80 + 1); p += 2
        elif c >= 0x40:
            match(((c << 8) + i[p + 1]) - 0x4000 + 1, i[p + 2] + 1); p += 3
        elif c >= 0x20:
            lit(p + 1, c - 0x20 + 1); p += 1 + c - 0x20 + 1
        elif c >= 0x18:
            match(((c << 16) | (i[p + 1] << 8) | i[p + 2]) - 0x180000 + 1, i[p + 3] + 1); p += 4
        elif c >= 0x10:
            match(((c << 16) | (i[p + 1] << 8) | i[p + 2]) - 0x100000 + 1, ((i[p + 3] << 8) | i[p + 4]) + 1); p += 5
        elif c >= 0x08:
            ln = ((c << 8) | i[p + 1]) - 0x0800 + 1; lit(p + 2, ln); p += 2 + ln
        elif c == 0x07:
            ln = ((i[p + 1] << 8) | i[p + 2]) + 1; lit(p + 3, ln); p += 3 + ln
        elif c == 0x06:
            match(((i[p + 1] << 16) | (i[p + 2] << 8) | i[p + 3]) + 1, i[p + 4] + 1); p += 5
        elif c == 0x04:
            match(((i[p + 1] << 16) | (i[p + 2] << 8) | i[p + 3]) + 1, ((i[p + 4] << 8) | i[p + 5]) + 1); p += 6
        elif c == 0x05 and i[p + 1] == 0xFA:
            break
        else:
            return None
    ttf = bytes(out[:o])
    tabs = {ttf[12 + 16 * k:16 + 16 * k]: struct.unpack_from(">III", ttf, 16 + 16 * k)
            for k in range(struct.unpack_from(">H", ttf, 4)[0])}
    co = tabs[b"cmap"][1]
    cps = set()
    for k in range(struct.unpack_from(">H", ttf, co + 2)[0]):
        _, _, off = struct.unpack_from(">HHI", ttf, co + 4 + 8 * k)
        t = co + off
        if struct.unpack_from(">H", ttf, t)[0] == 4:
            n2 = struct.unpack_from(">H", ttf, t + 6)[0]
            n = n2 // 2
            ends = struct.unpack_from(">%dH" % n, ttf, t + 14)
            starts = struct.unpack_from(">%dH" % n, ttf, t + 16 + n2)
            for a, b in zip(starts, ends):
                if a != 0xFFFF:
                    cps.update(range(a, b + 1))
    return cps


def main():
    game = source_strings()
    launcher, interp = launcher_strings()
    game_keys = {k for k, _ in game}
    launcher_keys = {k for k, _ in launcher}
    keys = game + [(k, f) for k, f in launcher if k not in game_keys]
    bad = 0
    for rel, line in interp:
        print("  INTERP  launcher/%s:%d: T($\"...\") / S($\"...\") can't be translated: use {0} slots" % (rel, line))
        bad += 1
    if "--list" in sys.argv:
        for k, f in keys:
            print("%-28s %s" % (f, esc(k)))
        print("%d strings" % len(keys), file=sys.stderr)
        return 0
    if "--update" in sys.argv:
        os.makedirs(LANG, exist_ok=True)
        for code in CODES:
            header, table, _ = read_lang(code)
            write_lang(code, keys, header, table)
        print("updated %d files, %d strings" % (len(CODES), len(keys)))
        return 1 if bad else 0
    font = default_font_chars()
    print("%d strings in the code (%d the game's, %d the launcher's, %d of them both)"
          % (len(keys), len(game_keys), len(launcher_keys), len(game_keys & launcher_keys)))
    for code in CODES:
        _, table, _ = read_lang(code)
        have = {k for k, f in keys if table.get(k)}
        missing = [k for k, f in keys if not table.get(k)]
        unused = [k for k in table if table[k] and k not in {k2 for k2, _ in keys}]
        print("\n%s: %d of %d translated, %d missing, %d no longer in the code"
              % (code, len(have), len(keys), len(missing), len(unused)))
        for k, f in keys:
            v = table.get(k)
            if not v:
                continue
            if k in game_keys and SPEC.findall(k) != SPEC.findall(v):
                print("  PRINTF  %s: %r -> %r" % (f, k, v))
                bad += 1
            if k in launcher_keys and slots(k) != slots(v):
                print("  SLOTS   %s: %r -> %r" % (f, k, v))
                bad += 1
            # settings.c's marked texts with line breaks are settings.ini comments the launcher shows as one-line
            # tooltips (to-do 289): their breaks only fit the file's width, so a one-line translation is right.
            if k.count("\n") != v.count("\n") and "\n" in k and f != "settings.c":
                print("  LINES   %s: %r has %d line breaks, the translation %d" % (f, k, k.count("\n"), v.count("\n")))
            if font and code != "ja" and k in game_keys:
                lack = sorted({ch for ch in v if ord(ch) not in font and ch not in "\n"})
                if lack:
                    print("  FONT    %s: %r uses %s (not in the menu font: drawn from Segoe UI)" % (f, v, "".join(lack)))
            if len(k) >= 12 and len(v) > 1.6 * len(k):
                print("  LONG    %s: %.1fx  %r" % (f, len(v) / len(k), v))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
