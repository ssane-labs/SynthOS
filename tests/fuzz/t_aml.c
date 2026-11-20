/* the AML interpreter: the input is the body of a DSDT; every device is initialized, then every method is run */
#include <stdlib.h>
#include <string.h>

#include <synth/aml.h>

#include "targets.h"

static uint64_t fake_read(int space, uint64_t addr, int bits)
{
    uint64_t v = (addr * 0x9e3779b97f4a7c15ull) ^ (uint64_t)space;
    return bits == 64 ? v : v & ((1ull << bits) - 1);
}

static void fake_write(int space, uint64_t addr, int bits, uint64_t value)
{
    (void)space;
    (void)addr;
    (void)bits;
    (void)value;
}

static void no_notify(struct aml_node *n, uint64_t v)
{
    (void)n;
    (void)v;
}

void fuzz_aml(const uint8_t *d, size_t n)
{
    uint8_t *t = malloc(n + 36);
    memset(t, 0, 36);
    memcpy(t, "DSDT", 4);
    uint32_t len = (uint32_t)(n + 36);
    memcpy(t + 4, &len, 4);
    t[8] = 2;
    memcpy(t + 36, d, n);
    aml_os.read = fake_read;
    aml_os.write = fake_write;
    aml_os.notify = no_notify;
    aml_loop_limit = 500;
    aml_load(t, len);
    aml_init_devices();
    int runs = 0;
    for (struct aml_node *x = aml_walk(NULL); x && runs < 200; x = aml_walk(x)) {
        if (aml_node_type(x) != AML_METHOD && aml_node_type(x) != AML_DEVICE)
            continue;
        struct aml_obj *args[7];
        for (int i = 0; i < 7; i++)
            args[i] = aml_int((uint64_t)i * 3 + 1);
        struct aml_obj *r = NULL;
        if (aml_node_type(x) == AML_METHOD)
            aml_eval(x, NULL, args, 7, &r);
        else
            aml_hid_is(x, "PNP0C0D");
        aml_put(r);
        for (int i = 0; i < 7; i++)
            aml_put(args[i]);
        runs++;
    }
    aml_reset();
    free(t);
}
