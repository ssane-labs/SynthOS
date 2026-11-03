#include <synth/error.h>
#include <synth/keys.h>
#include <synth/shell.h>
#include <synth/string.h>

/* Keyboard layouts on top of the US key codes the drivers deliver: a typed character is looked up in the layout's
   "from" keys and comes back as a text key with the code point at the same place in "to" (UTF-8). The enabled
   layouts keep the order they were added in; the first is the primary one. Win+Space moves to the next; shortcuts
   are never translated. */

struct layout {
    const char *name, *label, *title, *native, *from, *to;
};

#define RU_FROM "qwertyuiop[]asdfghjkl;'zxcvbnm,.`QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>~/?@$^&|"
#define NORDIC_FROM ";'[:\"{-=]\\/?@$^&*()_+}|<>`~"
#define CZ_FROM "1234567890-=[];'\\`yzYZ!@#$%^&*()_{}:\"|<>?/~"

static const struct layout layouts[] = {
    {"us", "EN", "English (US)", "English", "", ""},
    {"ru", "RU", "Russian", "Русский", RU_FROM,
     "йцукенгшщзхъфывапролджэячсмитьбюёЙЦУКЕНГШЩЗХЪФЫВАПРОЛДЖЭЯЧСМИТЬБЮЁ.,\";:?/"},
    {"ua", "UK", "Ukrainian", "Українська", "qwertyuiop[]asdfghjkl;'zxcvbnm,.`QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>\\|/?@$^&",
     "йцукенгшщзхїфівапролджєячсмитьбю'ЙЦУКЕНГШЩЗХЇФІВАПРОЛДЖЄЯЧСМИТЬБЮґҐ.,\";:?"},
    {"de", "DE", "German", "Deutsch", "yzYZ;:'\"[{-]}/?@#^&*()_<>`~\\|", "zyZYöÖäÄüÜß+*-_\"§&/()=?;:^°#'"},
    {"fr", "FR", "French", "Français", "qawzQAWZ;:mM,<.>/?1234567890-!@#$%^&*()_]}'\"\\|`",
     "aqzwAQZWmM,?;.:/!§&é\"'(-è_çà)1234567890°$£ù%*µ²"},
    {"es", "ES", "Spanish", "Español", ";:-_=+/?`~@#^&*()<>]}\\|", "ñÑ'?¡¿-_ºª\"·&/()=;:+*çÇ"},
    {"it", "IT", "Italian", "Italiano", ";'[]\\/-=:\"{}|@#^&*()_+<>?`~", "òàè+ù-'ìç°é*§\"£&/()=?^;:_\\|"},
    {"pt", "PT", "Portuguese", "Português", ";'[]\\-=/`:\"{}|_+?@^&*()<>~", "çº+´~'«-\\Çª*`^?»_\"&/()=;:|"},
    {"pl", "PL", "Polish", "Polski", "yzYZ;:'\"[{]}\\|-_=+/?@^&*()<>", "zyZYłŁąĄżńśćóź+?'*-_\"&/()=;:"},
    {"gb", "GB", "English (UK)", "English (UK)", "@\"#\\|~", "\"@£#~¬"},
    {"dvorak", "DV", "Dvorak", "Dvorak",
     "qwertyuiop[]asdfghjkl;'zxcvbnm,./-=QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM<>?_+",
     "',.pyfgcrl/=aoeuidhtns-;qjkxbmwvz[]\"<>PYFGCRL?+AOEUIDHTNS_:QJKXBMWVZ{}"},
    {"colemak", "CM", "Colemak", "Colemak", "ertyuiopsdfgjkl;nERTYUIOPSDFGJKL:N",
     "fpgjluy;rstdneiokFPGJLUY:RSTDNEIOK"},
    {"by", "BE", "Belarusian", "Беларуская", RU_FROM,
     "йцукенгшўзх'фывапролджэячсмітьбюёЙЦУКЕНГШЎЗХ'ФЫВАПРОЛДЖЭЯЧСМІТЬБЮЁ.,\";:?/"},
    {"bg", "BG", "Bulgarian", "Български", "qwertyuiop[]asdfghjkl;'zxcvbnm`\\QWERTYUIOP{}ASDFGHJKL:\"ZXCVBNM~|",
     "явертъуиопшщасдфгхйкл;'зьцжбнмчюЯВЕРТЪУИОПШЩАСДФГХЙКЛ:\"ЗЬЦЖБНМЧЮ"},
    {"rs", "SR", "Serbian", "Српски", "qwertyuiop[]asdfghjkl;'\\zxcvbnmQWERTYUIOP{}ASDFGHJKL:\"|ZXCVBNM",
     "љњертзуиопшђасдфгхјклчћжѕџцвбнмЉЊЕРТЗУИОПШЂАСДФГХЈКЛЧЋЖЅЏЦВБНМ"},
    {"cz", "CS", "Czech", "Čeština", CZ_FROM, "+ěščřžýáíé=´ú)ů§¨;zyZY1234567890%/(\"!'?:_-°"},
    {"sk", "SK", "Slovak", "Slovenčina", CZ_FROM, "+ľščťžýáíé=´úäô§ň;zyZY1234567890%/(\"!)?:_-°"},
    {"hu", "HU", "Hungarian", "Magyar", "`0-=[];'\\yzYZ~!@#^&*()_+{}:\"|<>?/", "0öüóőúéáűzyZY§'\"+/=()ÖÜÓŐÚÉÁŰ?:_-"},
    {"ro", "RO", "Romanian", "Română", "[]\\;'{}|:\"", "ăîâşţĂÎÂŞŢ"},
    {"tr", "TR", "Turkish", "Türkçe", "i[];',./\\\"{}:<>?|", "ığüşiöç.,İĞÜŞÖÇ:;"},
    {"se", "SV", "Swedish", "Svenska", NORDIC_FROM, "öäåÖÄÅ+´¨'-_\"¤&/()=?`^*;:§½"},
    {"fi", "FI", "Finnish", "Suomi", NORDIC_FROM, "öäåÖÄÅ+´¨'-_\"¤&/()=?`^*;:§½"},
    {"no", "NO", "Norwegian", "Norsk", NORDIC_FROM, "øæåØÆÅ+\\¨'-_\"¤&/()=?`^*;:|§"},
    {"dk", "DA", "Danish", "Dansk", NORDIC_FROM, "æøåÆØÅ+´¨'-_\"¤&/()=?`^*;:½§"},
    {"ee", "ET", "Estonian", "Eesti", ";'[]:\"{}-=\\/?@$^&*()_+|<>", "öäüõÖÄÜÕ+´'-_\"¤&/()=?`*;:"},
    {"lt", "LT", "Lithuanian", "Lietuvių", "12345678=!@#$%^&*+", "ąčęėįšųūžĄČĘĖĮŠŲŪŽ"},
    {"gr", "EL", "Greek", "Ελληνικά", "qwertyuiopasdfghjklzxcvbnmQERTYUIOPASDFGHJKLZXCVBNM",
     ";ςερτυθιοπασδφγηξκλζχψωβνμ:ΕΡΤΥΘΙΟΠΑΣΔΦΓΗΞΚΛΖΧΨΩΒΝΜ"},
};

