#ifndef SYNTH_DXBC_H
#define SYNTH_DXBC_H

/* Direct3D shader bytecode (DXBC, Shader Model 4 and 5): the container, the signatures, and an interpreter that runs
   a vertex or pixel shader on four lanes at once (a 2x2 pixel quad, so derivatives work, or four vertices). */

#include <synth/types.h>

#define DXBC_MAX_SIG 32
#define DXBC_MAX_CB 15
#define DXBC_MAX_RES 32
#define DXBC_MAX_UAV 8
#define DXBC_MAX_TGSM 16

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
    uint8_t interp[32];        /* pixel shader inputs: D3D_INTERPOLATION_MODE (1 constant, 2 linear, 4 noperspective...) */
    bool discards;
    /* compute shaders */
    uint32_t group[3];                           /* threads per group */
    uint32_t tgsm_off[DXBC_MAX_TGSM], tgsm_size[DXBC_MAX_TGSM], tgsm_stride[DXBC_MAX_TGSM], tgsm_bytes;
    uint8_t uav_kind[DXBC_MAX_UAV];              /* 0 typed, 1 raw, 2 structured */
    uint32_t uav_stride[DXBC_MAX_UAV], srv_stride[DXBC_MAX_RES];
    bool syncs;                                  /* has sync instructions that wait for the group */
    char error[64];
};

/* what a shader reads: constant buffers, textures through the caller's sampler */
struct dxbc_env {
    const float *cb[DXBC_MAX_CB];
    uint32_t cb_vec4[DXBC_MAX_CB];
    void *ctx;
    /* lod < -1e30: from the derivatives; ddx, ddy: of the coordinates across the quad; ref: the value a comparison
       sampler compares every texel with (sample_c), or NULL */
    void (*sample)(void *ctx, int texture, int sampler, const float coord[4], const float ddx[4], const float ddy[4],
                   float lod, float bias, const int offset[3], const float *ref, float out[4]);
    /* the four texels bilinear filtering would use, one component of each (gather4) */
    void (*gather)(void *ctx, int texture, int sampler, const float coord[4], const int offset[3], int component,
                   const float *ref, float out[4]);
    void (*load)(void *ctx, int texture, const int32_t coord[4], const int offset[3], uint32_t out[4]);
    void (*info)(void *ctx, int texture, uint32_t mip, uint32_t out[4]); /* width, height, depth, mips */
    /* raw and structured buffers: the bytes a view looks at (uav false: t#, true: u#), or NULL */
    uint8_t *(*buffer)(void *ctx, bool uav, int slot, uint32_t *size);
    /* typed unordered access views: one element at coord (x, y, z/slice); ptr: its 32 bits for atomics, or NULL */
    void (*uav_load)(void *ctx, int uav, const uint32_t coord[4], uint32_t out[4]);
    void (*uav_store)(void *ctx, int uav, const uint32_t coord[4], const uint32_t v[4], unsigned mask);
    uint32_t *(*uav_ptr)(void *ctx, int uav, const uint32_t coord[4]);
    void (*uav_info)(void *ctx, int uav, uint32_t out[4]);
    uint32_t *(*uav_counter)(void *ctx, int uav); /* the hidden counter of append and consume buffers */
    uint8_t *tgsm;                                /* the group's shared memory: shader->tgsm_bytes */
};

/* registers of four lanes: [register][lane][component], 32-bit values that are floats or integers */
struct dxbc_lanes {
    uint32_t (*v)[4][4]; /* inputs */
    uint32_t (*o)[4][4]; /* outputs */
    unsigned live;       /* lanes that run (bit per lane); pixel shaders: discarded lanes are cleared */
    float depth[4];
    void *scratch;       /* dxbc_scratch(shader) bytes for the registers, or NULL: allocated (not on helper CPUs) */
    char error[48];      /* why it stopped, when dxbc_run says false */
    /* compute shaders: SV_DispatchThreadID, SV_GroupID, SV_GroupThreadID, SV_GroupIndex of each lane */
    uint32_t tid[4][4], gid[4], gtid[4][4], flat[4];
    /* resume: carry on where a sync stopped the last run (same scratch); paused: this run stopped at a sync and
       waits for the rest of the group. Without scratch, syncs do not stop. */
    bool resume, paused;
};

bool dxbc_parse(struct dxbc_shader *sh, const void *blob, size_t size);
void dxbc_free(struct dxbc_shader *sh);
/* the memory one run needs for its temporary registers */
size_t dxbc_scratch(const struct dxbc_shader *sh);
/* runs the shader on the live lanes; false and lanes->error on a fault */
bool dxbc_run(const struct dxbc_shader *sh, const struct dxbc_env *env, struct dxbc_lanes *lanes);

#endif
