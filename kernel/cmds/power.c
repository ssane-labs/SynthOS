/* acpi: what the firmware's tables describe, and evaluating its methods by hand */
#include <synth/aml.h>
#include <synth/command.h>
#include <synth/fmt.h>
#include <synth/hal.h>
#include <synth/heap.h>
#include <synth/string.h>

static void print_obj(struct shell *sh, const struct aml_obj *o, int depth)
{
    uint64_t v;
    uint32_t len;
    const uint8_t *bytes;
    switch (aml_type_of(o)) {
    case AML_INT:
        aml_obj_int(o, &v);
        sh_printf(sh, "0x%llx", (unsigned long long)v);
        break;
    case AML_STR:
        bytes = aml_buffer(o, &len);
        sh_printf(sh, "\"%s\"", (const char *)bytes);
        break;
    case AML_BUF:
        bytes = aml_buffer(o, &len);
        sh_printf(sh, "Buffer(%u) {", (unsigned)len);
        for (uint32_t i = 0; i < len && i < 32; i++)
            sh_printf(sh, i ? " %02x" : "%02x", bytes[i]);
        sh_puts(sh, len > 32 ? " ...}" : "}");
        break;
    case AML_PKG:
        sh_puts(sh, "Package {");
        for (uint32_t i = 0; i < aml_pkg_count(o); i++) {
            if (i)
                sh_puts(sh, ", ");
            if (depth < 4)
                print_obj(sh, aml_pkg_item(o, i), depth + 1);
            else
                sh_puts(sh, "...");
        }
        sh_puts(sh, "}");
        break;
    case AML_NAMEREF:
        sh_puts(sh, "(name)");
        break;
    case AML_NONE:
        sh_puts(sh, "(none)");
        break;
    default:
        sh_printf(sh, "(object type %d)", (int)aml_type_of(o));
        break;
    }
}

static void list_devices(struct shell *sh)
{
    static const char *const ids[] = {"PNP0C0D", "PNP0C0C", "PNP0C0E", "PNP0C09", "PNP0C0A", "ACPI0003",
                                      "PNP0A03", "PNP0A08", "ACPI0008", "PNP0C14"};
    static const char *const what[] = {"lid", "power button", "sleep button", "embedded controller", "battery",
                                       "AC adapter", "PCI root", "PCI Express root", "light sensor", "WMI"};
    for (struct aml_node *n = aml_walk(NULL); n; n = aml_walk(n)) {
        if (aml_node_type(n) != AML_DEVICE)
            continue;
        for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++)
            if (aml_hid_is(n, ids[i])) {
                char path[80];
                aml_path(n, path, sizeof(path));
                sh_printf(sh, "%-28s %-8s %s, status 0x%llx\n", path, ids[i], what[i],
                          (unsigned long long)aml_status(n));
                break;
            }
    }
}

COMMAND(acpi, NULL, "system", "acpi [devices | eval PATH [NUMBER...]]",
        "the ACPI events, the devices the firmware describes, or the value of one of its names")
{
    if (!aml_root()) {
        sh_puts(sh, "acpi: no ACPI tables loaded\n");
        return 1;
    }
    if (argc == 1) {
        char *buf = kmalloc(1024);
        if (!buf)
            return 1;
        hal_acpi_info(buf, 1024);
        sh_puts(sh, buf);
        kfree(buf);
        return 0;
    }
    if (!strcmp(argv[1], "devices")) {
        list_devices(sh);
        return 0;
    }
    if (strcmp(argv[1], "eval") || argc < 3 || argc > 10)
        return command_usage(sh, command_find("acpi"));
    struct aml_obj *args[7] = {0};
    int n = 0;
    for (int i = 3; i < argc; i++) {
        long v;
        if (!str_parse_long(argv[i], &v)) {
            for (int k = 0; k < n; k++)
                aml_put(args[k]);
            return command_usage(sh, command_find("acpi"));
        }
        args[n++] = aml_int((uint64_t)v);
    }
    struct aml_obj *r;
    bool ok = aml_eval(NULL, argv[2], args, n, &r);
    for (int k = 0; k < n; k++)
        aml_put(args[k]);
    if (!ok) {
        sh_printf(sh, "acpi: %s: %s\n", argv[2], aml_find(NULL, argv[2]) ? aml_last_error : "no such name");
        return 1;
    }
    sh_printf(sh, "%s = ", argv[2]);
    print_obj(sh, r, 0);
    sh_puts(sh, "\n");
    aml_put(r);
    return 0;
}
