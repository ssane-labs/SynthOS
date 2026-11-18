"""What tools/previews/mkpreviews.py cuts out of the recordings: (clip name, recording, crop box on the 1024x768
screen, size, segments). Pane clips are "h_<pane>", the clip of one value of a setting "s_<key>_<value>" (numbers
as "lo" and "hi")."""

HERO = (600, 260)
THUMB = (160, 100)
MID = (0, 100, 1024, 544)
TOP_RIGHT = (424, 0, 1024, 260)
BOTTOM = (0, 324, 1024, 768)
DOCK = (352, 568, 672, 768)
SCREEN = (0, 0, 1024, 640)
CC = (560, 0, 1024, 290)

CLIPS = [
    ("h_dock", "h_dock", (112, 424, 912, 768), HERO, 1),
    ("h_network", "h_network", TOP_RIGHT, HERO, 1),
    ("h_sound", "h_sound", TOP_RIGHT, HERO, 1),
    ("h_datetime", "h_datetime", (324, 0, 1024, 303), HERO, 1),
    ("h_general", "h_general", (112, 120, 912, 467), HERO, 1),
    ("h_language", "h_language", (112, 120, 912, 467), HERO, 1),
    ("h_accessibility", "h_accessibility", MID, HERO, 2),
    ("h_appearance", "h_appearance", MID, HERO, 2),
    ("h_displays", "h_displays", MID, HERO, 2),
    ("h_devices", "h_devices", MID, HERO, 1),
    ("h_keyboard", "h_keyboard", MID, HERO, 1),
    ("h_screenshot", "h_screenshot", MID, HERO, 1),
    ("h_ai", "h_ai", MID, HERO, 1),
    ("h_about", "h_about", MID, HERO, 1),
    ("h_storage", "h_storage", MID, HERO, 1),
    ("h_update", "h_update", MID, HERO, 1),
    ("h_terminal", "h_terminal", MID, HERO, 1),
    ("h_apps", "h_apps", (0, 120, 1024, 564), HERO, 1),
    ("h_mouse", "h_mouse", (0, 160, 1024, 604), HERO, 1),
    ("h_lock", "h_lock", (0, 0, 1024, 444), HERO, 2),
    ("h_users", "h_users", BOTTOM, HERO, 1),
    ("h_privacy", "h_privacy", BOTTOM, HERO, 1),
]

for key, values in [("dockhalo", ["on", "off"]), ("dockspot", ["on", "off"]), ("dockfx", ["subtle", "normal", "vivid"]),
                    ("magnify", ["off", "on"]), ("dockdots", ["off", "on"]), ("docksize", ["small", "medium", "large"]),
                    ("dockbounce", ["off", "on"]), ("colorfilter", ["grayscale", "invert", "contrast", "none"]),
                    ("iconshape", ["rounded", "circle", "square", "leaf", "squircle"])]:
    for v in values:
        CLIPS.append((f"s_{key}_{v}", f"s_{key}_{v}", DOCK, THUMB, 1))
for key, values in [("dockstyle", ["corner", "side", "bottom"]), ("dockmode", ["autohide", "off", "show"]),
                    ("appearance", ["light", "dark"]), ("nightshift", ["off", "on"]), ("animations", ["off", "on"])]:
    for v in values:
        CLIPS.append((f"s_{key}_{v}", f"s_{key}_{v}", SCREEN, THUMB, 1))
CLIPS += [("s_appearance_auto", "s_appearance_dark", SCREEN, THUMB, 1),
          ("s_appearance_schedule", "s_appearance_dark", SCREEN, THUMB, 1),
          ("s_nightshift_auto", "s_nightshift_on", SCREEN, THUMB, 1)]
for key, lo, hi, box in [("nightwarmth", 0, 100, SCREEN), ("brightness", 30, 100, SCREEN), ("glassstrength", 10, 100, CC),
                         ("glassedge", 0, 100, CC), ("glassfrost", 0, 100, CC)]:
    CLIPS += [(f"s_{key}_lo", f"s_{key}_{lo}", box, THUMB, 1), (f"s_{key}_hi", f"s_{key}_{hi}", box, THUMB, 1)]
for key, values in [("glass", ["frosted", "liquid"]), ("glasseffect", ["regular", "clear", "tinted", "smoky", "prism", "halo"])]:
    for v in values:
        CLIPS.append((f"s_{key}_{v}", f"s_{key}_{v}", CC, THUMB, 1))
