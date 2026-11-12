import re, sys, difflib
CH = {'x':0,'y':1,'z':2,'w':3}
def parse(line):
    line = re.sub(r'^\s*\d+:\s*', '', line.rstrip('\n'))
    op = line.split()[0]
    d = {'op': op, 'text': line}
    m = re.search(r'dst=(\w+?)(\d+):(\d+)', line)
    d['dst'] = (m.group(1), int(m.group(2)), int(m.group(3))) if m else None
    for k in ('s0','s1','s2'):
        m = re.search(k + r'=([|\-]*)([a-z]+)(\d+)\.([xyzw01h_]{4})', line)
        d[k] = (m.group(1), m.group(2), int(m.group(3)), m.group(4)) if m else None
    m = re.search(r' u=(\d+),(\d+),(\d+)', line); d['u'] = tuple(map(int, m.groups())) if m else (0,0,0)
    m = re.search(r' r0=(\w+?)(\d+) r1=', line); d['r0'] = (m.group(1), int(m.group(2))) if m else None
    return d
def stage(txt, name):
    out, on = [], False
    for l in txt:
        if l.startswith('== '):
            on = l.startswith('== ' + name + ' (')
            continue
        if on: out.append(l)
    return out
def lanes_read(inst, s):
    """(lane l -> channel) for a source of a vec/test instruction"""
    src = inst[s]
    if src is None: return {}
    swz = src[3]
    if inst['op'] == 'test':
        c = inst['u'][2]
        return {0: CH.get(swz[c & 3])}
    mask = inst['dst'][2] if inst['dst'] else 0xf
    wide = 4 if mask == 0xf else 2
    return {l: CH.get(swz[l]) for l in range(wide) if (mask >> l) & 1 and swz[l] in CH}
def writes(inst):
    """absolute (bank, idx) lanes an instruction writes (conservative for unknown ops)"""
    res = set()
    if inst['dst'] and inst['dst'][0] not in ('r', 'i', 'c'):
        b, n, m = inst['dst']
        if inst['op'] in ('vec', 'mov_internal', 'cond_mov', 'vec16', 'mad', 'mad2'):
            wide = 4 if m == 0xf else 2
            for l in range(wide):
                if (m >> l) & 1: res.add((b, n + l))
        else:
            for l in range(4): res.add((b, n + l))
    if inst['r0'] and inst['r0'][0] not in ('r', 'i', 'c'):
        for l in range(4): res.add((inst['r0'][0], inst['r0'][1] + l))
    return res
txt = open(sys.argv[1]).read().splitlines()
A = [parse(l) for l in stage(txt, 'two-wide')]
B = [parse(l) for l in stage(txt, 'load sources')]
sm = difflib.SequenceMatcher(a=[x['text'] for x in A], b=[x['text'] for x in B], autojunk=False)
# map: for each replaced block, pair original readers with rewritten ones in order (skipping loads)
def is_load(x): return x['op']=='vec' and x['dst'] and x['dst'][0]=='i' and x['dst'][2]==15 and x['s1'] and x['s1'][1]=='c' and x['s1'][2]==2
problems = 0
for tag, a0, a1, b0, b1 in sm.get_opcodes():
    if tag == 'equal': continue
    orig = list(range(a0, a1)); new = list(range(b0, b1))
    readers = [i for i in orig if not is_load(A[i])]
    if len(readers) != len(new):
        print('UNALIGNED', a0, a1, b0, b1); problems += 1; continue
    for ia, ib in zip(readers, new):
        a, b = A[ia], B[ib]
        if a['op'] == 'test':
            continue
        swapped = (b['s0'] and b['s1'] and a['s0'] and a['s1'] and a['s1'][1] == 'i' and
                   b['s1'][1] == a['s0'][1] and b['s1'][2] == a['s0'][2])
        for s in ('s0','s1','s2'):
            if not a[s] or a[s][1] != 'i': continue
            sb = ('s0' if s == 's1' else 's1') if swapped and s in ('s0','s1') else s
            k = a[s][2]
            # find the load of i<k> before ia
            li = max(j for j in range(ia) if is_load(A[j]) and A[j]['dst'][1] == k)
            L = A[li]
            ra = lanes_read(a, s); rb = lanes_read(b, sb)
            for l, c in ra.items():
                if c is None: continue
                lc = CH.get(L['s0'][3][c])
                want = (L['s0'][1], L['s0'][2] + lc)
                got_c = rb.get(l)
                got = (b[sb][1], b[sb][2] + got_c) if got_c is not None else None
                clob = any(want in writes(A[j]) for j in range(li + 1, ia))
                mods_ok = (b[s][0] == (a[s][0] or L['s0'][0]) ) or True
                if got != want or clob:
                    problems += 1
                    print(f'BAD lane {l} {s}: want {want} got {got} clobbered={clob}')
                    print('   load  ', L['text'][:110]); print('   orig  ', a['text'][:110]); print('   new   ', b['text'][:110])
                    if clob:
                        for j in range(li + 1, ia):
                            if want in writes(A[j]): print('   writer', A[j]['text'][:110])
print('problems', problems)
