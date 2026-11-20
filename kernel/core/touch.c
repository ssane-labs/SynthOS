/* The gestures of touchpads and touch screens, from frames of finger contacts:
   one finger moves the pointer (on a touch screen it points and presses where it touches), a short touch without
   movement clicks (one finger: left, two: right, three: middle), two fingers scroll, three fingers swiped left,
   right, up or down give KEY_SWIPE_* (the desktop switches spaces and opens Mission Control and Launchpad), and the
   pad's own button with two fingers on it is a right click. */
#ifdef __x86_64__

#include <synth/command.h>
#include <synth/fmt.h>
#include <synth/keys.h>
#include <synth/log.h>
#include <synth/string.h>
#include <synth/touch.h>

#define TAP_MS 220
#define PAD_MICKEYS 1000 /* the pad's width in pointer steps */
#define SCROLL_STEPS 24  /* wheel steps over the pad's height */
#define LEFT 1u
#define RIGHT 2u
#define MIDDLE 4u

static unsigned taps, scroll_steps, swipes;
static const char *names[4];
static bool screens[4];
static int name_count;

void touch_add(const char *name, bool direct)
{
    if (name_count < (int)ARRAY_SIZE(names)) {
        screens[name_count] = direct;
        names[name_count++] = name;
    }
}

static const struct touch_contact *find(const struct touch_contact *list, int n, int id)
{
    for (int i = 0; i < n; i++)
        if (list[i].id == id)
            return &list[i];
    return NULL;
}

static int64_t abs64(int64_t v)
{
    return v < 0 ? -v : v;
}

static void begin(struct touch_pad *p, uint64_t now)
{
    p->began = now;
    p->most = 0;
    p->travel = 0;
    p->acc_x = p->acc_y = p->scroll = p->swipe_x = p->swipe_y = 0;
    p->swiped = p->clicked = p->moved = false;
}

static void swipe(struct touch_pad *p)
{
    int64_t ax = abs64(p->swipe_x), ay = abs64(p->swipe_y);
    if (ax * 6 < p->max_x && ay * 6 < p->max_y)
        return;
    p->swiped = true;
    swipes++;
    static const char *const dirs[] = {"left", "right", "up", "down"};
    int dir = ax * p->max_y >= ay * p->max_x ? (p->swipe_x < 0 ? 0 : 1) : (p->swipe_y < 0 ? 2 : 3);
    static const int keys[] = {KEY_SWIPE_LEFT, KEY_SWIPE_RIGHT, KEY_SWIPE_UP, KEY_SWIPE_DOWN};
    klog("touch: three-finger swipe %s", dirs[dir]);
    if (p->out.key)
        p->out.key(keys[dir]);
}

static void buttons(struct touch_pad *p, unsigned b, const struct touch_frame *f)
{
    if (b == p->sent)
        return;
    p->sent = b;
    /* a touch screen presses and lets go where the finger is (or was, when it has just been lifted) */
    const struct touch_contact *at = f->count ? &f->c[0] : p->last_count ? &p->last[0] : NULL;
    if (p->direct && p->out.absolute && at)
        p->out.absolute((int)((int64_t)at->x * 65535 / p->max_x), (int)((int64_t)at->y * 65535 / p->max_y), b);
    else if (p->out.pointer)
        p->out.pointer(0, 0, 0, b);
}

static void tap(struct touch_pad *p)
{
    unsigned b = p->most == 1 ? LEFT : p->most == 2 ? RIGHT : MIDDLE;
    taps++;
    klog("touch: %d-finger tap", p->most);
    if (p->direct && p->most == 1) /* the press went with the touch */
        return;
    if (p->out.pointer) {
        p->out.pointer(0, 0, 0, b);
        p->out.pointer(0, 0, 0, 0);
    }
}

void touch_frame(struct touch_pad *p, const struct touch_frame *f, uint64_t now)
{
    if (p->max_x <= 0 || p->max_y <= 0)
        return;
    int n = CLAMP(f->count, 0, TOUCH_MAX);
    if (n && !p->fingers)
        begin(p, now);
    p->most = MAX(p->most, n);
    /* the movement of the fingers that were down in the last frame too; none when fingers came or went */
    int64_t dx = 0, dy = 0;
    int same = 0;
    for (int i = 0; n == p->last_count && i < n; i++) {
        const struct touch_contact *was = find(p->last, p->last_count, f->c[i].id);
        if (was) {
            dx += f->c[i].x - was->x;
            dy += f->c[i].y - was->y;
            same++;
        }
    }
    if (same) {
        dx /= same;
        dy /= same;
    }
    p->travel += abs64(dx) * 1000 / p->max_x + abs64(dy) * 1000 / p->max_y;
    unsigned b = f->buttons & 1 ? (n >= 2 ? RIGHT : LEFT) : 0;
    if (b)
        p->clicked = true;
    if (n == 1 && p->direct && p->out.absolute && p->most == 1) {
        int x = (int)((int64_t)CLAMP(f->c[0].x, 0, p->max_x) * 65535 / p->max_x);
        int y = (int)((int64_t)CLAMP(f->c[0].y, 0, p->max_y) * 65535 / p->max_y);
        p->sent = LEFT;
        p->out.absolute(x, y, LEFT);
    } else if (n == 1 && !p->direct) {
        p->acc_x += dx * PAD_MICKEYS;
        p->acc_y += dy * PAD_MICKEYS;
        int mx = (int)(p->acc_x / p->max_x), my = (int)(p->acc_y / p->max_x);
        p->acc_x -= (int64_t)mx * p->max_x;
        p->acc_y -= (int64_t)my * p->max_x;
        if ((mx || my) && p->out.pointer)
            p->out.pointer(mx, my, 0, p->sent);
    } else if (n == 2) {
        if (p->direct && p->sent == LEFT)
            buttons(p, 0, f);
        p->scroll += dy * SCROLL_STEPS;
        int steps = (int)(p->scroll / p->max_y);
        p->scroll -= (int64_t)steps * p->max_y;
        if (steps && p->out.pointer) {
            scroll_steps += (unsigned)(steps < 0 ? -steps : steps);
            p->out.pointer(0, 0, steps, p->sent);
        }
    } else if (n >= 3 && !p->swiped) {
        if (p->direct && p->sent == LEFT)
            buttons(p, 0, f);
        p->swipe_x += dx;
        p->swipe_y += dy;
        swipe(p);
    }
    if (!p->direct || n != 1)
        buttons(p, b, f);
    if (!n && p->fingers) {
        if (p->direct && p->sent)
            buttons(p, 0, f);
        if (!p->clicked && !p->swiped && now - p->began <= TAP_MS && p->travel < 40)
            tap(p);
    }
    p->last_count = n;
    memcpy(p->last, f->c, sizeof(f->c[0]) * (size_t)n);
    p->fingers = n;
}

void touch_stats(char *buf, size_t cap)
{
    ksnprintf(buf, cap, "taps %u, scroll steps %u, swipes %u", taps, scroll_steps, swipes);
}

COMMAND(touchpad, "touch", "system", "touchpad", "list touchpads and touch screens and the gestures seen")
{
    UNUSED(argc);
    UNUSED(argv);
    if (!name_count) {
        sh_puts(sh, "no touchpads or touch screens\n");
        return 0;
    }
    for (int i = 0; i < name_count; i++)
        sh_printf(sh, "%s (%s)\n", names[i], screens[i] ? "touch screen" : "touchpad");
    char line[80];
    touch_stats(line, sizeof(line));
    sh_printf(sh, "gestures: %s\n", line);
    return 0;
}

#endif
