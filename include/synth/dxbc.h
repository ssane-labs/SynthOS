#ifndef SYNTH_DXBC_H
#define SYNTH_DXBC_H

/* Direct3D shader bytecode (DXBC, Shader Model 4 and 5): the container, the signatures, and an interpreter that runs
   a vertex or pixel shader on four lanes at once (a 2x2 pixel quad, so derivatives work, or four vertices). */

#include <synth/types.h>

#define DXBC_MAX_SIG 32
#define DXBC_MAX_CB 15
#define DXBC_MAX_RES 32

enum dxbc_stage { DXBC_PS, DXBC_VS, DXBC_GS, DXBC_HS, DXBC_DS, DXBC_CS };

struct dxbc_sig_elem {
    char name[24];
    uint32_t index, system_value, component_type, reg, mask, rw_mask;
};

struct dxbc_ins; /* decoded instructions */

struct dxbc_shader {
    enum dxbc_stage stage;
    int major, minor;
    struct dxbc_sig_elem in[DXBC_MAX_SIG], out[DXBC_MAX_SIG];
    int nin, nout;
    struct dxbc_ins *code;
    int ncode;
    int temps;                 /* r# */
    int xtemps[8], xsize[8];   /* x#[size] */
    float *icb;                /* the immediate constant buffer */
    int icb_vec4;
    int max_in_reg, max_out_reg;
    int pos_in_reg;            /* the pixel shader's SV_Position input, or -1 */
    int depth_out;             /* writes oDepth */
    bool discards;
    char error[64];
};

/* what a shader reads: constant buffers, textures through the caller's sampler */
struct dxbc_env {
    const float *cb[DXBC_MAX_CB];
    uint32_t cb_vec4[DXBC_MAX_CB];
    void *ctx;
    /* lod < -1e30: from the derivatives; ddx, ddy: of the coordinates across the quad */
    void (*sample)(void *ctx, int texture, int sampler, const float coord[4], const float ddx[4], const float ddy[4],
                   float lod, float bias, const int offset[3], float out[4]);
    void (*load)(void *ctx, int texture, const int32_t coord[4], const int offset[3], uint32_t out[4]);
    void (*info)(void *ctx, int texture, uint32_t mip, uint32_t out[4]); /* width, height, depth, mips */
};

/* registers of four lanes: [register][lane][component], 32-bit values that are floats or integers */
struct dxbc_lanes {
    uint32_t (*v)[4][4]; /* inputs */
    uint32_t (*o)[4][4]; /* outputs */
    unsigned live;       /* lanes that run (bit per lane); pixel shaders: discarded lanes are cleared */
    float depth[4];
    char error[48];      /* why it stopped, when dxbc_run says false */
};

bool dxbc_parse(struct dxbc_shader *sh, const void *blob, size_t size);
void dxbc_free(struct dxbc_shader *sh);
/* runs the shader on the live lanes; false and lanes->error on a fault */
bool dxbc_run(const struct dxbc_shader *sh, const struct dxbc_env *env, struct dxbc_lanes *lanes);

#endif
