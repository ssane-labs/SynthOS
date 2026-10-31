/* SynthOS UEFI loader: loads \SYNTHOS\KERNEL.BIN from the boot volume, picks a framebuffer,
 * converts the memory map, exits boot services and enters the 32-bit kernel at 1 MiB.
 * On an installed system it also adds itself to the firmware boot menu once and offers the other systems. */

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
    void *test_string, *query_mode, *set_mode;
    status(EFIAPI *set_attribute)(struct text_output *self, uintn attr);
    status(EFIAPI *clear_screen)(struct text_output *self);
    status(EFIAPI *set_cursor_position)(struct text_output *self, uintn col, uintn row);
    status(EFIAPI *enable_cursor)(struct text_output *self, u8 on);
};

struct input_key {
    u16 scan;
    char16 unicode;
};

struct text_input {
    void *reset;
    status(EFIAPI *read_key_stroke)(struct text_input *self, struct input_key *key);
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
    status(EFIAPI *load_image)(u8 boot_policy, void *parent, void *path, void *source, uintn size, void **out);
    status(EFIAPI *start_image)(void *image, uintn *exit_size, char16 **exit_data);
    void *exit, *unload_image;
    status(EFIAPI *exit_boot_services)(void *image, uintn key);
    void *get_next_count;
    status(EFIAPI *stall)(uintn microseconds);
    status(EFIAPI *set_watchdog_timer)(uintn timeout, u64 code, uintn size, const char16 *data);
    void *connect, *disconnect, *open_protocol, *close_protocol, *open_info, *protocols_per_handle;
    status(EFIAPI *locate_handle_buffer)(u32 search, const struct guid *guid, void *key, uintn *count,
                                         void ***handles);
    status(EFIAPI *locate_protocol)(const struct guid *guid, void *registration, void **out);
};

struct runtime_services {
    struct table_header hdr;
    void *get_time, *set_time, *get_wakeup_time, *set_wakeup_time, *set_virtual_address_map, *convert_pointer;
    status(EFIAPI *get_variable)(const char16 *name, const struct guid *vendor, u32 *attrs, uintn *size, void *data);
    void *get_next_variable_name;
    status(EFIAPI *set_variable)(const char16 *name, const struct guid *vendor, u32 attrs, uintn size,
                                 const void *data);
    void *get_next_high_count;
    void(EFIAPI *reset_system)(u32 type, status code, uintn size, void *data);
};

struct system_table {
    struct table_header hdr;
    char16 *vendor;
    u32 revision;
    void *con_in_handle;
    struct text_input *con_in;
    void *con_out_handle;
    struct text_output *con_out;
    void *stderr_handle, *stderr;
    struct runtime_services *runtime;
    struct boot_services *boot;
};

struct loaded_image {
    u32 revision;
    void *parent, *system_table;
    void *device;
    void *file_path;
};

struct file {
    u64 revision;
    status(EFIAPI *open)(struct file *self, struct file **out, const char16 *name, u64 mode, u64 attrs);
    status(EFIAPI *close)(struct file *self);
    void *remove;
    status(EFIAPI *read)(struct file *self, uintn *size, void *buf);
    status(EFIAPI *write)(struct file *self, uintn *size, const void *buf);
    void *get_position;
    status(EFIAPI *set_position)(struct file *self, u64 position);
    void *get_info, *set_info;
    status(EFIAPI *flush)(struct file *self);
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
    u32 system_lba, system_sectors;
    u8 id[8];
};

struct e820 {
    u64 base, length;
    u32 type, ext;
};

#define BOOT_MAGIC 0x5359424fu
#define KERNEL_MAGIC 0x4b4e5953u
#define KERNEL_ADDR 0x100000ull
#define KERNEL_LIMIT 0x800000ull
#define KERNEL_LBA 17
#define MMAP_MAX 160
#define BOOT_UEFI 1
#define BOOT_SYSTEM 2
#define BY_PROTOCOL 2
#define NOT_FOUND 0x800000000000000eull
#define VAR_ATTRS 7
#define FILE_READ 1
#define FILE_WRITE 2
#define SCAN_UP 1
#define SCAN_DOWN 2
#define SCAN_ESC 0x17
#define ALLOCATE_MAX_ADDRESS 1
#define ALLOCATE_ADDRESS 2
#define LOADER_CODE 1
#define LOADER_DATA 2