#define LAYOUTS ((int)ARRAY_SIZE(layouts))

static uint8_t order[ARRAY_SIZE(layouts)];
static int order_n = 1;
static int active;

static uint32_t utf8_at(const char *s, int index)
{
    const unsigned char *p = (const unsigned char *)s;
    for (;;) {
        uint32_t cp = *p++;
        int more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
        cp &= more == 3 ? 0x07 : more == 2 ? 0x0f : more == 1 ? 0x1f : 0x7f;
        while (more-- && *p)
            cp = cp << 6 | (*p++ & 0x3f);
        if (!index--)
            return cp;
    }
}

int keymap_count(void)
{
    return LAYOUTS;
}

const char *keymap_layout_name(int index, const char **title)
{
    if (index < 0 || index >= LAYOUTS)
        return NULL;
    if (title)
        *title = layouts[index].title;
    return layouts[index].name;
}

const char *keymap_native(int index)
{
    return index >= 0 && index < LAYOUTS ? layouts[index].native : NULL;
}

const char *keymap_short(int index)
{
    return index >= 0 && index < LAYOUTS ? layouts[index].label : NULL;
}

bool keymap_enabled(int index)
{
    for (int i = 0; i < order_n; i++)
        if (order[i] == index)
            return true;
    return false;
}

int keymap_order(int *out, int max)
{
    int n = MIN(order_n, max);
    for (int i = 0; i < n; i++)
        out[i] = order[i];
    return n;
}

int keymap_active(void)
{
    return active;
}

bool keymap_multi(void)
{
    return order_n > 1;
}

const char *keymap_label(void)
{
    return layouts[active].label;
}

void keymap_next(void)
{
    int k = 0;
    while (k < order_n && order[k] != active)
        k++;
    active = order[(k + 1) % order_n];
}

int keymap_translate(int key)
{
    if (KEY_IS_TEXT(key) || (key & (KEY_MOD_CTRL | KEY_MOD_ALT | KEY_MOD_SUPER)) || !active)
        return key;
    int base = KEY_BASE(key);
    if (base < 0x20 || base > 0x7e)
        return key;
    const char *at = strchr(layouts[active].from, base);
    if (!at)
        return key;
    return KEY_TEXT_CHAR(utf8_at(layouts[active].to, (int)(at - layouts[active].from)), base);
}

int keymap_plain(int key)
{
    return KEY_IS_TEXT(key) ? KEY_TEXT_PHYS(key) : key;
}

int keymap_set_enabled(const char *list)
{
    uint8_t want[ARRAY_SIZE(layouts)];
    int n = 0;
    for (const char *p = list; *p;) {
        size_t len = 0;
        while (p[len] && p[len] != ',')
            len++;
        int found = -1;
        for (int i = 0; i < LAYOUTS; i++)
            if (strlen(layouts[i].name) == len && !strncmp(layouts[i].name, p, len))
                found = i;
        if (found < 0)
            return E_INVAL;
        bool dup = false;
        for (int i = 0; i < n; i++)
            dup = dup || want[i] == found;
        if (!dup)
            want[n++] = (uint8_t)found;
        p += len + (p[len] == ',');
    }
    if (!n)
        want[n++] = 0;
    bool new_primary = want[0] != order[0];
    memcpy(order, want, (size_t)n);
    order_n = n;
    if (new_primary || !keymap_enabled(active))
        active = order[0];
    return 0;
}

static void get_keyboard(char *buf, size_t cap)
{
    buf[0] = 0;
    for (int i = 0; i < order_n; i++) {
        if (i)
            str_append(buf, ",", cap);
        str_append(buf, layouts[order[i]].name, cap);
    }
}

static int set_keyboard(struct shell *sh, const char *value)
{
    UNUSED(sh);
    return keymap_set_enabled(value);
}

static const struct setting s_keyboard = {"keyboard", "<layout>,<layout>,... (us, ru, ua, de, fr, es, it, pt, pl, gb, "
                                          "dvorak, colemak, by, bg, rs, cz, sk, hu, ro, tr, se, fi, no, dk, ee, lt, gr)",
                                          "input languages, the primary one first; Win+Space switches between them",
                                          get_keyboard, set_keyboard};
SETTING(s_keyboard);
