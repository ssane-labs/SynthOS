/* Chinese, Japanese and Korean: input methods and the characters to show them.
 *
 * Korean (2-Set) builds syllables from jamo as they are typed. Japanese turns romaji into kana; Space offers
 * kanji, katakana and words. Chinese takes Pinyin and offers characters and common words, picked with Space or
 * a digit. A small panel near the window shows what is being composed and the choices.
 *
 * The desktop fonts have no CJK characters, so the language pack (`languages install`, or the Download button
 * when such an input source is first used) fetches two free files: GNU Unifont's 16-pixel glyphs (SIL OFL 1.1 /
 * GPL with font exception) from unifoundry.com, and the Unihan readings (Unicode License) from unicode.org,
 * which give every common character its Pinyin and its Japanese readings. They go to /home/fonts/cjk. */
#include <synth/command.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/http.h>
#include <synth/inflate.h>
#include <synth/keys.h>
#include <synth/module.h>
#include <synth/shell.h>
#include <synth/string.h>
#include <synth/tls.h>
#include <synth/vfs.h>
#include <synth/zip.h>

#include "../internal.h"

#define PACK_DIR "/home/fonts/cjk"
#define FONT_FILE PACK_DIR "/unifont.hex.gz"
#define DICT_FILE PACK_DIR "/ime.dat"
#define FONT_URL "https://unifoundry.com/pub/unifont/unifont-15.1.05/font-builds/unifont-15.1.05.hex.gz"
#define UNIHAN_URL "https://www.unicode.org/Public/15.1.0/ucd/Unihan.zip"
#define MAX_CANDS 60
#define PAGE 9

/* ---- UTF-8 ---- */

static int put_utf8(char *out, uint32_t cp)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | cp >> 6);
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    out[0] = (char)(0xe0 | cp >> 12);
    out[1] = (char)(0x80 | (cp >> 6 & 0x3f));
    out[2] = (char)(0x80 | (cp & 0x3f));
    return 3;
}

static int decode(const char *s, uint32_t *out, int max)
{
    int n = 0;
    while (*s && n < max)
        out[n++] = utf8_next(&s);
    return n;
}

/* ---- the glyphs (GNU Unifont) ---- */

static uint32_t *glyph_at; /* per BMP code point: offset + 1 into glyph_data, bit 31 for 16 wide */
static uint8_t *glyph_data;
static int glyph_count;
static bool glyphs_tried;

static void load_glyphs(void)
{
    glyphs_tried = true;
    char *gz;
    size_t gz_len;
    if (vfs_read_all(FONT_FILE, &gz, &gz_len) != 0)
        return;
    uint8_t *hex;
    size_t len;
    int err = gunzip_alloc(gz, gz_len, 16u << 20, &hex, &len);
    kfree(gz);
    if (err)
        return;
    int lines = 0;
    for (size_t i = 0; i < len; i++)
        lines += hex[i] == '\n';
    glyph_at = kzalloc(sizeof(uint32_t) * 65536);
    glyph_data = kmalloc((size_t)lines * 32 + 32);
    if (!glyph_at || !glyph_data) {
        kfree(glyph_at);
        kfree(glyph_data);
        glyph_at = NULL;
        glyph_data = NULL;
        kfree(hex);
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < len;) {
        size_t start = i;
        while (i < len && hex[i] != '\n')
            i++;
        size_t end = i++;
        uint32_t cp = 0;
        size_t p = start;
        while (p < end && hex[p] != ':')
            cp = cp << 4 | (uint32_t)(is_digit(hex[p]) ? hex[p] - '0' : (to_lower(hex[p]) - 'a' + 10) & 15), p++;
        size_t digits = end - p - 1;
        if (p >= end || cp > 0xffff || (digits != 32 && digits != 64) || cp < 0x0590)
            continue;
        int bytes = (int)digits / 2;
        for (int k = 0; k < bytes; k++) {
            const uint8_t *h = &hex[p + 1 + k * 2];
            int hi = is_digit(h[0]) ? h[0] - '0' : (to_lower(h[0]) - 'a' + 10) & 15;
            int lo = is_digit(h[1]) ? h[1] - '0' : (to_lower(h[1]) - 'a' + 10) & 15;
            glyph_data[used + (size_t)k] = (uint8_t)(hi << 4 | lo);
        }
        glyph_at[cp] = (uint32_t)(used + 1) | (bytes == 32 ? 0x80000000u : 0);
        used += (size_t)bytes;
        glyph_count++;
    }
    kfree(hex);
}

static const uint8_t *wide_glyph(uint32_t code, int *width)
{
    if (code > 0xffff)
        return NULL;
    if (!glyphs_tried)
        load_glyphs();
    if (!glyph_at || !glyph_at[code])
        return NULL;
    *width = glyph_at[code] & 0x80000000u ? 16 : 8;
    return glyph_data + (glyph_at[code] & 0x7fffffffu) - 1;
}

/* ---- the readings (ime.dat: "z|j <TAB> reading <TAB> characters", best first) ---- */

struct reading {
    char lang;
    char key[15];
    const char *chars;
};

static struct reading *readings;
static int nreadings;
static char *dict_text;
static bool dict_tried;

static int reading_cmp(char lang, const char *key, const struct reading *r)
{
    if (lang != r->lang)
        return lang < r->lang ? -1 : 1;
    return strcmp(key, r->key);
}

static void load_dict(void)
{
    dict_tried = true;
    size_t size;
    if (vfs_read_all(DICT_FILE, &dict_text, &size) != 0)
        return;
    int lines = 0;
    for (size_t i = 0; i < size; i++)
        lines += dict_text[i] == '\n';
    readings = kmalloc(sizeof(struct reading) * (size_t)(lines + 1));
    if (!readings)
        return;
    for (char *line = dict_text; line < dict_text + size;) {
        char *end = memchr(line, '\n', (size_t)(dict_text + size - line));
        if (end)
            *end = 0;
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (t1 && t2 && t2 - t1 - 1 < 15) {
            struct reading *r = &readings[nreadings++];
            r->lang = line[0];
            memcpy(r->key, t1 + 1, (size_t)(t2 - t1 - 1));
            r->key[t2 - t1 - 1] = 0;
            r->chars = t2 + 1;
        }
        if (!end)
            break;
        line = end + 1;
    }
    /* the file is written sorted; keep it robust against a hand-edited one */
    for (int i = 1; i < nreadings; i++) {
        struct reading t = readings[i];
        int j = i;
        while (j > 0 && reading_cmp(t.lang, t.key, &readings[j - 1]) < 0) {
            readings[j] = readings[j - 1];
            j--;
        }
        readings[j] = t;
    }
}

static const char *lookup(char lang, const char *key)
{
    if (!dict_tried)
        load_dict();
    int lo = 0, hi = nreadings - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = reading_cmp(lang, key, &readings[mid]);
        if (!c)
            return readings[mid].chars;
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return NULL;
}

static bool pack_installed(void)
{
    return vfs_exists(FONT_FILE) && vfs_exists(DICT_FILE);
}

