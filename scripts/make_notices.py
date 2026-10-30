r"""Write THIRD_PARTY_NOTICES.txt (repo root) for the release zip (SPEC 9.5).

Every licence text is copied from the file its project ships (the submodules,
the toolkit's LICENSES\ folder, the NuGet package cache), so the notices are
exact and follow a version change: rerun this after moving a submodule or a
NuGet package, and commit the result. Packages whose NuGet entry gives only
the expression "MIT" get the standard MIT text with the copyright line from
their .nuspec.

Covers what the zip ships: conker.exe and xemu_apu.dll (toolkit, xemu APU,
Dear ImGui, SDL3 + HIDAPI, the picture filters) and conker-launcher.exe (the
.NET runtime, Avalonia and what it pulls in, CommunityToolkit.Mvvm,
SharpCompress). Build-only packages (ILLink, Avalonia.BuildServices) and tools
that never ship (capstone, Ghidra, xemu itself) are credits, not notices.

Usage: .venv\Scripts\python scripts\make_notices.py
Needs the submodules checked out and the launcher restored once
(launcher\obj\project.assets.json, %USERPROFILE%\.nuget\packages).
"""
import json
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NUGET = os.path.join(os.path.expanduser('~'), '.nuget', 'packages')
OUT = os.path.join(REPO, 'THIRD_PARTY_NOTICES.txt')

MIT = """Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE."""

INTRO = """Conker: Live & Recompiled - third-party notices
==============================================

Conker: Live & Recompiled is built from its own code (MIT, see LICENSE.txt)
and the work of the projects below. Each is listed with what it is used for,
where it comes from and its licence text; the longer notice files that come
with some of them are the appendices at the end. Thank you to everyone who
made them.

No game content is included. Conker: Live & Reloaded, Conker and Xbox are
trademarks of Microsoft Corporation / Rare Ltd. This project is not affiliated
with or endorsed by Microsoft or Rare. Players use their own copy of the game.
"""

CREDITS = """Credits for knowledge used (no code from these is included)
-----------------------------------------------------------

  - xemu (https://github.com/xemu-project/xemu): besides the APU code above,
    the reference for how the Xbox hardware behaves, used to check this port
    frame by frame against the real game.
  - Cxbx-Reloaded and xboxdevwiki: hardware and kernel facts (register
    offsets, kernel export ordinals, structure layouts).
  - codepdbh/xboxrecomp-conker: an earlier attempt at recompiling Conker with
    xboxrecomp, a useful map of the road.
  - The "Conker Xbox 60fps + widescreen" patch (moddb): where the game keeps
    its frame-rate and screen settings.
  - XboxDev/ghidra-xbe and Ghidra (NSA): loading and studying the game's code.
  - nastys, Vaani and Theconker64: the Conker: Live & Reloaded decensor patch.
    It is not included; the launcher downloads it from their GitHub releases
    if the player asks.
"""


def read(path):
    with open(path, encoding='utf-8-sig', errors='replace') as f:
        return f.read().replace('\r\n', '\n').strip('\n')


def git_version(path):
    out = subprocess.run(['git', '-C', path, 'describe', '--tags', '--always'],
                         capture_output=True, text=True, check=True)
    return out.stdout.strip()


def nuget_versions():
    with open(os.path.join(REPO, 'launcher', 'obj', 'project.assets.json'),
              encoding='utf-8') as f:
        assets = json.load(f)
    versions = {}
    for lib in assets['libraries']:
        name, ver = lib.split('/')
        versions[name.lower()] = ver
    for fw in assets['project']['frameworks'].values():
        for dep in fw.get('downloadDependencies', []):
            versions[dep['name'].lower()] = dep['version'].strip('[]').split(',')[0].strip()
    return versions


def nuspec_copyright(pkg, ver):
    path = os.path.join(NUGET, pkg, ver, pkg + '.nuspec')
    text = read(path)
    start = text.find('<copyright>')
    if start < 0:
        sys.exit(f'no <copyright> in {path}')
    line = text[start + len('<copyright>'):text.find('</copyright>')]
    line = line.replace('&amp;', '&').replace('&#169;', '(c)').replace('©', '(c)').strip()
    return line if line.lower().startswith('copyright') or line.startswith('(c)') \
        else 'Copyright (c) ' + line


def section(title, text):
    return f'{title}\n{"-" * len(title)}\n\n{text}\n'