static const struct guid loaded_image_guid = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const struct guid simple_fs_guid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const struct guid device_path_guid = {0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const struct guid global_guid = {0x8be4df61, 0x93ca, 0x11d2, {0xaa, 0x0d, 0x00, 0xe0, 0x98, 0x03, 0x2b, 0x8c}};
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

/* Under Secure Boot the firmware checks the signature of this loader, and the loader checks the kernel: the build
   writes the SHA-256 of KERNEL.BIN into kernel_seal and then signs the loader. */
static const volatile struct {
    char tag[16];
    u8 sum[32];
} kernel_seal __attribute__((used)) = {"SynthOSKernelSum", {0}};

/* shim (the Microsoft-signed first stage on the stick) starts only images that carry an SBAT generation list. */
static const char sbat[] __attribute__((section(".sbat"), used)) =
    "sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md\n"
    "synthos,1,SynthOS,synthos,1,https://github.com/ssane-labs/SynthOS\n";

static const u32 sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static u32 ror(u32 x, int n)
{
    return x >> n | x << (32 - n);
}

static void sha_block(u32 *h, const u8 *p)
{
    u32 w[64], v[8];
    for (int i = 0; i < 16; i++)
        w[i] = (u32)p[4 * i] << 24 | (u32)p[4 * i + 1] << 16 | (u32)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ w[i - 15] >> 3) + w[i - 7] +
               (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ w[i - 2] >> 10);
    for (int i = 0; i < 8; i++)
        v[i] = h[i];
    for (int i = 0; i < 64; i++) {
        u32 t1 = v[7] + (ror(v[4], 6) ^ ror(v[4], 11) ^ ror(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + sha_k[i] + w[i];
        u32 t2 = (ror(v[0], 2) ^ ror(v[0], 13) ^ ror(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
        for (int k = 7; k > 0; k--)
            v[k] = v[k - 1];
        v[4] += t1;
        v[0] = t1 + t2;
    }
    for (int i = 0; i < 8; i++)
        h[i] += v[i];
}

static void sha256(const u8 *data, uintn len, u8 *out)
{
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uintn i = 0;
    for (; i + 64 <= len; i += 64)
        sha_block(h, data + i);
    u8 tail[128];
    uintn rest = len - i, n = rest < 56 ? 64 : 128;
    fill(tail, 0, sizeof(tail));
    copy(tail, data + i, rest);
    tail[rest] = 0x80;
    u64 bits = (u64)len * 8;
    for (int k = 0; k < 8; k++)
        tail[n - 1 - k] = (u8)(bits >> (8 * k));
    for (uintn k = 0; k < n; k += 64)
        sha_block(h, tail + k);
    for (int k = 0; k < 32; k++)
        out[k] = (u8)(h[k / 4] >> (24 - 8 * (k % 4)));
}

static int secure_boot_on(void)
{
    u8 value = 0;
    u32 attrs;
    uintn size = 1;
    return !ERROR(st->runtime->get_variable(u"SecureBoot", &global_guid, &attrs, &size, &value)) && value == 1;
}

static int kernel_sealed(const u8 *kernel, uintn size)
{
    u8 sum[32];
    int set = 0, same = 1;
    sha256(kernel, size, sum);
    for (int i = 0; i < 32; i++) {
        set |= kernel_seal.sum[i];
        same &= kernel_seal.sum[i] == sum[i];
    }
    return set && same;
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
    if (ERROR(bs->locate_protocol(&gop_guid, 0, (void **)&gop)) || !gop->mode) {
        say("SynthOS: no graphics output, using the serial console\n");
        return;
    }
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

static struct file *open_root(void *device)
{
    struct simple_fs *fs;
    struct file *root;
    if (!device || ERROR(bs->handle_protocol(device, &simple_fs_guid, (void **)&fs)) || ERROR(fs->open_volume(fs, &root)))
        return 0;
    return root;
}

static u8 *load_kernel(struct file *root, uintn *size)
{
    struct file *file;
    static const char16 path[] = u"\\SYNTHOS\\KERNEL.BIN";
    if (!root || ERROR(root->open(root, &file, path, FILE_READ, 0)))
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

/* \SYNTHOS\SYSTEM.CFG exists on the EFI partition of an installed system: where the system area starts, its id,
   whether the loader still has to add itself to the firmware boot menu, and how long the boot menu waits. */
struct config {
    int present, reg, timeout;
    u64 system, sectors;
    u8 id[8];
    uintn reg_offset;
};

static int hex(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int key_is(const char *line, const char *key)
{
    while (*key)
        if (*line++ != *key++)
            return 0;
    return *line == '=';
}

static int read_config(struct file *root, struct config *cfg)
{
    static const char16 path[] = u"\\SYNTHOS\\SYSTEM.CFG";
    struct file *file;
    fill(cfg, 0, sizeof(*cfg));
    cfg->timeout = 3;
    if (!root || ERROR(root->open(root, &file, path, FILE_READ, 0)))
        return 0;
    char text[1024];
    uintn size = sizeof(text) - 1;
    if (ERROR(file->read(file, &size, text)))
        size = 0;
    file->close(file);
    text[size] = 0;
    for (uintn i = 0; i < size;) {
        const char *line = text + i;
        uintn len = 0;
        while (i + len < size && text[i + len] != '\n')
            len++;
        const char *v = line;
        while (*v && *v != '=' && *v != '\n')
            v++;
        if (*v == '=')
            v++;
        u64 n = 0;
        for (const char *p = v; *p >= '0' && *p <= '9'; p++)
            n = n * 10 + (u64)(*p - '0');
        if (key_is(line, "system"))
            cfg->system = n;
        else if (key_is(line, "sectors"))
            cfg->sectors = n;
        else if (key_is(line, "timeout"))
            cfg->timeout = (int)n;
        else if (key_is(line, "register")) {
            cfg->reg = n != 0;
            cfg->reg_offset = (uintn)(v - text);
        } else if (key_is(line, "id"))
            for (int k = 0; k < 8 && hex(v[2 * k]) >= 0 && hex(v[2 * k + 1]) >= 0; k++)
                cfg->id[k] = (u8)(hex(v[2 * k]) << 4 | hex(v[2 * k + 1]));
        i += len + 1;
    }
    cfg->present = 1;
    return 1;
}

static void clear_register(struct file *root, struct config *cfg)
{
    static const char16 path[] = u"\\SYNTHOS\\SYSTEM.CFG";
    struct file *file;
    if (ERROR(root->open(root, &file, path, FILE_READ | FILE_WRITE, 0)))
        return;
    uintn one = 1;
    if (!ERROR(file->set_position(file, cfg->reg_offset)))
        file->write(file, &one, "0");
    file->flush(file);
    file->close(file);
    cfg->reg = 0;
}

static uintn path_size(const u8 *p)
{
    uintn n = 0;
    for (;;) {
        uintn len = p[n + 2] | p[n + 3] << 8;
        if (p[n] == 0x7f && p[n + 1] == 0xff)
            return n;
        if (len < 4)
            return n;
        n += len;
    }
}

static uintn str16_len(const char16 *s)
{
    uintn n = 0;
    while (s[n])
        n++;
    return n;
}

/* The device path of a file: the volume's path, a file node, the end node. */
static u8 *file_path(void *device, const char16 *name, uintn *out_size)
{
    u8 *dev;
    if (ERROR(bs->handle_protocol(device, &device_path_guid, (void **)&dev)))
        return 0;
    uintn head = path_size(dev), node = 4 + (str16_len(name) + 1) * 2, size = head + node + 4;
    u8 *p;
    if (ERROR(bs->allocate_pool(LOADER_DATA, size, (void **)&p)))
        return 0;
    copy(p, dev, head);
    u8 *f = p + head;
    f[0] = 4;
    f[1] = 4;
    f[2] = (u8)node;
    f[3] = (u8)(node >> 8);
    copy(f + 4, name, node - 4);
    u8 *end = f + node;
    end[0] = 0x7f;
    end[1] = 0xff;
    end[2] = 4;
    end[3] = 0;
    *out_size = size;
    return p;
}

static void boot_name(char16 *out, u16 number)
{
    static const char digits[] = "0123456789ABCDEF";
    static const char16 prefix[] = u"Boot";
    for (int i = 0; i < 4; i++)
        out[i] = prefix[i];
    for (int i = 0; i < 4; i++)
        out[4 + i] = (char16)digits[(number >> (12 - 4 * i)) & 15];
    out[8] = 0;
}

static int same(const void *a, const void *b, uintn n)
{
    const u8 *x = a, *y = b;
    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static u8 option[1024];

/* The short form of a path starts at the partition node: the firmware finds the partition by its GUID on whatever
   port the disk sits. */
static uintn disk_node(const u8 *p, uintn len)
{
    for (uintn n = 0; n + 4 <= len;) {
        uintn l = p[n + 2] | p[n + 3] << 8;
        if (p[n] == 4 && p[n + 1] == 1)
            return n;
        if (l < 4 || (p[n] == 0x7f && p[n + 1] == 0xff))
            break;
        n += l;
    }
    return len;
}

/* Adds "SynthOS" to the firmware boot menu (a Boot#### variable) and puts it first in BootOrder. */
static int register_boot(const u8 *path, uintn path_len)
{
    static const char16 desc[] = u"SynthOS";
    uintn skip = disk_node(path, path_len);
    if (skip < path_len) {
        path += skip;
        path_len -= skip;
    }
    uintn desc_size = sizeof(desc), size = 6 + desc_size + path_len;
    if (size > sizeof(option))
        return 0;
    fill(option, 0, sizeof(option));
    option[0] = 1;
    option[4] = (u8)path_len;
    option[5] = (u8)(path_len >> 8);
    copy(option + 6, desc, desc_size);
    copy(option + 6 + desc_size, path, path_len);
    u16 order[128];
    uintn order_size = sizeof(order);
    u32 attrs;
    if (ERROR(st->runtime->get_variable(u"BootOrder", &global_guid, &attrs, &order_size, order)))
        order_size = 0;
    uintn count = order_size / 2;
    int number = -1, free_number = -1;
    for (u32 n = 0; n < 0x1000 && number < 0; n++) {
        char16 name[9];
        static u8 data[1024];
        uintn data_size = sizeof(data);
        boot_name(name, (u16)n);
        status r = st->runtime->get_variable(name, &global_guid, &attrs, &data_size, data);
        if (r == NOT_FOUND) {
            if (free_number < 0)
                free_number = (int)n;
            if (n > 0x100 && free_number >= 0)
                break;
            continue;
        }
        if (!ERROR(r) && data_size > 6 + desc_size && same(data + 6, desc, desc_size))
            number = (int)n;
    }
    if (number < 0)
        number = free_number;
    if (number < 0)
        return 0;
    char16 name[9];
    boot_name(name, (u16)number);
    if (ERROR(st->runtime->set_variable(name, &global_guid, VAR_ATTRS, size, option)))
        return 0;
    u16 next[129];
    uintn k = 0;
    next[k++] = (u16)number;
    for (uintn i = 0; i < count && k < 129; i++)
        if (order[i] != number)
            next[k++] = order[i];
    return !ERROR(st->runtime->set_variable(u"BootOrder", &global_guid, VAR_ATTRS, k * 2, next));
}

static char16 lower16(char16 c)
{
    return c >= 'A' && c <= 'Z' ? (char16)(c + 32) : c;
}

static int file_exists(struct file *root, const char16 *name)
{
    struct file *file;
    if (!root || ERROR(root->open(root, &file, name, FILE_READ, 0)))
        return 0;
    file->close(file);
    return 1;
}

/* The menu entry points at this loader, or at shim when shim started it (grubx64.efi next to BOOTX64.EFI or
   shimx64.efi): under Secure Boot only shim gets past the firmware. */
static int register_self(void *image, struct file *root)
{
    struct loaded_image *li;
    if (ERROR(bs->handle_protocol(image, &loaded_image_guid, (void **)&li)) || !li->file_path)
        return 0;
    const u8 *node = li->file_path;
    if (node[0] != 4 || node[1] != 4)
        return 0;
    uintn len = (uintn)(node[2] | node[3] << 8);
    static char16 name[260];
    uintn n = len > 4 ? (len - 4) / 2 : 0;
    if (n >= 240)
        return 0;
    copy(name, node + 4, n * 2);
    name[n] = 0;
    n = str16_len(name);
    uintn base = n;
    while (base > 0 && name[base - 1] != '\\')
        base--;
    static const char16 grub[] = u"grubx64.efi", *const shims[] = {u"BOOTX64.EFI", u"shimx64.efi"};
    int is_grub = n - base == 11;
    for (uintn i = 0; is_grub && i < 11; i++)
        is_grub = lower16(name[base + i]) == grub[i];
    for (int k = 0; is_grub && k < 2; k++) {
        copy(name + base, shims[k], (str16_len(shims[k]) + 1) * 2);
        if (file_exists(root, name))
            break;
        if (k == 1)
            return 0;
    }
    uintn size;
    u8 *path = file_path(li->device, name, &size);
    return path && register_boot(path, size);
}

/* The boot stick finds a fresh installation by its SYSTEM.CFG, registers its loader and starts it right away. */
static void start_new_installation(void *image, void *own_device)
{
    void **handles;
    uintn count;
    if (ERROR(bs->locate_handle_buffer(BY_PROTOCOL, &simple_fs_guid, 0, &count, &handles)))
        return;
    for (uintn i = 0; i < count; i++) {
        if (handles[i] == own_device)
            continue;
        struct file *root = open_root(handles[i]);
        struct config cfg;
        if (!root || !read_config(root, &cfg) || !cfg.reg)
            continue;
        uintn size;
        u8 *path = file_path(handles[i], u"\\EFI\\BOOT\\BOOTX64.EFI", &size);
        if (!path)
            continue;
        say("SynthOS: found the new installation, adding it to the boot menu\n");
        if (register_boot(path, size))
            clear_register(root, &cfg);
        void *child;
        if (!ERROR(bs->load_image(0, image, path, 0, 0, &child)))
            bs->start_image(child, 0, 0);
        say("SynthOS: the installed system did not start, starting from this disk\n");
        return;
    }
}

/* Other systems are the firmware's own boot entries on disks; one is started by the firmware itself (BootNext and
   a restart), exactly as it would start without SynthOS. */
struct entry {
    u16 number;
    char16 name[40];
};

static int has_disk_node(const u8 *p, uintn len)
{
    return disk_node(p, len) < len;
}

static int other_systems(struct entry *out, int max)
{
    u16 order[128];
    uintn order_size = sizeof(order);
    u32 attrs;
    if (ERROR(st->runtime->get_variable(u"BootOrder", &global_guid, &attrs, &order_size, order)))
        return 0;
    int n = 0;
    for (uintn i = 0; i < order_size / 2 && n < max; i++) {
        char16 name[9];
        static u8 data[1024];
        uintn size = sizeof(data);
        boot_name(name, order[i]);
        if (ERROR(st->runtime->get_variable(name, &global_guid, &attrs, &size, data)) || size < 8 || !(data[0] & 1))
            continue;
        const char16 *desc = (const char16 *)(data + 6);
        uintn len = 0;
        while (6 + 2 * (len + 1) <= size && desc[len])
            len++;
        uintn path_len = data[4] | data[5] << 8, path_at = 6 + 2 * (len + 1);
        if (path_at + path_len > size || !has_disk_node(data + path_at, path_len) || same(desc, u"SynthOS", 14))
            continue;
        out[n].number = order[i];
        uintn k = 0;
        for (; k < len && k < 39; k++)
            out[n].name[k] = desc[k];
        out[n].name[k] = 0;
        n++;
    }
    return n;
}

static void say16(const char16 *s)
{
    st->con_out->output_string(st->con_out, s);
}

static void draw_menu(struct entry *list, int n, int chosen, int seconds)
{
    st->con_out->clear_screen(st->con_out);
    say("\n  SynthOS boot menu\n\n");
    for (int i = 0; i <= n; i++) {
        st->con_out->set_attribute(st->con_out, i == chosen ? 0x70 : 0x07);
        say(i == chosen ? "  > " : "    ");
        if (i == 0)
            say("SynthOS");
        else
            say16(list[i - 1].name);
        say("  \n");
        st->con_out->set_attribute(st->con_out, 0x07);
    }
    say("\n  Up and Down (or a number) choose, Enter starts.");
    if (seconds >= 0) {
        char line[48] = "  SynthOS starts in 0 s.  ";
        line[20] = (char)('0' + (seconds > 9 ? 9 : seconds));
        say(line);
    }
    say("\n");
}

static void boot_menu(int timeout)
{
    struct entry list[8];
    int n = other_systems(list, 8);
    if (!n || timeout <= 0 || !st->con_in)
        return;
    int chosen = 0, ticks = timeout * 10;
    draw_menu(list, n, chosen, timeout);
    for (;;) {
        struct input_key key;
        if (ERROR(st->con_in->read_key_stroke(st->con_in, &key))) {
            bs->stall(100000);
            if (ticks > 0 && --ticks % 10 == 0)
                draw_menu(list, n, chosen, ticks / 10);
            if (ticks == 0)
                break;
            continue;
        }
        ticks = -1;
        if (key.scan == SCAN_UP && chosen > 0)
            chosen--;
        else if (key.scan == SCAN_DOWN && chosen < n)
            chosen++;
        else if (key.unicode == '\r' || key.unicode == '\n')
            break;
        else if (key.unicode >= '1' && key.unicode <= '1' + n) {
            chosen = key.unicode - '1';
            break;
        }
        else if (key.scan == SCAN_ESC)
            chosen = 0;
        draw_menu(list, n, chosen, -1);
    }
    st->con_out->clear_screen(st->con_out);
    if (chosen == 0)
        return;
    u16 number = list[chosen - 1].number;
    if (!ERROR(st->runtime->set_variable(u"BootNext", &global_guid, VAR_ATTRS, 2, &number)))
        st->runtime->reset_system(0, 0, 0, 0);
    say("SynthOS: could not start the other system\n");
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

    struct loaded_image *li;
    if (ERROR(bs->handle_protocol(image, &loaded_image_guid, (void **)&li)))
        return fail("no loaded image");
    struct file *root = open_root(li->device);
    struct config cfg;
    if (read_config(root, &cfg)) {
        if (cfg.reg && register_self(image, root))
            clear_register(root, &cfg);
        boot_menu(cfg.timeout);
    } else {
        start_new_installation(image, li->device);
    }

    u64 kernel_addr = KERNEL_ADDR;
    bs->allocate_pages(ALLOCATE_ADDRESS, LOADER_DATA, (KERNEL_LIMIT - KERNEL_ADDR) / 4096, &kernel_addr);
    uintn kernel_size = 0;
    u8 *kernel = load_kernel(root, &kernel_size);
    if (!kernel || kernel_size < 512 || *(u32 *)(kernel + 8) != KERNEL_MAGIC)
        return fail("\\SYNTHOS\\KERNEL.BIN is missing or damaged");
    if (secure_boot_on() && !kernel_sealed(kernel, kernel_size))
        return fail("Secure Boot: \\SYNTHOS\\KERNEL.BIN is not the kernel this loader was signed with");

    fill(bi, 0, sizeof(*bi));
    bi->magic = BOOT_MAGIC;
    bi->kernel_sectors = *(u32 *)(kernel + 12);
    bi->kernel_lba = KERNEL_LBA;
    bi->flags = BOOT_UEFI | BOOT_SYSTEM;
    bi->system_lba = (u32)cfg.system;
    bi->system_sectors = (u32)cfg.sectors;
    copy(bi->id, cfg.id, 8);
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
