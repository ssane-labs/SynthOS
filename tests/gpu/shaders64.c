/* Writes programs/win64/d3d64_shaders.h: the shaders of the D3D11 test program, assembled with asm.h.
   cc -Iinclude -o /tmp/shaders64 tests/gpu/shaders64.c && /tmp/shaders64 > programs/win64/d3d64_shaders.h */
#include <stdio.h>

#include "asm.h"

static void dump(const char *name, size_t size)
{
    printf("static const unsigned char %s[%zu] = {", name, size);
    for (size_t i = 0; i < size; i++)
        printf("%s0x%02x,", i % 16 ? " " : "\n    ", blob[i]);
    printf("\n};\n\n");
}

int main(void)
{
    printf("/* Shader Model 4 bytecode for d3d64.cpp, made by tests/gpu/shaders64.c */\n\n");
    /* vertex shader: position and color through */
    in_sig("POSITION", 0, 0, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
    out_sig("SV_Position", 0, 1, 0, 15), out_sig("COLOR", 0, 0, 1, 15);
    op(54, 0), dst(OUT, 0, 15), src(IN, 0, XYZW), end();
    op(54, 0), dst(OUT, 1, 15), src(IN, 1, XYZW), end();
    op(62, 0), end();
    dump("vs_blob", container(1, 1));
    /* pixel shader: the color times cb0[0] */
    in_sig("SV_Position", 0, 1, 0, 15), in_sig("COLOR", 0, 0, 1, 15);
    out_sig("SV_Target", 0, 0, 0, 15);
    op(56, 0), dst(OUT, 0, 15), src(IN, 1, XYZW), cbsrc(0, 0, XYZW), end();
    op(62, 0), end();
    dump("ps_blob", container(0, 1));
    return 0;
}
