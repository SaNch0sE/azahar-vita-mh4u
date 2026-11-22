#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Willi Ye
# SPDX-License-Identifier: GPL-2.0-or-later

"""shader_textures.py CAPTURE CONSOLE DUMPS OUT: a recorded frame's textures with the fragment
programs that drew with them.

CAPTURE is a frame capture (record.sh, or a diagnostics launch with sgxd_flags `vprof
vprof_at=S vprof_len=2`), CONSOLE that launch's console (its `vprof draw N: fs KEY` lines tie
the capture's draw N to the program's key; a draw without one was not drawn, as the second
render of a game's own anti-aliasing is not), DUMPS the directory of the launch's `dumpusse`
files (azahar-fs-KEY.glsl, .fsconfig, .json and the second tier's azahar-fs2-KEY.json).

OUT gets:
  textures/HASH.png   each texture the frame's draws read, once
  targets/drawN.png   a pass's colour buffer as guest memory had it at the pass's start
  shaders/            each program's dump files
  index.html          a program a section: its draws, its textures by unit, its dumps
  pairs.tsv           draw, key, words, vertices, kind, target, unit, texture, size and format

frame-replay (tools/frame_replay's CLI) is taken from FRAME_REPLAY or build-frame-replay/."""
import collections, hashlib, html, os, re, shutil, subprocess, sys

