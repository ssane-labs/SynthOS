BOOT = 26
START = [('shell', ['config docksleep 0'])]
def path(t0, t1, pts, fps=10):
    out = []; n = max(1, int((t1 - t0) * fps)); segs = len(pts) - 1
    for i in range(n + 1):
        u = i / n * segs; k = min(int(u), segs - 1); v = u - k
        v = v * v * (3 - 2 * v)
        (x0, y0), (x1, y1) = pts[k], pts[k + 1]
        out.append((t0 + i / fps, 'move', (int(x0 + (x1 - x0) * v), int(y0 + (y1 - y0) * v))))
    return out
def seg(timeline, secs, setup=(), before=()):
    return (list(setup), list(before), timeline, secs)
def typing(t0, text, step=0.12):
    return [(t0 + i * step, 'type', ch) for i, ch in enumerate(text)]
CLIPS = [
 ('h_dock', [seg(path(0.3, 4.5, [(300, 600), (372, 735), (650, 735), (420, 733), (520, 650)]), 5.5)]),
 ('h_network', [seg([(0.5, 'click', (771, 11))] + path(1.5, 4, [(771, 30), (700, 80), (700, 140), (760, 200)]) + [(5.2, 'key', 'esc')], 6)]),
 ('h_sound', [seg([(0.5, 'click', (801, 11))] + path(1.3, 2.0, [(801, 30), (790, 82)]) + [(2.1, 'down', None)] + path(2.2, 4.4, [(790, 82), (600, 82), (740, 82)]) + [(4.5, 'up', None), (5.4, 'key', 'esc')], 6)]),
 ('h_general', [seg([(0.4, 'key', 'ctrl-spc')] + typing(1.2, 'calc') + [(2.8, 'key', 'backspace'), (2.95, 'key', 'backspace'), (3.1, 'key', 'backspace'), (3.25, 'key', 'backspace')] + typing(3.5, '12*7'), 6)]),
 ('h_accessibility', [seg(path(0.3, 2.8, [(700, 500), (500, 380)]), 3, before=[('open', 'Files')]),
                      seg(path(0.3, 2.8, [(500, 380), (700, 500)]), 3, setup=['config colorfilter invert'])]),
 ('h_appearance', [seg(path(0.3, 2.8, [(700, 500), (500, 380)]), 3.2, setup=['config appearance light'], before=[('open', 'Files')]),
                   seg(path(0.3, 2.8, [(500, 380), (700, 500)]), 3.2, setup=['config appearance dark'])]),
 ('h_displays', [seg(path(0.3, 2.8, [(700, 500), (500, 380)]), 3, setup=['config appearance light'], before=[('open', 'Files')]),
                 seg(path(0.3, 2.8, [(500, 380), (700, 500)]), 3, setup=['config nightshift on'])]),
 ('h_lock', [seg([], 4, setup=['lock']), seg([(0.5, 'key', 'ret')], 3)]),
 ('h_privacy', [seg(typing(1.0, 'secret', 0.35), 5, setup=['lock'])]),
 ('h_users', [seg(path(0.5, 4, [(512, 500), (512, 420)]), 5, setup=['logout'])]),
 ('h_devices', [seg(path(0.3, 4, [(700, 500), (300, 300), (400, 250)]), 5, before=[('open', 'Files')])]),
 ('h_language', [seg([(0.4, 'key', 'ctrl-spc')] + typing(1.2, 'language'), 5)]),
 ('h_keyboard', [seg(typing(0.5, 'Typing on SynthOS feels quick.', 0.11), 5, before=[('open', 'Notepad')])]),
 ('h_mouse', [seg(path(0.2, 1.0, [(600, 500), (520, 420)]) + [(1.2, 'rclick', (520, 420))] + path(1.8, 4.2, [(520, 420), (560, 440), (560, 480), (560, 520)]) + [(5.0, 'key', 'esc')], 6)]),
 ('h_apps', [seg([(0.5, 'click', (386, 728))] + path(1.2, 4.5, [(386, 728), (420, 300), (600, 320)]) + [(5.5, 'key', 'esc')], 6)]),
 ('h_terminal', [seg(typing(0.3, 'ls /system\n', 0.1) + typing(2.5, 'uptime\n', 0.1), 5, before=[('key', 'ctrl-alt-t'), ('sleep', 1.0)])]),
 ('h_screenshot', [seg(path(0.3, 4, [(600, 500), (500, 400)]), 5, before=[('open', 'Screenshot')])]),
 ('h_ai', [seg([(0.4, 'click', (192, 115))] + typing(1.0, 'Assistant', 0.12) + path(2.4, 4.4, [(192, 115), (420, 330)]), 5, before=[('open', 'App Store')])]),
 ('h_about', [seg(path(0.3, 4, [(600, 500), (500, 400)]), 5, before=[('open', 'System Information')])]),
 ('h_storage', [seg(path(0.3, 4, [(600, 500), (500, 400)]), 5, before=[('open', 'Disk Utility')])]),
 ('h_datetime', [seg([(0.5, 'click', (945, 11))] + path(1.0, 4, [(945, 30), (900, 200)]) + [(5.5, 'key', 'esc')], 6)]),
 ('h_update', [seg(path(0.3, 4, [(600, 500), (500, 400)]), 5, before=[('open', 'App Store')])]),
]
