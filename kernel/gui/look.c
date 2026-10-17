#include <synth/config.h>
#include <synth/error.h>
#include <synth/shell.h>
#include <synth/string.h>

#include "internal.h"

static const struct gui_look looks[] = {
    {"dune", "Dune", "Warm desert glass, places on the left, launcher in the corner", false, RGB(214, 150, 96),
     RGB(212, 116, 46), DOCK_CORNER, STATUS_PILL, true, 1u << WIDGET_CLOCK | 1u << WIDGET_WEATHER | 1u << WIDGET_MUSIC},
    {"mist", "Mist", "Quiet morning fog, menu bar and a floating dock", false, RGB(120, 140, 170), RGB(78, 112, 214),
     DOCK_CENTER, STATUS_MENUBAR, false, 1u << WIDGET_CALENDAR},
    {"night", "Night", "Deep dark sky, an app rail and music on the desktop", true, RGB(110, 92, 200),
     RGB(150, 128, 255), DOCK_RAIL, STATUS_PILL, false,
     1u << WIDGET_CLOCK | 1u << WIDGET_MUSIC | 1u << WIDGET_VISUALIZER},
};

static int current = 0;

const struct gui_look *look_current(void)
{
    return &looks[current];
}

int look_count(void)
{
    return (int)ARRAY_SIZE(looks);
}

const struct gui_look *look_get(int index)
{
    return index >= 0 && index < look_count() ? &looks[index] : NULL;
}

int menubar_height(void)
{
    return looks[current].status == STATUS_MENUBAR ? MENUBAR_HEIGHT : 0;
}

static void get_look(char *buf, size_t cap)
{
    str_copy(buf, looks[current].name, cap);
}

static int set_look(struct shell *sh, const char *value)
{
    UNUSED(sh);
    for (int i = 0; i < look_count(); i++)
        if (!str_casecmp(value, looks[i].name)) {
            if (i != current) {
                current = i;
                gui_look_changed();
            }
            return 0;
        }
    return E_INVAL;
}

static const struct setting s_look = {"look", "dune|mist|night", "desktop design: colors, wallpaper and layout",
                                      get_look, set_look};
SETTING(s_look);