def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    capture, console, dumps, out = sys.argv[1:]
    here = os.path.dirname(os.path.abspath(__file__))
    tool = os.environ.get('FRAME_REPLAY',
                          os.path.join(here, '..', '..', 'build-frame-replay', 'frame-replay'))
    for d in ('textures', 'targets', 'shaders', 'draws'):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    keys = {}
    for line in open(console, errors='replace'):
        m = re.search(r'vprof draw (\d+): fs ([0-9a-f]{16}), (\d+) words, (\d+) vertices, '
                      r'([a-z\- ]+), target (0x[0-9a-f]+)', line)
        if m and int(m[1]) not in keys:
            keys[int(m[1])] = (m[2], int(m[3]), int(m[4]), m[5], m[6])
    frame = subprocess.run([tool, 'frame', capture], capture_output=True, text=True).stdout
    m = re.search(r'(\d+) draws', frame)
    count = int(m[1]) if m else 0
    if count == 0:
        sys.exit('no draws in the capture: ' + frame[:200])

    programs = collections.OrderedDict()   # key -> {'draws': [...], 'units': {unit: {hash: [..]}}}
    rows = []
    texture_info = {}
    for n in range(count):
        text = subprocess.run([tool, 'draw', capture, str(n), '--images',
                               os.path.join(out, 'draws')], capture_output=True, text=True).stdout
        key, words, vertices, kind, target = keys.get(n, ('not-drawn', 0, 0, '', ''))
        p = programs.setdefault(key, {'draws': [], 'units': collections.defaultdict(dict),
                                      'words': words, 'kinds': set(), 'targets': set()})
        p['draws'].append(n)
        p['kinds'].add(kind)
        p['targets'].add(target)
        seen = False
        for pic in re.finditer(r'^picture: (.*?)(?: -> (\S+)| \(not decoded\))$', text, re.M):
            title, png = pic[1], pic[2]
            t = re.match(r'texture unit (\d): (\S+) (\d+x\d+) (\S+)', title)
            if t:
                unit, addr, size, fmt = int(t[1]), t[2], t[3], t[4]
                h = 'undecoded-' + addr
                if png and os.path.exists(png):
                    h = hashlib.sha1(open(png, 'rb').read()).hexdigest()[:16]
                    dst = os.path.join(out, 'textures', h + '.png')
                    if not os.path.exists(dst):
                        shutil.move(png, dst)
                    else:
                        os.remove(png)
                texture_info[h] = (size, fmt, addr)
                p['units'][unit].setdefault(h, []).append(n)
                rows.append((n, key, words, vertices, kind, target, unit, h, size, fmt))
                seen = True
            elif png and os.path.exists(png):
                shutil.move(png, os.path.join(out, 'targets', f'draw{n}.png'))
        if not seen:
            rows.append((n, key, words, vertices, kind, target, '', '', '', ''))
    shutil.rmtree(os.path.join(out, 'draws'), ignore_errors=True)

    with open(os.path.join(out, 'pairs.tsv'), 'w') as f:
        f.write('draw\tfs\twords\tvertices\tkind\ttarget\tunit\ttexture\tsize\tformat\n')
        for r in rows:
            f.write('\t'.join(str(x) for x in r) + '\n')

    def dump_files(key):
        files = []
        for name in (f'azahar-fs-{key}.glsl', f'azahar-fs-{key}.fsconfig', f'azahar-fs-{key}.json',
                     f'azahar-fs2-{key}.json'):
            src = os.path.join(dumps, name)
            if os.path.exists(src):
                shutil.copy(src, os.path.join(out, 'shaders', name))
                files.append(name)
        return files

    page = ['<!doctype html><meta charset="utf-8"><title>Shaders and their textures</title>',
            '<style>body{font:14px system-ui,sans-serif;margin:16px;background:#16181c;color:#dde}'
            'h2{font:600 15px ui-monospace,monospace;margin:28px 0 4px;border-top:1px solid #445;'
            'padding-top:12px}a{color:#8bf}.t{display:inline-block;margin:6px 10px 6px 0;'
            'vertical-align:top;font-size:12px;max-width:260px}.t img{display:block;max-width:256px;'
            'max-height:256px;image-rendering:pixelated;background:repeating-conic-gradient(#333 0 25%,'
            '#222 0 50%) 0 0/16px 16px;border:1px solid #556}.m{color:#99a;font-size:12px}</style>',
            f'<h1>{html.escape(os.path.basename(capture))}: {count} draws, {len(programs)} fragment '
            f'programs, {len(texture_info)} textures</h1>',
            '<p class="m">A program a section, in the frame\'s draw order: its draws (the frame '
            'tool\'s numbers), the textures they read by unit, the program\'s dumps.</p>']
    for key, p in programs.items():
        files = dump_files(key)
        name = (f'fs {key}' if key != 'not-drawn' else
                'draws without a program line (the renderer did not draw them: a skipped pass)')
        page.append(f'<h2 id="{key}">{name}</h2><div class="m">{p["words"]} words; '
                    f'{len(p["draws"])} draws ({", ".join(map(str, p["draws"][:24]))}'
                    f'{" ..." if len(p["draws"]) > 24 else ""}); {", ".join(sorted(k for k in p["kinds"] if k))}; '
                    f'targets {", ".join(sorted(t for t in p["targets"] if t))}; '
                    + ' '.join(f'<a href="shaders/{f}">{f.split(".")[-1] if "fs2" not in f else "tier 2 json"}</a>'
                               for f in files) + '</div>')
        if not p['units']:
            page.append('<div class="m">no texture unit enabled</div>')
        for unit in sorted(p['units']):
            for h, draws in p['units'][unit].items():
                size, fmt, addr = texture_info[h]
                img = (f'<a href="textures/{h}.png"><img src="textures/{h}.png" loading="lazy"></a>'
                       if not h.startswith('undecoded') else '<div class="m">(not decoded)</div>')
                page.append(f'<div class="t">{img}unit {unit}: {size} {fmt} at {addr}<br>'
                            f'<span class="m">{len(draws)} draws: {", ".join(map(str, draws[:10]))}'
                            f'{" ..." if len(draws) > 10 else ""}</span></div>')
    open(os.path.join(out, 'index.html'), 'w').write('\n'.join(page) + '\n')
    undrawn = len(programs.get('not-drawn', {'draws': []})['draws'])
    print(f'{count} draws, {len(programs) - (1 if undrawn else 0)} programs, {len(texture_info)} '
          f'textures; {undrawn} draws the renderer did not draw; {out}/index.html')

if __name__ == '__main__':
    main()
