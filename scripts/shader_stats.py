r"""Shader compile numbers of run logs (to-do 25): .venv\Scripts\python scripts\shader_stats.py <log> [...]

The last [VSHGPU] / combiner-setup report lines of each log: how many shaders were
compiled while playing and how many batches were drawn the slow way meanwhile."""
import re, sys
for path in sys.argv[1:]:
    vsh = comb = None; pre = 0; cache = None; hitch = 0; drawn_compiled = 0
    for line in open(path, encoding='utf-8', errors='replace'):
        if line.startswith('[VSHGPU]') and 'program batches drawn' in line: vsh = line.strip()
        elif 'combiner setups (RECOMP_PS_SPEC)' in line: comb = line.strip()
        elif line.startswith('[VSHPRE] queue empty'):
            m = re.search(r': (\d+) compiled', line); pre = max(pre, int(m.group(1)) if m else 0)
        elif 'shader cache:' in line and 'loaded' in line: cache = line.strip()
        elif line.startswith('[VSHGPU] program') and 'draw shader compiled in' in line: drawn_compiled += 1
        elif re.match(r'\[HITCH\] frame \d+: \d+ ms', line): hitch += 1
    print('==', path)
    print('  ', cache)
    if vsh:
        m = re.search(r'on the CPU: (\d+) while its shader compiled', vsh)
        print('   vertex: batches drawn on the CPU while their shader compiled %s' % m.group(1))
    print('   vertex: compiled when first drawn %d, compiled by the level-change pre-compiler %d'
          % (drawn_compiled, pre))
    if comb:
        m = re.search(r'(\d+) seen, (\d+) compiled, (\d+) from the cache.*?batches (\d+) with their own shader, (\d+) with the one', comb)
        print('   combiner setups: %s seen, %s compiled, %s from cache; batches drawn with the general shader while theirs compiled: %s' % (m.group(1), m.group(2), m.group(3), m.group(5)))
    print('   long frames listed by [HITCH] (RECOMP_HITCH_MS):', hitch)
