BOOT = 26
REBOOT = False
START = [('shell', ['config docksleep 0'])]
def path(t0, t1, pts, fps=10):
    out = []; n = max(1, int((t1 - t0) * fps)); segs = len(pts) - 1
    for i in range(n + 1):
        u = i / n * segs; k = min(int(u), segs - 1); v = u - k
        v = v * v * (3 - 2 * v)
        (x0, y0), (x1, y1) = pts[k], pts[k + 1]
        out.append((t0 + i / fps, 'move', (int(x0 + (x1 - x0) * v), int(y0 + (y1 - y0) * v))))
    return out
SWEEP = path(0.2, 2.8, [(390, 650), (390, 732), (640, 732), (520, 650)])
def one(key, value, timeline=None, secs=3, extra=(), before=()):
    return (f's_{key}_{value}', [(list(extra) + [f'config {key} {value}'], list(before), SWEEP if timeline is None else timeline, secs)])
STILL = path(0.2, 2.8, [(600, 420), (560, 380)])
CC = [('click', (864, 11)), ('sleep', 0.8)]
CLIPS = []
for k, vals in [('dockhalo', ['on', 'off']), ('dockspot', ['on', 'off']), ('dockfx', ['subtle', 'normal', 'vivid']),
                ('magnify', ['off', 'on']), ('dockdots', ['off', 'on']),
                ('iconshape', ['rounded', 'circle', 'square', 'leaf', 'squircle'])]:
    extra = ['config dockhalo on'] if k == 'dockfx' else []
    for v in vals:
        CLIPS.append(one(k, v, extra=extra))
CLIPS.append(('reset1', [(['config dockhalo off'], [], [], 0.1)]))
for v in ['small', 'large', 'medium']:
    CLIPS.append(one('docksize', v))
for v in ['corner', 'side', 'bottom']:
    CLIPS.append(one('dockstyle', v, timeline=STILL))
for v in ['autohide', 'off', 'show']:
    CLIPS.append(one('dockmode', v, timeline=path(0.2, 2.8, [(512, 500), (512, 760), (512, 600)])))
for v in ['off', 'on']:
    CLIPS.append(one('dockbounce', v, timeline=[(0.3, 'click', (443, 728)), (2.2, 'key', 'meta_l-q')], secs=3.5))
for v in ['grayscale', 'invert', 'contrast', 'none']:
    CLIPS.append(one('colorfilter', v))
for v in ['light', 'dark']:
    CLIPS.append(one('appearance', v, timeline=STILL))
for v in ['off', 'on']:
    CLIPS.append(one('nightshift', v, timeline=STILL))
CLIPS.append(one('nightwarmth', 0, timeline=STILL, extra=['config nightshift on']))
CLIPS.append(one('nightwarmth', 100, timeline=STILL, extra=['config nightshift on']))
CLIPS.append(('reset2', [(['config nightshift off', 'config nightwarmth 50'], [], [], 0.1)]))
for v in [30, 100]:
    CLIPS.append(one('brightness', v, timeline=STILL))
for k, vals in [('glass', ['frosted', 'liquid']), ('glasseffect', ['regular', 'clear', 'tinted', 'smoky', 'prism', 'halo']),
                ('glassstrength', [10, 100, 70]), ('glassedge', [0, 100, 50]), ('glassfrost', [0, 100, 50])]:
    for v in vals:
        CLIPS.append(one(k, v, timeline=[(2.5, 'key', 'esc')], before=CC))
for v in ['off', 'on']:
    CLIPS.append(one('animations', v, timeline=[(0.3, 'click', (386, 728)), (1.6, 'key', 'esc')], secs=2.5))