def main():
    nv = nuget_versions()

    def pkg(name):
        ver = nv.get(name.lower())
        if not ver:
            sys.exit(f'{name} is not in launcher\\obj\\project.assets.json: restore the launcher first')
        return name.lower(), ver

    toolkit = os.path.join(REPO, 'xboxrecomp')
    lic = os.path.join(toolkit, 'LICENSES')
    sdl = os.path.join(REPO, 'third_party', 'SDL')
    imgui = os.path.join(REPO, 'third_party', 'imgui')
    runtime = pkg('Microsoft.NETCore.App.Runtime.win-x64')
    avalonia = pkg('Avalonia')
    angle = pkg('Avalonia.Angle.Windows.Natives')
    skia = pkg('SkiaSharp')
    skia_native = pkg('SkiaSharp.NativeAssets.Win32')
    harfbuzz = pkg('HarfBuzzSharp')
    mvvm = pkg('CommunityToolkit.Mvvm')
    sharpcompress = pkg('SharpCompress')
    microcom = pkg('MicroCom.Runtime')
    dbus = pkg('Tmds.DBus.Protocol')

    def nuget_file(p, *names):
        for n in names:
            path = os.path.join(NUGET, p[0], p[1], n)
            if os.path.exists(path):
                return read(path)
        sys.exit(f'none of {names} in {p[0]} {p[1]}')

    def mit_from_nuspec(p):
        return f'MIT License\n\n{nuspec_copyright(*p)}\n\n{MIT}'

    parts = [INTRO]

    # ── conker.exe and xemu_apu.dll ──
    parts.append('\nPART 1: conker.exe and xemu_apu.dll (the game)\n'
                 '===============================================\n')
    parts.append(section(
        '1. xboxrecomp (the recompilation toolkit and runtime)',
        'https://github.com/sp00nznet/xboxrecomp by Ned Heller (sp00nznet) and\n'
        'contributors, used through the fork https://github.com/cdetv/xboxrecomp\n'
        f'(version {git_version(toolkit)}). It translated the game\'s code and provides\n'
        'the runtime around it: the Xbox kernel, graphics, sound and controllers.\n'
        'MIT licence:\n\n' + read(os.path.join(toolkit, 'LICENSE'))))
    parts.append(section(
        '2. xemu: MCPX APU sound emulation (in xemu_apu.dll)',
        'https://github.com/xemu-project/xemu\n'
        'The Xbox sound chip emulation, taken from xemu into the toolkit (folder\n'
        'src/apu: apu_core.c, apu_vp.c, apu_dsp.c and their headers).\n'
        'Copyright (c) 2012 espes; 2018-2019 Jannik Vogel; 2019-2025 Matt Borgerson.\n\n'
        'Licensed under the GNU Lesser General Public License, version 2.1 or (at\n'
        'your option) any later version; the full text is Appendix A. It is built\n'
        'as its own file, xemu_apu.dll, so you may replace it with one you built\n'
        'from changed source. The complete source of the library, as used here, is\n'
        'in https://github.com/cdetv/xboxrecomp (src/apu) and in this release\'s\n'
        'source at https://github.com/cdetv/conker-live-recompiled.'))
    parts.append(section(
        '3. Dear ImGui (the F2/F3 menus and photo mode panel)',
        f'https://github.com/ocornut/imgui ({git_version(imgui)})\n'
        'It includes the ProggyClean and ProggyForever fonts (MIT; Copyright (c)\n'
        '2004, 2005, 2019, 2023 Tristan Grimmer; 2026 Disco Hello).\n\n'
        + read(os.path.join(imgui, 'LICENSE.txt'))))
    parts.append(section(
        '4. SDL3 (PlayStation controllers)',
        f'https://github.com/libsdl-org/SDL ({git_version(sdl)}), only its gamepad parts.\n'
        'zlib licence:\n\n' + read(os.path.join(sdl, 'LICENSE.txt'))
        + '\n\nSDL includes HIDAPI (src/hidapi), used here under its BSD-style licence:\n\n'
        + read(os.path.join(sdl, 'src', 'hidapi', 'LICENSE-bsd.txt'))))
    parts.append(section(
        '5. AMD FidelityFX CAS (sharpening)',
        'Ported to HLSL in the toolkit (src/kernel/postfx_d3d11.c).\n\n'
        + read(os.path.join(lic, 'FidelityFX-CAS.txt'))))
    parts.append(section(
        '6. AMD FidelityFX Super Resolution 1.0 (FSR 1 upscaler)',
        'Included unchanged in the toolkit (third_party/fsr1).\n\n'
        + read(os.path.join(lic, 'FidelityFX-FSR.txt'))))
    parts.append(section(
        '7. NVIDIA FXAA 3.11 (anti-aliasing)',
        'Included unchanged in the toolkit (third_party/fxaa).\n\n'
        + read(os.path.join(lic, 'FXAA-3.11.txt'))))
    parts.append(section(
        '8. SMAA (anti-aliasing)',
        'https://github.com/iryoku/smaa, included unchanged in the toolkit\n'
        '(third_party/smaa).\n\n' + read(os.path.join(lic, 'SMAA.txt'))))

    # ── conker-launcher.exe ──
    parts.append('\nPART 2: conker-launcher.exe (the launcher)\n'
                 '==========================================\n')
    parts.append(section(
        '9. .NET runtime',
        f'https://github.com/dotnet/runtime ({runtime[1]}), built into the launcher so\n'
        'no .NET install is needed. Its own third-party notices are Appendix B.\n\n'
        + nuget_file(runtime, 'LICENSE.TXT', 'LICENSE.txt')))
    parts.append(section(
        '10. Avalonia (the launcher\'s window and controls)',
        f'https://github.com/AvaloniaUI/Avalonia ({avalonia[1]}): Avalonia, Avalonia.Desktop,\n'
        'Avalonia.Win32, Avalonia.Skia, Avalonia.HarfBuzz, Avalonia.Themes.Fluent\n'
        'and the packages they bring along.\n\n' + mit_from_nuspec(avalonia)))
    parts.append(section(
        '11. ANGLE (graphics for Avalonia, via Avalonia.Angle.Windows.Natives)',
        f'https://chromium.googlesource.com/angle/angle ({angle[1]})\n\n'
        + nuget_file(angle, 'LICENSE')))
    parts.append(section(
        '12. SkiaSharp and HarfBuzzSharp (drawing and text for Avalonia)',
        f'https://github.com/mono/SkiaSharp (SkiaSharp {skia[1]}, HarfBuzzSharp {harfbuzz[1]}).\n'
        'Their native libraries contain Skia, HarfBuzz and the libraries those use;\n'
        'those notices are Appendix C.\n\n' + nuget_file(skia, 'LICENSE.txt')))
    parts.append(section(
        '13. CommunityToolkit.Mvvm',
        f'https://github.com/CommunityToolkit/dotnet ({mvvm[1]}). Its own third-party\n'
        'notices are Appendix D.\n\n' + nuget_file(mvvm, 'License.md', 'LICENSE.md')))
    parts.append(section(
        '14. SharpCompress (opening .zip, .7z and .rar files when importing)',
        f'https://github.com/adamhathcock/sharpcompress ({sharpcompress[1]})\n\n'
        + mit_from_nuspec(sharpcompress)))
    parts.append(section(
        '15. MicroCom.Runtime (used by Avalonia)',
        f'https://github.com/kekekeks/MicroCom ({microcom[1]})\n\n' + mit_from_nuspec(microcom)))
    parts.append(section(
        '16. Tmds.DBus.Protocol (used by Avalonia)',
        f'https://github.com/tmds/Tmds.DBus ({dbus[1]})\n\n' + mit_from_nuspec(dbus)))

    parts.append('\n' + CREDITS)

    # ── appendices: the long texts ──
    parts.append('\nAPPENDICES\n==========\n')
    parts.append(section('Appendix A. GNU Lesser General Public License, version 2.1',
                         read(os.path.join(lic, 'LGPL-2.1.txt'))))
    parts.append(section('Appendix B. .NET runtime third-party notices',
                         nuget_file(runtime, 'THIRD-PARTY-NOTICES.TXT', 'THIRD-PARTY-NOTICES.txt')))
    parts.append(section('Appendix C. SkiaSharp and HarfBuzzSharp native library notices',
                         nuget_file(skia_native, 'THIRD-PARTY-NOTICES.txt')))
    parts.append(section('Appendix D. CommunityToolkit.Mvvm third-party notices',
                         nuget_file(mvvm, 'ThirdPartyNotices.txt')))

    text = '\n'.join(parts).rstrip('\n') + '\n'
    with open(OUT, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(text)
    print(f'{OUT}: {len(text.splitlines())} lines, {os.path.getsize(OUT)} bytes')


if __name__ == '__main__':
    main()
