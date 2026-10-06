/* SynthOS UEFI loader: loads \SYNTHOS\KERNEL.BIN from the boot volume, picks a framebuffer,
 * converts the memory map, exits boot services and enters the 32-bit kernel at 1 MiB. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef u64 uintn;
typedef u64 status;
typedef u16 char16;

#define EFIAPI __attribute__((ms_abi))
#define ERROR(s) ((s) >> 63)

struct guid {
    u32 a;
    u16 b, c;
    u8 d[8];
};

struct table_header {
    u64 signature;
    u32 revision, size, crc, reserved;
};

struct text_output {
    void *reset;
    status(EFIAPI *output_string)(struct text_output *self, const char16 *s);
};

struct memory_descriptor {
    u32 type;
    u32 pad;
    u64 physical_start;
    u64 virtual_start;
    u64 pages;
    u64 attribute;
};

struct boot_services {
    struct table_header hdr;
    void *raise_tpl, *restore_tpl;
    status(EFIAPI *allocate_pages)(u32 type, u32 memory_type, uintn pages, u64 *address);
    status(EFIAPI *free_pages)(u64 address, uintn pages);
    status(EFIAPI *get_memory_map)(uintn *size, struct memory_descriptor *map, uintn *key, uintn *desc_size,
                                   u32 *desc_version);
    status(EFIAPI *allocate_pool)(u32 type, uintn size, void **out);
    status(EFIAPI *free_pool)(void *p);
    void *create_event, *set_timer, *wait_for_event, *signal_event, *close_event, *check_event;
    void *install_protocol, *reinstall_protocol, *uninstall_protocol;
    status(EFIAPI *handle_protocol)(void *handle, const struct guid *guid, void **out);
    void *reserved, *register_notify, *locate_handle, *locate_device_path, *install_config;
    void *load_image, *start_image, *exit, *unload_image;
    status(EFIAPI *exit_boot_services)(void *image, uintn key);
    void *get_next_count, *stall;
    status(EFIAPI *set_watchdog_timer)(uintn timeout, u64 code, uintn size, const char16 *data);
    void *connect, *disconnect, *open_protocol, *close_protocol, *open_info, *protocols_per_handle;
    void *locate_handle_buffer;
    status(EFIAPI *locate_protocol)(const struct guid *guid, void *registration, void **out);
};

struct system_table {
    struct table_header hdr;
    char16 *vendor;
    u32 revision;
    void *con_in_handle, *con_in, *con_out_handle;
    struct text_output *con_out;
    void *stderr_handle, *stderr;
    void *runtime;
    struct boot_services *boot;
};

struct loaded_image {
    u32 revision;
    void *parent, *system_table;
    void *device;
};

struct file {
    u64 revision;
    status(EFIAPI *open)(struct file *self, struct file **out, const char16 *name, u64 mode, u64 attrs);
    status(EFIAPI *close)(struct file *self);
    void *remove;
    status(EFIAPI *read)(struct file *self, uintn *size, void *buf);
};

struct simple_fs {
    u64 revision;
    status(EFIAPI *open_volume)(struct simple_fs *self, struct file **root);
};

struct gop_mode_info {
    u32 version, width, height, format;
    u32 red_mask, green_mask, blue_mask, reserved_mask;
    u32 pixels_per_line;
};

struct gop_mode {
    u32 max_mode, mode;
    struct gop_mode_info *info;
    uintn info_size;
    u64 fb_base;
    uintn fb_size;
};

struct gop {
    status(EFIAPI *query_mode)(struct gop *self, u32 mode, uintn *size, struct gop_mode_info **info);
    status(EFIAPI *set_mode)(struct gop *self, u32 mode);
    void *blt;
    struct gop_mode *mode;
};

struct bootinfo {
    u32 magic, drive, mmap_addr, mmap_count, kernel_sectors, kernel_lba, flags;
    u32 fb_addr, fb_width, fb_height, fb_pitch, fb_bpp;
    u8 fb_shift[4];
};

struct e820 {
    u64 base, length;
    u32 type, ext;
};

#define BOOT_MAGIC 0x5359424fu
#define KERNEL_MAGIC 0x4b4e5953u
#define KERNEL_ADDR 0x100000ull
#define KERNEL_LIMIT 0x300000ull
#define KERNEL_LBA 17
#define MMAP_MAX 160
#define BOOT_UEFI 1
#define ALLOCATE_MAX_ADDRESS 1
#define ALLOCATE_ADDRESS 2
#define LOADER_CODE 1
#define LOADER_DATA 2

static const struct guid loaded_image_guid = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const struct guid simple_fs_guid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const struct guid gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};

extern const u8 trampoline_start[], trampoline_end[];

static struct system_table *st;
static struct boot_services *bs;

void *memmove(void *dst, const void *src, uintn n)
{
    u8 *d = dst;
    const u8 *s = src;
    if (d < s)
        while (n--)
            *d++ = *s++;
    else
        while (n--)
            d[n] = s[n];
    return dst;
}

void *memcpy(void *dst, const void *src, uintn n)
{
    return memmove(dst, src, n);
}

void *memset(void *dst, int v, uintn n)
{
    u8 *d = dst;
    while (n--)
        *d++ = (u8)v;
    return dst;
}

#define copy memmove
#define fill memset

static void say(const char *s)
{
    char16 buf[128];
    uintn n = 0;
    while (*s && n < 126) {
        if (*s == '\n')
            buf[n++] = '\r';
        buf[n++] = (u8)*s++;
    }
    buf[n] = 0;
    st->con_out->output_string(st->con_out, buf);
}

static status fail(const char *why)
{
    say("SynthOS: ");
    say(why);
    say("\n");
    for (;;)
        __asm__ volatile("hlt");
    return 1;
}

static u8 mask_shift(u32 mask)
{
    u8 s = 0;
    while (mask && !(mask & 1)) {
        mask >>= 1;
        s++;
    }
    return s;
}

static int bits(u32 mask)
{
    int n = 0;
    for (; mask; mask >>= 1)
        n += mask & 1;
    return n;
}

static void setup_framebuffer(struct bootinfo *bi)
{
    struct gop *gop;
    if (ERROR(bs->locate_protocol(&gop_guid, 0, (void **)&gop)) || !gop->mode)
        return;
    u32 best = gop->mode->mode;
    u64 best_score = 0;
    for (u32 m = 0; m < gop->mode->max_mode; m++) {
        struct gop_mode_info *info;
        uintn size;
        if (ERROR(gop->query_mode(gop, m, &size, &info)) || info->format > 2)
            continue;
        if (info->width > 1920 || info->height > 1200)
            continue;
        u64 score = (u64)info->width * info->height;
        if (score > best_score) {
            best_score = score;
            best = m;
        }
    }
    if (best != gop->mode->mode)
        gop->set_mode(gop, best);
    struct gop_mode_info *info = gop->mode->info;
    if (info->format > 2 || gop->mode->fb_base + gop->mode->fb_size > 0x100000000ull) {
        say("SynthOS: framebuffer is not usable, falling back to serial console\n");
        return;
    }
    bi->fb_addr = (u32)gop->mode->fb_base;
    bi->fb_width = info->width;
    bi->fb_height = info->height;
    bi->fb_pitch = info->pixels_per_line * 4;
    bi->fb_bpp = 32;
    if (info->format == 0) {
        bi->fb_shift[0] = 0;
        bi->fb_shift[1] = 8;
        bi->fb_shift[2] = 16;
    } else if (info->format == 1) {
        bi->fb_shift[0] = 16;
        bi->fb_shift[1] = 8;
        bi->fb_shift[2] = 0;
    } else {
        int depth = bits(info->red_mask | info->green_mask | info->blue_mask | info->reserved_mask);
        bi->fb_bpp = depth > 24 ? 32 : depth > 16 ? 24 : 16;
        bi->fb_pitch = info->pixels_per_line * (bi->fb_bpp / 8);
        bi->fb_shift[0] = mask_shift(info->red_mask);
        bi->fb_shift[1] = mask_shift(info->green_mask);
        bi->fb_shift[2] = mask_shift(info->blue_mask);
    }
}

static u8 *load_kernel(void *image, uintn *size)
{
    struct loaded_image *li;
    struct simple_fs *fs;
    struct file *root, *file;
    static const char16 path[] = u"\\SYNTHOS\\KERNEL.BIN";
    if (ERROR(bs->handle_protocol(image, &loaded_image_guid, (void **)&li)) ||
        ERROR(bs->handle_protocol(li->device, &simple_fs_guid, (void **)&fs)) || ERROR(fs->open_volume(fs, &root)) ||
        ERROR(root->open(root, &file, path, 1, 0)))
        return 0;
    uintn cap = KERNEL_LIMIT - KERNEL_ADDR;
    u8 *buf;
    if (ERROR(bs->allocate_pool(LOADER_DATA, cap, (void **)&buf)))
        return 0;
    uintn total = 0;
    for (;;) {
        uintn chunk = cap - total;
        if (!chunk || ERROR(file->read(file, &chunk, buf + total)) || !chunk)
            break;
        total += chunk;
    }
    file->close(file);
    *size = total;
    return buf;
}

static u32 e820_type(u32 efi_type)
{
    switch (efi_type) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 7:
        return 1;
    case 9:
        return 3;
    case 10:
        return 4;
    default:
        return 2;
    }
}

static u32 convert_map(struct memory_descriptor *map, uintn size, uintn desc_size, struct e820 *out)
{
    u32 n = 0;
    for (uintn off = 0; off < size; off += desc_size) {
        struct memory_descriptor *d = (struct memory_descriptor *)((u8 *)map + off);
        u64 base = d->physical_start, length = d->pages * 4096;
        u32 type = e820_type(d->type);
        if (n && out[n - 1].type == type && out[n - 1].base + out[n - 1].length == base) {
            out[n - 1].length += length;
            continue;
        }
        if (n == MMAP_MAX)
            break;
        out[n++] = (struct e820){base, length, type, 1};
    }
    return n;
}

static int kernel_region_free(struct memory_descriptor *map, uintn size, uintn desc_size)
{
    for (uintn off = 0; off < size; off += desc_size) {
        struct memory_descriptor *d = (struct memory_descriptor *)((u8 *)map + off);
        u64 start = d->physical_start, end = start + d->pages * 4096;
        if (end > KERNEL_ADDR && start < KERNEL_LIMIT && e820_type(d->type) != 1)
            return 0;
    }
    return 1;
}

status EFIAPI efi_main(void *image, struct system_table *table)
{
    st = table;
    bs = table->boot;
    bs->set_watchdog_timer(0, 0, 0, 0);
    say("SynthOS UEFI loader\n");

    u64 low = 0x9ffff;
    if (ERROR(bs->allocate_pages(ALLOCATE_MAX_ADDRESS, LOADER_CODE, 4, &low)))
        return fail("no low memory for the boot data");
    u8 *page = (u8 *)(uintn)low;
    fill(page, 0, 4 * 4096);
    u8 *trampoline = page;
    struct bootinfo *bi = (struct bootinfo *)(page + 0x800);
    struct e820 *e820 = (struct e820 *)(page + 0x1000);
    copy(trampoline, trampoline_start, (uintn)(trampoline_end - trampoline_start));

    u64 kernel_addr = KERNEL_ADDR;
    bs->allocate_pages(ALLOCATE_ADDRESS, LOADER_DATA, (KERNEL_LIMIT - KERNEL_ADDR) / 4096, &kernel_addr);
    uintn kernel_size = 0;
    u8 *kernel = load_kernel(image, &kernel_size);
    if (!kernel || kernel_size < 512 || *(u32 *)(kernel + 8) != KERNEL_MAGIC)
        return fail("\\SYNTHOS\\KERNEL.BIN is missing or damaged");

    fill(bi, 0, sizeof(*bi));
    bi->magic = BOOT_MAGIC;
    bi->kernel_sectors = *(u32 *)(kernel + 12);
    bi->kernel_lba = KERNEL_LBA;
    bi->flags = BOOT_UEFI;
    bi->mmap_addr = (u32)(uintn)e820;
    setup_framebuffer(bi);

    uintn map_size = 0, key, desc_size;
    u32 desc_version;
    struct memory_descriptor *map = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        map_size = 0;
        bs->get_memory_map(&map_size, 0, &key, &desc_size, &desc_version);
        map_size += 8 * desc_size;
        if (map)
            bs->free_pool(map);
        if (ERROR(bs->allocate_pool(LOADER_DATA, map_size, (void **)&map)))
            return fail("cannot allocate the memory map");
        if (ERROR(bs->get_memory_map(&map_size, map, &key, &desc_size, &desc_version)))
            continue;
        if (!kernel_region_free(map, map_size, desc_size))
            return fail("memory at 1 MiB is reserved by the firmware");
        bi->mmap_count = convert_map(map, map_size, desc_size, e820);
        if (!ERROR(bs->exit_boot_services(image, key)))
            goto exited;
    }
    return fail("cannot exit boot services");

exited:
    copy((void *)(uintn)KERNEL_ADDR, kernel, kernel_size);
    __asm__ volatile("cli\n\t"
                     "jmp *%0"
                     :
                     : "r"((uintn)trampoline), "S"((uintn)bi)
                     : "memory");
    for (;;)
        ;
}
