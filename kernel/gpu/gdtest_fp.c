/* "gdtest": the graphics device tests of tests/gpu/device.c inside the kernel. With "hostgpu on" and a virtio-gpu
   with virgl the draws go to the host's graphics card, so the same scenes check both pipelines. */

#if defined(__x86_64__) && !defined(RS_HOST)

#include <synth/command.h>
#include <synth/shell.h>
#include <synth/string.h>

static struct shell *gdtest_sh;
#define printf(...) sh_printf(gdtest_sh, __VA_ARGS__)

#include "../../tests/gpu/device.c"

COMMAND(gdtest, NULL, "system", "gdtest", "run the graphics device tests (on the host's graphics card when there is one)")
{
    UNUSED(argc), UNUSED(argv);
    gdtest_sh = sh;
    failures = 0;
    int failed = device_tests();
    sh_printf(sh, "gdtest: %d failed\n", failed);
    return failed ? 1 : 0;
}

#endif