/* ---- common words, ours: the readings give single characters ---- */

static const char *const zh_words[] = {
    "nihao=你好", "xiexie=谢谢", "zaijian=再见", "women=我们", "nimen=你们", "tamen=他们", "zhongguo=中国",
    "zhongwen=中文", "hanzi=汉字", "pengyou=朋友", "jintian=今天", "mingtian=明天", "zuotian=昨天",
    "xianzai=现在", "shijian=时间", "shenme=什么", "meiyou=没有", "keyi=可以", "zhidao=知道", "xihuan=喜欢",
    "yiqi=一起", "gongzuo=工作", "xuexi=学习", "xuesheng=学生", "laoshi=老师", "dianhua=电话",
    "diannao=电脑", "shouji=手机", "wenti=问题", "yinwei=因为", "suoyi=所以", "danshi=但是", "ruguo=如果",
    "yijing=已经", "haishi=还是", "duibuqi=对不起", "meiguanxi=没关系", "bukeqi=不客气", "beijing=北京",
    "shanghai=上海", "shijie=世界", "guojia=国家", "zhengfu=政府", "jingji=经济", "wenhua=文化",
    "yinyue=音乐", "dianying=电影", "kafei=咖啡", "chifan=吃饭", "shuijiao=睡觉", "kaishi=开始",
    "jieshu=结束", "wangluo=网络", "ruanjian=软件", "xitong=系统", "wenjian=文件", "chengxu=程序",
    "shuru=输入", "shezhi=设置", "bangzhu=帮助", "zhunbei=准备", "xiwang=希望", "yiding=一定",
    "yixia=一下", "dajia=大家", "zenme=怎么", "weishenme=为什么", "nali=哪里", "zheli=这里", "nage=那个",
    "zhege=这个", "yidian=一点", "feichang=非常", "kuaile=快乐", "shengri=生日", "xinnian=新年",
};

static const char *const ja_words[] = {
    "nihon=日本", "nippon=日本", "nihongo=日本語", "watashi=私", "anata=貴方", "kyou=今日", "ashita=明日",
    "kinou=昨日", "ima=今", "jikan=時間", "toukyou=東京", "oosaka=大阪", "kyouto=京都", "gakkou=学校",
    "sensei=先生", "gakusei=学生", "tomodachi=友達", "kazoku=家族", "denwa=電話", "kaisha=会社",
    "shigoto=仕事", "sekai=世界", "kuruma=車", "densha=電車", "eki=駅", "mizu=水", "taberu=食べる",
    "nomu=飲む", "iku=行く", "kuru=来る", "miru=見る", "suru=為る", "hanasu=話す", "kiku=聞く",
    "yomu=読む", "kaku=書く", "ookii=大きい", "chiisai=小さい", "atarashii=新しい", "furui=古い",
    "takai=高い", "yasui=安い", "ii=良い", "warui=悪い", "nan=何", "nani=何", "dare=誰", "doko=何処",
    "itsu=何時", "kanji=漢字", "hiragana=平仮名", "katakana=片仮名", "eigo=英語", "benkyou=勉強",
    "shinbun=新聞", "tenki=天気", "ame=雨", "yuki=雪", "haru=春", "natsu=夏", "aki=秋", "fuyu=冬",
    "asa=朝", "hiru=昼", "yoru=夜", "ohayou=お早う", "arigatou=有難う", "sayounara=さようなら",
    "konnichiha=今日は", "konbanha=今晩は", "daijoubu=大丈夫", "onegai=お願い", "kokoro=心",
    "yume=夢", "ai=愛", "hito=人", "otoko=男", "onna=女", "kodomo=子供", "namae=名前", "ongaku=音楽",
    "eiga=映画", "shashin=写真", "konpyuuta=コンピュータ", "intaanetto=インターネット",
};

/* ---- kana ---- */

