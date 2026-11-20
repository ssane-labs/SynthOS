#!/usr/bin/env python3
"""tools/previews/record.py IMG OUTDIR SCENES.py [only,names]: records every clip of SCENES (scenes_panes.py,
scenes_settings.py), each on a freshly booted SynthOS in QEMU (QMP input, a screendump every 1/10 s).
A clip is a list of segments: (setup commands for the terminal, actions before recording, timeline, seconds).
Timeline entries: (t, action, args). Frames: OUTDIR/<clip>/<seg>_<n>.png (full screen). Then mkpreviews.py OUTDIR."""
import json, os, socket, subprocess, sys, time, tempfile
from PIL import Image
img, out, scenes = sys.argv[1], sys.argv[2], sys.argv[3]
only = sys.argv[4].split(',') if len(sys.argv) > 4 else None
W, H = 1024, 768
FPS = 10
g = {}
exec(open(scenes).read(), g)
CLIPS = g['CLIPS']
q = f = None
def boot():
    global q, f
    sock = tempfile.mktemp(prefix='qmp')
    q = subprocess.Popen(['qemu-system-x86_64', '-drive', f'format=raw,file={img},snapshot=on', '-m', '1536', '-smp', '4',
                          '-device', 'qemu-xhci', '-device', 'usb-tablet', '-display', 'none', '-qmp', f'unix:{sock},server,nowait'])
    for _ in range(100):
        if os.path.exists(sock): break
        time.sleep(0.1)
    s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile('rw')
    f.readline(); cmd('qmp_capabilities')
def cmd(c, **a):
    f.write(json.dumps({'execute': c, 'arguments': a} if a else {'execute': c}) + '\n'); f.flush()
    while True:
        r = json.loads(f.readline())
        if 'return' in r or 'error' in r: return r
def move(x, y):
    cmd('input-send-event', events=[{'type': 'abs', 'data': {'axis': 'x', 'value': int(x * 32767 / (W - 1))}},
                                    {'type': 'abs', 'data': {'axis': 'y', 'value': int(y * 32767 / (H - 1))}}])
def btn(b, down):
    cmd('input-send-event', events=[{'type': 'btn', 'data': {'down': down, 'button': b}}])
def key(k):
    cmd('send-key', keys=[{'type': 'qcode', 'data': p} for p in k.split('-')])
KM = {"'": 'apostrophe', '"': 'shift-apostrophe', '>': 'shift-dot', '<': 'shift-comma', ';': 'semicolon', '&': 'shift-7',
      '|': 'shift-backslash', '*': 'shift-8', '(': 'shift-9', ')': 'shift-0', '+': 'shift-equal', ' ': 'spc', '-': 'minus',
      '.': 'dot', '/': 'slash', '=': 'equal', '_': 'shift-minus', ':': 'shift-semicolon', ',': 'comma', '\n': 'ret',
      '!': 'shift-1', '?': 'shift-slash', '%': 'shift-5'}
def typ(text, delay=0.05):
    for ch in text:
        key(KM.get(ch, ('shift-' + ch.lower()) if ch.isupper() else ch)); time.sleep(delay)
cur = [W // 2, H // 2]
def glide(x, y):
    move(x, y); cur[:] = [x, y]
def click(x, y, b='left'):
    glide(x, y); time.sleep(0.12); btn(b, True); time.sleep(0.06); btn(b, False)
def shell(*cmds):
    """runs commands in the terminal, then closes it"""
    key('ctrl-alt-t'); time.sleep(1.5)
    typ('; '.join(cmds) + '\n'); time.sleep(1.2)
    key('meta_l-w'); time.sleep(1.0)
def act(a, args):
    if a == 'move': glide(*args)
    elif a == 'click': click(*args)
    elif a == 'rclick': click(*args, b='right')
    elif a == 'key': key(args)
    elif a == 'type': typ(args)
    elif a == 'shell': shell(*args)
    elif a == 'down': btn('left', True)
    elif a == 'up': btn('left', False)
    elif a == 'wheel':  # args: notches, > 0 down
        for _ in range(abs(args)):
            b = 'wheel-down' if args > 0 else 'wheel-up'
            btn(b, True); btn(b, False); time.sleep(0.03)
    elif a == 'sleep': time.sleep(args)
    elif a == 'open':
        key('ctrl-spc'); time.sleep(0.8); typ(args); time.sleep(1.0); key('ret'); time.sleep(2.0)
def dump(path):
    p = path + '.ppm'
    cmd('screendump', filename=p)
    for _ in range(100):
        if os.path.exists(p) and os.path.getsize(p) >= W * H * 3: break
        time.sleep(0.01)
    im = Image.open(p); im.load(); os.remove(p)
    return im
REBOOT = g.get('REBOOT', True)
booted = False
for name, segs in CLIPS:
    if only and name not in only: continue
    if REBOOT or not booted:
        boot(); booted = True
        time.sleep(float(g.get('BOOT', 26)))
        for c in g.get('START', []):
            act(*c); time.sleep(0.3)
    d = os.path.join(out, name); os.makedirs(d, exist_ok=True)
    for f_ in os.listdir(d): os.remove(os.path.join(d, f_))
    for si, (setup, before, timeline, secs) in enumerate(segs):
        if setup: shell(*setup)
        for a in before: act(*a); time.sleep(0.25)
        time.sleep(0.6)
        t0 = time.time(); n = 0; tl = sorted(timeline, key=lambda e: e[0]); k = 0
        while n < secs * FPS:
            while k < len(tl) and tl[k][0] <= n / FPS:
                act(tl[k][1], tl[k][2]); k += 1
            due = t0 + n / FPS; now = time.time()
            if due > now: time.sleep(due - now)
            dump(os.path.join(d, f'{si}_{n:03d}')).save(os.path.join(d, f'{si}_{n:03d}.png'), compress_level=1)
            n += 1
        print(name, si, n, 'frames in', round(time.time() - t0, 1), 's', flush=True)
    if REBOOT:
        q.kill(); q.wait()
        cur[:] = [W // 2, H // 2]
if not REBOOT: q.kill()