static const struct {
    const char *romaji, *kana;
} kana_table[] = {
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"}, {"sha", "しゃ"}, {"shu", "しゅ"}, {"sho", "しょ"},
    {"she", "しぇ"}, {"sya", "しゃ"}, {"syu", "しゅ"}, {"syo", "しょ"}, {"cha", "ちゃ"}, {"chu", "ちゅ"},
    {"cho", "ちょ"}, {"che", "ちぇ"}, {"tya", "ちゃ"}, {"tyu", "ちゅ"}, {"tyo", "ちょ"}, {"nya", "にゃ"},
    {"nyu", "にゅ"}, {"nyo", "にょ"}, {"hya", "ひゃ"}, {"hyu", "ひゅ"}, {"hyo", "ひょ"}, {"mya", "みゃ"},
    {"myu", "みゅ"}, {"myo", "みょ"}, {"rya", "りゃ"}, {"ryu", "りゅ"}, {"ryo", "りょ"}, {"gya", "ぎゃ"},
    {"gyu", "ぎゅ"}, {"gyo", "ぎょ"}, {"jya", "じゃ"}, {"jyu", "じゅ"}, {"jyo", "じょ"}, {"bya", "びゃ"},
    {"byu", "びゅ"}, {"byo", "びょ"}, {"pya", "ぴゃ"}, {"pyu", "ぴゅ"}, {"pyo", "ぴょ"}, {"shi", "し"},
    {"chi", "ち"}, {"tsu", "つ"}, {"xtu", "っ"}, {"ltu", "っ"}, {"xya", "ゃ"}, {"xyu", "ゅ"}, {"xyo", "ょ"},
    {"ja", "じゃ"}, {"ju", "じゅ"}, {"jo", "じょ"}, {"je", "じぇ"}, {"fa", "ふぁ"}, {"fi", "ふぃ"},
    {"fe", "ふぇ"}, {"fo", "ふぉ"}, {"wi", "うぃ"}, {"we", "うぇ"}, {"ka", "か"}, {"ki", "き"}, {"ku", "く"},
    {"ke", "け"}, {"ko", "こ"}, {"sa", "さ"}, {"si", "し"}, {"su", "す"}, {"se", "せ"}, {"so", "そ"},
    {"ta", "た"}, {"ti", "ち"}, {"tu", "つ"}, {"te", "て"}, {"to", "と"}, {"na", "な"}, {"ni", "に"},
    {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"}, {"ha", "は"}, {"hi", "ひ"}, {"fu", "ふ"}, {"hu", "ふ"},
    {"he", "へ"}, {"ho", "ほ"}, {"ma", "ま"}, {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"},
    {"ya", "や"}, {"yu", "ゆ"}, {"yo", "よ"}, {"ra", "ら"}, {"ri", "り"}, {"ru", "る"}, {"re", "れ"},
    {"ro", "ろ"}, {"wa", "わ"}, {"wo", "を"}, {"ga", "が"}, {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"},
    {"go", "ご"}, {"za", "ざ"}, {"ji", "じ"}, {"zi", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"},
    {"da", "だ"}, {"di", "ぢ"}, {"du", "づ"}, {"de", "で"}, {"do", "ど"}, {"ba", "ば"}, {"bi", "び"},
    {"bu", "ぶ"}, {"be", "べ"}, {"bo", "ぼ"}, {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"}, {"pe", "ぺ"},
    {"po", "ぽ"}, {"va", "ゔぁ"}, {"vu", "ゔ"}, {"xa", "ぁ"}, {"xi", "ぃ"}, {"xu", "ぅ"}, {"xe", "ぇ"},
    {"xo", "ぉ"}, {"la", "ぁ"}, {"li", "ぃ"}, {"lu", "ぅ"}, {"le", "ぇ"}, {"lo", "ぉ"}, {"nn", "ん"},
    {"a", "あ"}, {"i", "い"}, {"u", "う"}, {"e", "え"}, {"o", "お"}, {"-", "ー"},
};

static bool is_vowel(int c)
{
    return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o';
}

/* romaji to hiragana, as far as it goes: what is left (a consonant waiting for its vowel) stays in *rest;
   final: the typing is over, so a last "n" is ん */
static int romaji_to_kana(const char *in, uint32_t *out, int max, const char **rest, bool final)
{
    int n = 0;
    const char *p = in;
    while (*p && n < max - 2) {
        if (p[0] == 'n' && ((p[1] && p[1] != 'y' && !is_vowel(p[1]) && (p[1] != 'n' || is_vowel(p[2]) || p[2] == 'y')) ||
                            (!p[1] && final))) {
            out[n++] = 0x3093; /* ん before another consonant */
            p++;
            continue;
        }
        if (p[0] == p[1] && !is_vowel(p[0]) && p[0] != 'n' && is_alpha(p[0])) {
            out[n++] = 0x3063; /* っ: a doubled consonant */
            p++;
            continue;
        }
        bool done = false, prefix = false;
        for (size_t i = 0; i < ARRAY_SIZE(kana_table) && !done; i++) {
            size_t k = strlen(kana_table[i].romaji);
            if (!strncmp(p, kana_table[i].romaji, k)) {
                n += decode(kana_table[i].kana, out + n, max - n);
                p += k;
                done = true;
            } else if (!strncmp(kana_table[i].romaji, p, strlen(p))) {
                prefix = true;
            }
        }
        if (done)
            continue;
        if (prefix)
            break; /* still typing this one */
        out[n++] = (uint32_t)(unsigned char)*p++; /* not romaji: keep the letter */
    }
    if (rest)
        *rest = p;
    return n;
}

/* hiragana back to romaji in the readings' spelling (CHUU, NAKA): the first table entry for each kana wins */
static bool kana_to_romaji(const uint32_t *kana, int n, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = 0;
    for (int i = 0; i < n;) {
        if (kana[i] == 0x3063 && i + 1 < n) { /* っ doubles the next consonant */
            char next[8];
            if (!kana_to_romaji(&kana[i + 1], 1, next, sizeof(next)) || !next[0] || len + 1 >= cap)
                return false;
            out[len++] = next[0];
            out[len] = 0;
            i++;
            continue;
        }
        bool found = false;
        for (int take = 2; take >= 1 && !found; take--) {
            if (i + take > n)
                continue;
            for (size_t t = 0; t < ARRAY_SIZE(kana_table) && !found; t++) {
                uint32_t k[4];
                int kn = decode(kana_table[t].kana, k, 4);
                if (kn != take || memcmp(k, &kana[i], sizeof(uint32_t) * (size_t)take) || kana_table[t].romaji[0] == '-')
                    continue;
                if (!strcmp(kana_table[t].romaji, "nn")) {
                    if (len + 1 >= cap)
                        return false;
                    out[len++] = 'n';
                    out[len] = 0;
                } else {
                    if (len + strlen(kana_table[t].romaji) >= cap)
                        return false;
                    str_append(out, kana_table[t].romaji, cap);
                    len = strlen(out);
                }
                i += take;
                found = true;
            }
        }
        if (!found)
            return false;
    }
    return len > 0;
}

/* ---- the composition ---- */

struct cand {
    char text[40];
    int consume; /* Japanese: how many kana it replaces; Chinese: how many letters */
};

static struct {
    char lang; /* 'k', 'j', 'z', or 0 when idle */
    int cho, jung, jong;
    char raw[64];        /* Japanese romaji / Chinese pinyin as typed */
    struct cand cands[MAX_CANDS];
    int ncands, sel;
    bool converting;     /* Japanese: Space was pressed, choosing */
    char *sink;          /* the `languages type` command collects the text here instead of sending it */
    size_t sink_len, sink_cap;
} ime = {.cho = -1, .jung = -1};

static void send_cp(uint32_t cp)
{
    if (ime.sink) {
        if (ime.sink_len + 4 < ime.sink_cap)
            ime.sink_len += (size_t)put_utf8(ime.sink + ime.sink_len, cp);
        ime.sink[ime.sink_len] = 0;
        return;
    }
    wm_key(KEY_TEXT_CHAR((int)cp, 0));
}

static void send_text(const char *s)
{
    while (*s)
        send_cp(utf8_next(&s));
}

static void panel_update(void);

/* Korean */

static const uint16_t cho_jamo[19] = {0x3131, 0x3132, 0x3134, 0x3137, 0x3138, 0x3139, 0x3141, 0x3142, 0x3143, 0x3145,
                                      0x3146, 0x3147, 0x3148, 0x3149, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e};
static const int8_t cho_of[30] = {0,  1,  -1, 2,  -1, -1, 3,  4,  5,  -1, -1, -1, -1, -1, -1,
                                  -1, 6,  7,  8,  -1, 9,  10, 11, 12, 13, 14, 15, 16, 17, 18};
static const int8_t jong_of[30] = {1,  2,  3,  4,  5,  6,  7,  -1, 8,  9,  10, 11, 12, 13, 14,
                                   15, 16, 17, -1, 18, 19, 20, 21, 22, -1, 23, 24, 25, 26, 27};
static const uint16_t jong_jamo[28] = {0,      0x3131, 0x3132, 0x3133, 0x3134, 0x3135, 0x3136, 0x3137, 0x3139, 0x313a,
                                       0x313b, 0x313c, 0x313d, 0x313e, 0x313f, 0x3140, 0x3141, 0x3142, 0x3144, 0x3145,
                                       0x3146, 0x3147, 0x3148, 0x314a, 0x314b, 0x314c, 0x314d, 0x314e};
/* compound finals: {first final, added consonant (jamo), result} */
static const uint16_t jong_pairs[][3] = {{1, 0x3145, 3},  {4, 0x3148, 5},  {4, 0x314e, 6},  {8, 0x3131, 9},
                                         {8, 0x3141, 10}, {8, 0x3142, 11}, {8, 0x3145, 12}, {8, 0x314c, 13},
                                         {8, 0x314d, 14}, {8, 0x314e, 15}, {17, 0x3145, 18}};
static const uint8_t jung_pairs[][3] = {{8, 0, 9}, {8, 1, 10}, {8, 20, 11}, {13, 4, 14},
                                        {13, 5, 15}, {13, 20, 16}, {18, 20, 19}};

static const char ko_keys[] = "qwertyuiopasdfghjklzxcvbnmQWERTOP";
static const uint16_t ko_jamo[] = {0x3142, 0x3148, 0x3137, 0x3131, 0x3145, 0x315b, 0x3155, 0x3151, 0x3150, 0x3154,
                                   0x3141, 0x3134, 0x3147, 0x3139, 0x314e, 0x3157, 0x3153, 0x314f, 0x3163, 0x314b,
                                   0x314c, 0x314a, 0x314d, 0x3160, 0x315c, 0x3161, 0x3143, 0x3149, 0x3138, 0x3132,
                                   0x3146, 0x3152, 0x3156};

static uint32_t ko_current(void)
{
    if (ime.cho >= 0 && ime.jung >= 0)
        return 0xac00 + (uint32_t)((ime.cho * 21 + ime.jung) * 28 + ime.jong);
    if (ime.cho >= 0)
        return cho_jamo[ime.cho];
    if (ime.jung >= 0)
        return 0x314f + (uint32_t)ime.jung;
    return 0;
}

static void ko_commit(void)
{
    uint32_t cp = ko_current();
    if (cp)
        send_cp(cp);
    ime.cho = ime.jung = -1;
    ime.jong = 0;
}

static void ko_jamo_typed(uint16_t j)
{
    if (j >= 0x314f) { /* a vowel */
        int v = j - 0x314f;
        if (ime.jung < 0) {
            ime.jung = v;
            return;
        }
        if (!ime.jong) {
            for (size_t i = 0; i < ARRAY_SIZE(jung_pairs); i++)
                if (jung_pairs[i][0] == ime.jung && jung_pairs[i][1] == v && ime.cho >= 0) {
                    ime.jung = jung_pairs[i][2];
                    return;
                }
            ko_commit();
            ime.jung = v;
            return;
        }
        /* the final consonant moves to the new syllable */
        int moved = -1;
        for (size_t i = 0; i < ARRAY_SIZE(jong_pairs); i++)
            if (jong_pairs[i][2] == ime.jong) {
                ime.jong = jong_pairs[i][0];
                moved = cho_of[jong_pairs[i][1] - 0x3131];
            }
        if (moved < 0) {
            moved = cho_of[jong_jamo[ime.jong] - 0x3131];
            ime.jong = 0;
        }
        ko_commit();
        ime.cho = moved;
        ime.jung = v;
        return;
    }
    int idx = j - 0x3131;
    if (ime.jung < 0) {
        if (ime.cho >= 0)
            ko_commit();
        ime.cho = cho_of[idx];
        return;
    }
    if (ime.cho >= 0 && !ime.jong && jong_of[idx] >= 0) {
        ime.jong = jong_of[idx];
        return;
    }
    if (ime.cho >= 0 && ime.jong)
        for (size_t i = 0; i < ARRAY_SIZE(jong_pairs); i++)
            if (jong_pairs[i][0] == ime.jong && jong_pairs[i][1] == j) {
                ime.jong = jong_pairs[i][2];
                return;
            }
    ko_commit();
    ime.cho = cho_of[idx];
}

static bool ko_back(void)
{
    if (ime.jong) {
        for (size_t i = 0; i < ARRAY_SIZE(jong_pairs); i++)
            if (jong_pairs[i][2] == ime.jong) {
                ime.jong = jong_pairs[i][0];
                return true;
            }
        ime.jong = 0;
    } else if (ime.jung >= 0) {
        for (size_t i = 0; i < ARRAY_SIZE(jung_pairs); i++)
            if (jung_pairs[i][2] == ime.jung) {
                ime.jung = jung_pairs[i][0];
                return true;
            }
        ime.jung = -1;
    } else if (ime.cho >= 0) {
        ime.cho = -1;
    } else {
        return false;
    }
    return true;
}

/* Japanese and Chinese */

static void add_cand(const char *text, int consume)
{
    if (ime.ncands >= MAX_CANDS || !text[0])
        return;
    for (int i = 0; i < ime.ncands; i++)
        if (!strcmp(ime.cands[i].text, text))
            return;
    str_copy(ime.cands[ime.ncands].text, text, sizeof(ime.cands[0].text));
    ime.cands[ime.ncands++].consume = consume;
}

static void add_chars(const char *chars, int consume, int max)
{
    for (int n = 0; chars && *chars && n < max; n++) {
        const char *start = chars;
        utf8_next(&chars);
        char one[8];
        memcpy(one, start, (size_t)(chars - start));
        one[chars - start] = 0;
        add_cand(one, consume);
    }
}

static int ja_kana(uint32_t *kana, int max)
{
    return romaji_to_kana(ime.raw, kana, max, NULL, true);
}

static void kana_text(const uint32_t *kana, int n, bool katakana, char *out, size_t cap)
{
    size_t len = 0;
    for (int i = 0; i < n && len + 4 < cap; i++) {
        uint32_t cp = kana[i];
        if (katakana && cp >= 0x3041 && cp <= 0x3096)
            cp += 0x60;
        len += (size_t)put_utf8(out + len, cp);
    }
    out[len] = 0;
}

static void ja_candidates(void)
{
    ime.ncands = 0;
    ime.sel = 0;
    uint32_t kana[64];
    int n = ja_kana(kana, 64);
    if (!n)
        return;
    char romaji[96], text[160];
    for (int len = n; len > 0 && ime.ncands < 40; len--) {
        if (!kana_to_romaji(kana, len, romaji, sizeof(romaji)))
            continue;
        for (size_t i = 0; i < ARRAY_SIZE(ja_words); i++) {
            const char *eq = strchr(ja_words[i], '=');
            if ((size_t)(eq - ja_words[i]) == strlen(romaji) && !strncmp(ja_words[i], romaji, strlen(romaji)))
                add_cand(eq + 1, len);
        }
        char upper[96];
        str_copy(upper, romaji, sizeof(upper));
        add_chars(lookup('j', upper), len, 30);
        if (len == n) {
            kana_text(kana, n, true, text, sizeof(text));
            add_cand(text, n);
            kana_text(kana, n, false, text, sizeof(text));
            add_cand(text, n);
        }
    }
}

/* the Pinyin syllable at the start of s: the longest the readings know */
static int zh_syllable(const char *s)
{
    char key[8];
    for (int len = MIN((int)strlen(s), 6); len > 0; len--) {
        memcpy(key, s, (size_t)len);
        key[len] = 0;
        if (lookup('z', key))
            return len;
    }
    return 0;
}

static void zh_candidates(void)
{
    ime.ncands = 0;
    ime.sel = 0;
    int total = (int)strlen(ime.raw);
    if (!total)
        return;
    /* words: the longest typed prefix that is a word comes first */
    for (int len = total; len > 1; len--)
        for (size_t i = 0; i < ARRAY_SIZE(zh_words); i++) {
            const char *eq = strchr(zh_words[i], '=');
            if (eq - zh_words[i] == len && !strncmp(zh_words[i], ime.raw, (size_t)len))
                add_cand(eq + 1, len);
        }
    int syl = zh_syllable(ime.raw);
    if (syl) {
        char key[8];
        memcpy(key, ime.raw, (size_t)syl);
        key[syl] = 0;
        add_chars(lookup('z', key), syl, MAX_CANDS);
    }
}

static void reset(void)
{
    ime.lang = 0;
    ime.cho = ime.jung = -1;
    ime.jong = 0;
    ime.raw[0] = 0;
    ime.ncands = 0;
    ime.sel = 0;
    ime.converting = false;
}

static bool composing(void)
{
    return ime.cho >= 0 || ime.jung >= 0 || ime.raw[0];
}

/* commits what is being composed as it stands */
static void flush(void)
{
    if (ime.lang == 'k') {
        ko_commit();
    } else if (ime.lang == 'j' && ime.raw[0]) {
        if (ime.converting && ime.ncands) {
            send_text(ime.cands[ime.sel].text);
        } else {
            uint32_t kana[64];
            char text[200];
            kana_text(kana, ja_kana(kana, 64), false, text, sizeof(text));
            send_text(text);
        }
    } else if (ime.lang == 'z' && ime.raw[0]) {
        send_text(ime.raw);
    }
    char lang = ime.lang;
    reset();
    ime.lang = lang;
}

static void pick(int index)
{
    if (index < 0 || index >= ime.ncands)
        return;
    struct cand c = ime.cands[index];
    send_text(c.text);
    if (ime.lang == 'z') {
        memmove(ime.raw, ime.raw + c.consume, strlen(ime.raw + c.consume) + 1);
        zh_candidates();
        return;
    }
    /* Japanese: drop the kana it stood for and keep composing the rest */
    uint32_t kana[64];
    int n = ja_kana(kana, 64);
    char rest[64] = "";
    if (c.consume < n && !kana_to_romaji(kana + c.consume, n - c.consume, rest, sizeof(rest)))
        rest[0] = 0;
    str_copy(ime.raw, rest, sizeof(ime.raw));
    ime.converting = false;
    ime.ncands = 0;
    if (ime.raw[0]) {
        ja_candidates();
        ime.converting = ime.ncands > 0;
    }
}

static uint32_t punctuation(char lang, int c)
{
    static const char from[] = ",.?!;:[]\\";
    static const uint16_t zh[] = {0xff0c, 0x3002, 0xff1f, 0xff01, 0xff1b, 0xff1a, 0x3010, 0x3011, 0x3001};
    static const uint16_t ja[] = {0x3001, 0x3002, 0xff1f, 0xff01, 0xff1b, 0xff1a, 0x300c, 0x300d, 0xffe5};
    const char *at = c > 0 && c < 0x80 ? strchr(from, c) : NULL;
    if (!at || lang == 'k')
        return 0;
    return lang == 'z' ? zh[at - from] : ja[at - from];
}

/* The hook: every key while a CJK input source is active. 0 keeps the key. */
static int ime_key(const char *layout, int key)
{
    char lang = layout[0] == 'k' ? 'k' : layout[0] == 'j' ? 'j' : 'z';
    if (ime.lang != lang) {
        flush();
        reset();
        ime.lang = lang;
    }
    int base = KEY_BASE(key);
    if (key & (KEY_MOD_CTRL | KEY_MOD_ALT | KEY_MOD_SUPER)) {
        flush();
        panel_update();
        return key;
    }
    int result = 0;
    if (lang == 'k') {
        const char *at = base < 0x80 && base > 0x20 ? strchr(ko_keys, base) : NULL;
        if (at)
            ko_jamo_typed(ko_jamo[at - ko_keys]);
        else if (base >= 'A' && base <= 'Z' && (at = strchr(ko_keys, base + 32)))
            ko_jamo_typed(ko_jamo[at - ko_keys]);
        else if (base == KEY_BACKSPACE && composing())
            ko_back();
        else {
            flush();
            result = key;
        }
    } else if (base >= 'a' && base <= 'z' && !(lang == 'z' && ime.ncands && !ime.raw[0])) {
        if (lang == 'j' && ime.converting) {
            pick(ime.sel);
            flush();
        }
        size_t len = strlen(ime.raw);
        if (len + 1 < sizeof(ime.raw)) {
            ime.raw[len] = (char)base;
            ime.raw[len + 1] = 0;
        }
        if (lang == 'z')
            zh_candidates();
    } else if (composing() && lang == 'j' && base == '-') {
        size_t len = strlen(ime.raw);
        if (len + 1 < sizeof(ime.raw)) {
            ime.raw[len] = '-';
            ime.raw[len + 1] = 0;
        }
    } else if (composing() && base == KEY_BACKSPACE) {
        if (ime.converting) {
            ime.converting = false;
            ime.ncands = 0;
        } else {
            ime.raw[strlen(ime.raw) - 1] = 0;
            if (lang == 'z')
                zh_candidates();
        }
    } else if (composing() && base == KEY_ESC) {
        char l = ime.lang;
        reset();
        ime.lang = l;
    } else if (composing() && base == ' ') {
        if (lang == 'j' && !ime.converting) {
            ja_candidates();
            ime.converting = ime.ncands > 0;
        } else if (lang == 'j') {
            ime.sel = (ime.sel + 1) % MAX(ime.ncands, 1);
        } else if (ime.ncands) {
            pick(ime.sel);
        } else {
            flush();
        }
    } else if (composing() && base >= '1' && base <= '9' && (ime.converting || lang == 'z') &&
               ime.ncands > (ime.sel / PAGE) * PAGE + base - '1') {
        pick((ime.sel / PAGE) * PAGE + base - '1');
    } else if (composing() && (base == KEY_RIGHT || base == KEY_DOWN) && ime.ncands) {
        ime.sel = MIN(ime.sel + (base == KEY_DOWN ? PAGE : 1), ime.ncands - 1);
    } else if (composing() && (base == KEY_LEFT || base == KEY_UP) && ime.ncands) {
        ime.sel = MAX(ime.sel - (base == KEY_UP ? PAGE : 1), 0);
    } else if (composing() && (base == '\n' || base == '\r')) {
        flush();
    } else {
        flush();
        uint32_t p = punctuation(lang, base);
        if (p)
            send_cp(p);
        else
            result = key;
    }
    panel_update();
    return result;
}

/* ---- the panel ---- */

static struct window *panel;
static struct window *panel_for;

static void preedit(char *out, size_t cap)
{
    out[0] = 0;
    if (ime.lang == 'k') {
        uint32_t cp = ko_current();
        if (cp)
            out[put_utf8(out, cp)] = 0;
    } else if (ime.lang == 'j') {
        uint32_t kana[64];
        const char *rest;
        int n = romaji_to_kana(ime.raw, kana, 64, &rest, false);
        kana_text(kana, n, false, out, cap);
        str_append(out, rest, cap);
    } else {
        str_copy(out, ime.raw, cap);
    }
}

static void cand_line(char *out, size_t cap, int *sel_x, int *sel_w)
{
    out[0] = 0;
    *sel_x = *sel_w = -1;
    int first = ime.sel / PAGE * PAGE;
    for (int i = first; i < ime.ncands && i < first + PAGE; i++) {
        char item[56];
        ksnprintf(item, sizeof(item), "%d %s   ", i - first + 1, ime.cands[i].text);
        if (i == ime.sel) {
            *sel_x = gfx_text_width(&font_sans, out);
            *sel_w = gfx_text_width(&font_sans, item) - gfx_text_width(&font_sans, "   ");
        }
        str_append(out, item, cap);
    }
}

static void ime_paint(struct window *w, struct canvas *c)
{
    UNUSED(w);
    struct rect r = {0, 0, c->w, c->h};
    gfx_clear(c, r, 0);
    gfx_round(c, r, 12, ALPHA(style.window, 250));
    gfx_round_frame(c, r, 12, style.separator);
    char text[200];
    preedit(text, sizeof(text));
    gfx_text(c, 14, 8, &font_bold, text, style.text);
    int tx = 14 + gfx_text_width(&font_bold, text);
    gfx_fill(c, (struct rect){14, 27, tx - 14, 1}, style.accent);
    if (!glyph_at && !glyph_count)
        gfx_text(c, tx + 12, 9, &font_small, "language pack: languages install", style.tertiary);
    if (ime.ncands) {
        char line[400];
        int sx, sw;
        cand_line(line, sizeof(line), &sx, &sw);
        if (sx >= 0)
            gfx_round(c, (struct rect){14 + sx - 4, 34, sw + 8, 22}, 6, style.selection);
        gfx_text(c, 14, 36, &font_sans, line, style.text);
        if (ime.ncands > PAGE) {
            char more[24];
            ksnprintf(more, sizeof(more), "%d/%d", ime.sel / PAGE + 1, (ime.ncands + PAGE - 1) / PAGE);
            gfx_text_box(c, (struct rect){c->w - 54, 36, 44, 18}, &font_small, more, style.tertiary, ALIGN_RIGHT);
        }
    }
}

static void ime_event(struct window *w, struct gui_event *e)
{
    UNUSED(w);
    if (e->type == EV_MOUSE_DOWN)
        e->handled = true;
}

static void ime_destroy(struct window *w)
{
    if (panel == w)
        panel = NULL;
}

static const struct window_ops ime_ops = {ime_paint, ime_event, NULL, ime_destroy};

static void panel_update(void)
{
    if (ime.sink)
        return;
    if (!composing()) {
        if (panel && wm_window_alive(panel))
            wm_destroy(panel);
        panel = NULL;
        return;
    }
    char text[200], line[400];
    int sx, sw;
    preedit(text, sizeof(text));
    cand_line(line, sizeof(line), &sx, &sw);
    int width = MAX(gfx_text_width(&font_bold, text) + (glyph_count ? 0 : 230), ime.ncands ? gfx_text_width(&font_sans, line) + 50 : 0);
    width = CLAMP(width + 28, 120, 900);
    int height = ime.ncands ? 64 : 36;
    struct window *focus = wm_focused();
    struct rect s = wm_screen_rect(0), at = focus ? wm_client_rect(focus) : s;
    int x = CLAMP(at.x + 24, s.x + 8, s.x + s.w - width - 8);
    int y = CLAMP(at.y + at.h - height - 24, s.y + 30, s.y + s.h - height - 8);
    if (panel && (panel->surface.w != width || panel->surface.h != height || panel_for != focus)) {
        wm_destroy(panel);
        panel = NULL;
    }
    if (!panel) {
        panel = wm_popup(x, y, width, height, &ime_ops, NULL, NULL);
        if (!panel)
            return;
        panel->radius = 12;
        panel->flags |= WIN_PASSIVE;
        panel_for = focus;
    }
    wm_invalidate(panel);
}

/* ---- the language pack ---- */

struct item {
    char key[12];
    uint32_t cp;
    int score;
    char lang;
};

struct builder {
    struct item *items;
    int n, cap;
};

static void add_item(struct builder *b, char lang, const char *key, uint32_t cp, int score)
{
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 4096;
        struct item *grown = krealloc(b->items, sizeof(struct item) * (size_t)cap);
        if (!grown)
            return;
        b->items = grown;
        b->cap = cap;
    }
    struct item *it = &b->items[b->n++];
    str_copy(it->key, key, sizeof(it->key));
    it->cp = cp;
    it->score = score;
    it->lang = lang;
}

/* "zhōng" -> "zhong", "lǜ" -> "lv" */
static void plain_pinyin(const char *in, char *out, size_t cap)
{
    size_t n = 0;
    while (*in && n + 1 < cap) {
        uint32_t c = utf8_next(&in);
        char a = 0;
        if (c < 0x80 && is_alpha((int)c))
            a = (char)to_lower((int)c);
        else if (c == 0xfc || (c >= 0x1d6 && c <= 0x1dc))
            a = 'v';
        else if ((c >= 0xe0 && c <= 0xe5) || c == 0x101 || c == 0x1ce)
            a = 'a';
        else if ((c >= 0xe8 && c <= 0xeb) || c == 0x113 || c == 0x11b)
            a = 'e';
        else if ((c >= 0xec && c <= 0xef) || c == 0x12b || c == 0x1d0)
            a = 'i';
        else if ((c >= 0xf2 && c <= 0xf6) || c == 0x14d || c == 0x1d2)
            a = 'o';
        else if ((c >= 0xf9 && c <= 0xfb) || c == 0x16b || c == 0x1d4)
            a = 'u';
        else if (c == 0x144 || c == 0x148 || c == 0x1f9)
            a = 'n';
        else if (c == 0x1e3f)
            a = 'm';
        if (a)
            out[n++] = a;
    }
    out[n] = 0;
}

static int item_cmp(const struct item *a, const struct item *b)
{
    if (a->lang != b->lang)
        return a->lang < b->lang ? -1 : 1;
    int c = strcmp(a->key, b->key);
    if (c)
        return c;
    return b->score - a->score;
}

static void sort_items(struct item *v, int n)
{
    /* heap sort: no recursion, no extra memory */
    for (int start = n / 2 - 1; start >= 0; start--)
        for (int root = start;;) {
            int child = root * 2 + 1;
            if (child >= n)
                break;
            if (child + 1 < n && item_cmp(&v[child], &v[child + 1]) < 0)
                child++;
            if (item_cmp(&v[root], &v[child]) >= 0)
                break;
            struct item t = v[root];
            v[root] = v[child];
            v[child] = t;
            root = child;
        }
    for (int end = n - 1; end > 0; end--) {
        struct item t = v[0];
        v[0] = v[end];
        v[end] = t;
        for (int root = 0;;) {
            int child = root * 2 + 1;
            if (child >= end)
                break;
            if (child + 1 < end && item_cmp(&v[child], &v[child + 1]) < 0)
                child++;
            if (item_cmp(&v[root], &v[child]) >= 0)
                break;
            t = v[root];
            v[root] = v[child];
            v[child] = t;
            root = child;
        }
    }
}

struct unihan {
    const uint8_t *zip;
    size_t len;
    char *readings, *other;
    size_t readings_len, other_len;
};

static int find_files(const struct zip_entry *e, void *ctx)
{
    struct unihan *u = ctx;
    uint8_t *out = NULL;
    char name[64];
    size_t n = MIN(e->name_len, sizeof(name) - 1);
    memcpy(name, e->name + e->name_len - n, n);
    name[n] = 0;
    if (str_ends(name, "Unihan_Readings.txt") && !u->readings && zip_extract(u->zip, u->len, e, &out) == 0) {
        u->readings = (char *)out;
        u->readings_len = e->usize;
    } else if (str_ends(name, "Unihan_OtherMappings.txt") && !u->other && zip_extract(u->zip, u->len, e, &out) == 0) {
        u->other = (char *)out;
        u->other_len = e->usize;
    }
    return 0;
}

/* JIS X 0208 kanji: level 1 (the common ones) scores higher */
static uint8_t *jis_levels(const char *text, size_t len)
{
    uint8_t *level = kzalloc(0x10000);
    if (!level)
        return NULL;
    for (const char *line = text; text && line < text + len;) {
        const char *end = memchr(line, '\n', (size_t)(text + len - line));
        if (!end)
            end = text + len;
        if (line[0] == 'U' && line[1] == '+') {
            const char *p = line + 2;
            uint32_t cp = (uint32_t)str_to_long(p, &p, 16);
            if (cp < 0x10000 && !strncmp(p, "\tkJis0\t", 7)) {
                long v = str_to_long(p + 7, NULL, 10);
                level[cp] = v / 100 >= 16 && v / 100 <= 47 ? 2 : 1;
            }
        }
        line = end + 1;
    }
    return level;
}

/* Reads Unihan_Readings: Chinese characters with a Pinyin frequency or in the standard list of 2013, and the
   JIS kanji with their Japanese readings. */
static int build_dict(struct unihan *u, struct builder *b)
{
    uint8_t *jis = jis_levels(u->other, u->other_len);
    if (!jis)
        return -1;
    char field[24], value[512], key[24];
    for (const char *line = u->readings; line < u->readings + u->readings_len;) {
        const char *end = memchr(line, '\n', (size_t)(u->readings + u->readings_len - line));
        if (!end)
            end = u->readings + u->readings_len;
        if (line[0] == 'U' && line[1] == '+') {
            const char *p = line + 2;
            uint32_t cp = (uint32_t)str_to_long(p, &p, 16);
            if (*p == '\t' && cp < 0x10000) {
                const char *f = ++p;
                while (p < end && *p != '\t')
                    p++;
                size_t fl = MIN((size_t)(p - f), sizeof(field) - 1);
                memcpy(field, f, fl);
                field[fl] = 0;
                size_t vl = MIN((size_t)(end - p - 1), sizeof(value) - 1);
                memcpy(value, p + 1, vl);
                value[vl] = 0;
                bool pinlu = !strcmp(field, "kHanyuPinlu"), tghz = !strcmp(field, "kTGHZ2013");
                bool kun = !strcmp(field, "kJapaneseKun"), on = !strcmp(field, "kJapaneseOn");
                for (char *w = value; (pinlu || tghz || ((kun || on) && jis[cp])) && *w;) {
                    char *sp = strchr(w, ' ');
                    if (sp)
                        *sp = 0;
                    if (pinlu) { /* zhōng(4875) */
                        char *paren = strchr(w, '(');
                        long count = paren ? str_to_long(paren + 1, NULL, 10) : 0;
                        if (paren)
                            *paren = 0;
                        plain_pinyin(w, key, sizeof(key));
                        if (key[0])
                            add_item(b, 'z', key, cp, 1000 + (int)MIN(count, 1000000));
                    } else if (tghz) { /* 480.020:zhōng */
                        char *colon = strchr(w, ':');
                        plain_pinyin(colon ? colon + 1 : w, key, sizeof(key));
                        if (key[0])
                            add_item(b, 'z', key, cp, 10);
                    } else {
                        str_copy(key, w, sizeof(key));
                        str_lower(key);
                        add_item(b, 'j', key, cp, jis[cp] * 10 + (kun ? 1 : 0));
                    }
                    if (!sp)
                        break;
                    w = sp + 1;
                }
            }
        }
        line = end + 1;
    }
    kfree(jis);
    return 0;
}

static int write_dict(struct builder *b)
{
    sort_items(b->items, b->n);
    size_t cap = (size_t)b->n * 4 + 1024 + (size_t)b->n * 4;
    char *out = kmalloc(cap);
    if (!out)
        return -1;
    size_t len = 0;
    for (int i = 0; i < b->n;) {
        int j = i;
        len += (size_t)ksnprintf(out + len, cap - len, "%c\t%s\t", b->items[i].lang, b->items[i].key);
        for (; j < b->n && b->items[j].lang == b->items[i].lang && !strcmp(b->items[j].key, b->items[i].key); j++) {
            bool dup = false;
            for (int k = i; k < j && !dup; k++)
                dup = b->items[k].cp == b->items[j].cp;
            if (!dup && len + 8 < cap)
                len += (size_t)put_utf8(out + len, b->items[j].cp);
        }
        if (len + 2 < cap)
            out[len++] = '\n';
        i = j;
    }
    int err = vfs_write_all(DICT_FILE, out, len);
    kfree(out);
    return err;
}

static bool cancelled(void)
{
    return false;
}

static int download(const char *url, const char *what, char **body, size_t *len, char *error, size_t cap)
{
    if (url[0] == '/') { /* a folder on this computer */
        if (vfs_read_all(url, body, len) == 0)
            return 0;
        ksnprintf(error, cap, "can't read %s", url);
        return -1;
    }
    struct http_request req = {.url = url, .max_size = 24u << 20, .flags = HTTP_NO_COOKIES, .cancelled = cancelled,
                               .progress = ime.sink ? NULL : progress_update};
    struct http_response resp;
    if (!ime.sink)
        progress_open("Language Pack", what);
    int err = http_fetch(&req, &resp);
    if (err) {
        ksnprintf(error, cap, "can't download %s (%s)", url, tls_strerror(err));
        return -1;
    }
    if (resp.status != 200 || !resp.body) {
        ksnprintf(error, cap, "%s answered %d", url, resp.status);
        http_free(&resp);
        return -1;
    }
    *body = resp.body;
    *len = resp.length;
    resp.body = NULL;
    http_free(&resp);
    return 0;
}

/* base: the web address or folder (ending in /) where unifont.hex.gz and Unihan.zip are, or NULL for their home
   sites */
static int install_pack(const char *base, char *error, size_t cap, int *chars)
{
    char url[512];
    char *font = NULL, *zip = NULL;
    size_t font_len = 0, zip_len = 0;
    int rc = -1;
    vfs_mkdirs(PACK_DIR);
    if (base)
        ksnprintf(url, sizeof(url), "%sunifont.hex.gz", base);
    if (download(base ? url : FONT_URL, "Downloading the CJK font (GNU Unifont)…", &font, &font_len, error, cap))
        goto out;
    if (vfs_write_all(FONT_FILE, font, font_len)) {
        ksnprintf(error, cap, "can't write %s", FONT_FILE);
        goto out;
    }
    if (base)
        ksnprintf(url, sizeof(url), "%sUnihan.zip", base);
    if (download(base ? url : UNIHAN_URL, "Downloading the character readings (Unihan)…", &zip, &zip_len, error, cap))
        goto out;
    if (!ime.sink)
        progress_open("Language Pack", "Building the dictionary…");
    struct unihan u = {(const uint8_t *)zip, zip_len, NULL, NULL, 0, 0};
    zip_each(u.zip, u.len, find_files, &u);
    if (!u.readings) {
        ksnprintf(error, cap, "Unihan.zip has no Unihan_Readings.txt");
        kfree(u.other);
        goto out;
    }
    struct builder b = {0};
    if (build_dict(&u, &b) || write_dict(&b)) {
        ksnprintf(error, cap, "out of memory building the dictionary");
    } else {
        *chars = b.n;
        rc = 0;
    }
    kfree(b.items);
    kfree(u.readings);
    kfree(u.other);
    /* use the new files */
    kfree(glyph_at);
    kfree(glyph_data);
    glyph_at = NULL;
    glyph_data = NULL;
    glyph_count = 0;
    glyphs_tried = false;
    kfree(readings);
    kfree(dict_text);
    readings = NULL;
    dict_text = NULL;
    nreadings = 0;
    dict_tried = false;
out:
    if (!ime.sink)
        progress_close();
    kfree(font);
    kfree(zip);
    return rc;
}

static void offer_picked(struct window *owner, int index)
{
    UNUSED(owner);
    if (index != 0)
        return;
    char error[160];
    int chars = 0;
    if (install_pack(NULL, error, sizeof(error), &chars))
        gui_notify("Language Pack", error);
    else
        gui_notify("Language Pack", "Chinese, Japanese and Korean are ready to type and read");
    wm_damage_all();
}

static void tick(void)
{
    static bool hooked;
    static int last_layout = -1;
    if (!hooked) {
        keymap_ime = ime_key;
        gfx_wide_glyph = wide_glyph;
        hooked = true;
    }
    int now = keymap_active();
    if (now != last_layout) {
        const char *name = keymap_layout_name(now, NULL);
        bool cjk = name && (!strcmp(name, "ko") || !strcmp(name, "ja") || !strcmp(name, "zh"));
        if (composing())
            flush();
        if (cjk && last_layout >= 0 && !pack_installed()) {
            static const char *const buttons[] = {"Download", "Not Now"};
            ui_alert(wm_focused(), "Chinese, Japanese and Korean",
                     "Showing these characters and choosing them while typing needs the free language pack "
                     "(about 9 MB from unifoundry.com and unicode.org).",
                     ICON_SETTINGS, buttons, 2, offer_picked);
        }
        last_layout = now;
        panel_update();
    }
    if (panel && panel_for != wm_focused() && composing()) {
        flush();
        panel_update();
    }
}

/* ---- the command ---- */

/* the text, and its code points for a console without the glyphs */
static void show_text(struct shell *sh, const char *label, const char *s)
{
    sh_printf(sh, "%s: %s (", label, s);
    for (const char *p = s; *p;) {
        uint32_t cp = utf8_next(&p);
        sh_printf(sh, cp < 0x80 ? "%c" : "U+%04X%s", cp, *p && (unsigned char)*p >= 0x80 ? " " : "");
    }
    sh_printf(sh, ")\n");
}

COMMAND(languages, "ime", "system",
        "languages [install [URL|FOLDER/] | type ko|ja|zh KEYS...]",
        "Chinese, Japanese and Korean: the input methods and the language pack with their characters")
{
    if (argc == 1) {
        int order[64], n = keymap_order(order, 64);
        sh_printf(sh, "input sources:");
        for (int i = 0; i < n; i++) {
            const char *title;
            keymap_layout_name(order[i], &title);
            sh_printf(sh, "%s %s", i ? "," : "", title);
        }
        sh_printf(sh, "\nChinese, Japanese and Korean pack: %s\n",
                  pack_installed() ? "installed" : "not installed (languages install)");
        if (pack_installed()) {
            if (!glyphs_tried)
                load_glyphs();
            if (!dict_tried)
                load_dict();
            sh_printf(sh, "  %d characters to show, %d readings to type with\n", glyph_count, nreadings);
        }
        sh_printf(sh, "add one: config keyboard us,ko (or ja, zh); Win+Space switches\n");
        return 0;
    }
    if (!strcmp(argv[1], "install")) {
        char error[200], buf[16];
        int chars = 0;
        ime.sink = buf; /* no progress card from the shell */
        ime.sink_cap = 0;
        int rc = install_pack(argc > 2 ? argv[2] : NULL, error, sizeof(error), &chars);
        ime.sink = NULL;
        if (rc)
            return sh_error(sh, "languages", "%s", error);
        if (!glyphs_tried)
            load_glyphs();
        sh_printf(sh, "installed: %d characters to show, %d readings to type with\n", glyph_count, chars);
        return 0;
    }
    if (!strcmp(argv[1], "type") && argc > 3) {
        const char *lang = argv[2];
        if (strcmp(lang, "ko") && strcmp(lang, "ja") && strcmp(lang, "zh"))
            return command_usage(sh, command_find("languages"));
        char out[512];
        ime.sink = out;
        ime.sink_len = 0;
        ime.sink_cap = sizeof(out);
        out[0] = 0;
        reset();
        for (int i = 3; i < argc; i++) {
            if (i > 3)
                ime_key(lang, ' ');
            for (const char *p = argv[i]; *p; p++)
                ime_key(lang, *p == '#' ? '\n' : *p == '^' ? KEY_BACKSPACE : *p == '_' ? ' ' : *p);
        }
        char pre[200];
        preedit(pre, sizeof(pre));
        if (pre[0])
            show_text(sh, "composing", pre);
        if (ime.ncands) {
            sh_printf(sh, "choices:");
            for (int i = 0; i < ime.ncands && i < PAGE; i++) {
                sh_printf(sh, " %d", i + 1);
                for (const char *p = ime.cands[i].text; *p;)
                    sh_printf(sh, " U+%04X", utf8_next(&p));
            }
            sh_printf(sh, "\n");
        }
        flush();
        show_text(sh, "typed", out);
        ime.sink = NULL;
        reset();
        return 0;
    }
    return command_usage(sh, command_find("languages"));
}

static const struct command *const commands[] = {&cmddef_languages};
static const struct module_ext ext = {commands, ARRAY_SIZE(commands), NULL, NULL, NULL, NULL, NULL, NULL, NULL};
GUI_APP_FULL(NULL, tick, &ext);
