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
#define DXBC_MAX_PHASES 16
#define DXBC_MAX_CP 32

enum dxbc_stage { DXBC_PS, DXBC_VS, DXBC_GS, DXBC_HS, DXBC_DS, DXBC_CS };

struct dxbc_sig_elem {
    char name[24];
    uint32_t index, system_value, component_type, reg, mask, rw_mask;
};

struct dxbc_ins; /* decoded instructions */
struct dxbc_jit;

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
    uint8_t uav_dim[DXBC_MAX_UAV], uav_ret[DXBC_MAX_UAV]; /* typed u#: dimension and return type as for t# */
    uint32_t srv_raw;                            /* t# declared raw or structured (bit per slot) */
    bool syncs;                                  /* has sync instructions that wait for the group */
    /* geometry shaders: input primitive (1 point, 2 line, 3 triangle, 6 line_adj, 7 triangle_adj), output topology
       (1 points, 3 line strip, 5 triangle strip), most vertices emitted, instances */
    uint32_t gs_in, gs_out, gs_max, gs_instances;
    /* hull and domain shaders: the patch constants' signature; the hull shader's phases (0 control point, 1 fork,
       2 join: where in code each starts, how many instances); control points in and out; domain (1 isoline,
       2 triangle, 3 quad), partitioning (1 integer, 2 pow2, 3 fractional odd, 4 fractional even), output primitive
       (1 point, 2 line, 3 triangle cw, 4 triangle ccw), the largest tessellation factor */
    struct dxbc_sig_elem pc[DXBC_MAX_SIG];
    int npc;
    struct {
        uint8_t kind;
        int start;
        uint32_t instances;
    } phase[DXBC_MAX_PHASES];
    int nphase;
    uint32_t cp_in, cp_out, domain, partitioning, out_prim;
    float max_tess;
    /* declared resources: t# dimension (D3D10_SB_RESOURCE_DIMENSION, 0 undeclared) and return type of its first
       component (1 unorm, 2 snorm, 3 sint, 4 uint, 5 float); cb# size in vectors (0 undeclared); s# declared with
       mode comparison (bit per sampler), s# declared at all */
    uint8_t res_dim[DXBC_MAX_RES], res_ret[DXBC_MAX_RES];
    uint32_t cb_size[DXBC_MAX_CB];
    uint16_t samp_cmp, samp_used;
    struct dxbc_jit *jit; /* compiled runs of arithmetic, or NULL */
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
    /* the level of detail sampling would use: clamped, and as worked out (lod) */
    void (*lod)(void *ctx, int texture, int sampler, const float coord[4], const float ddx[4], const float ddy[4],
                float out[2]);
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
    unsigned helper;     /* pixel shaders: lanes outside the primitive, run for derivatives only: no memory writes */
    float depth[4];
    void *scratch;       /* dxbc_scratch(shader) bytes for the registers, or NULL: allocated (not on helper CPUs) */
    char error[48];      /* why it stopped, when dxbc_run says false */
    /* compute shaders: SV_DispatchThreadID, SV_GroupID, SV_GroupThreadID, SV_GroupIndex of each lane */
    uint32_t tid[4][4], gid[4], gtid[4][4], flat[4];
    /* resume: carry on where a sync stopped the last run (same scratch); paused: this run stopped at a sync and
       waits for the rest of the group. Without scratch, syncs do not stop. */
    bool resume, paused;
    /* geometry shaders: the input vertices' registers, gsv[(vertex * gsv_regs + reg) * 4 + component]; the primitive
       and the instance; emit: called for emit (with the outputs in o) and cut */
    const uint32_t *gsv;
    int gsv_regs;
    uint32_t prim_id, gs_instance;
    void (*emit)(void *user, struct dxbc_lanes *l, int lane, bool emit, bool cut);
    void *user;
    /* hull and domain shaders: where the run starts (a hull shader phase); vicp[point][reg] comes from gsv,
       vocp[point][reg] from ocp, vpc[reg] from pc (registers of four 32-bit values); each lane's vDomain,
       vOutputControlPointID and fork or join instance */
    int entry;
    const uint32_t *ocp, *pc;
    int ocp_regs;
    float domain[4][4];
    uint32_t cp_id[4], phase_id[4];
};

bool dxbc_parse(struct dxbc_shader *sh, const void *blob, size_t size);
void dxbc_free(struct dxbc_shader *sh);
/* the memory one run needs for its temporary registers */
size_t dxbc_scratch(const struct dxbc_shader *sh);
/* runs the shader on the live lanes; false and lanes->error on a fault */
bool dxbc_run(const struct dxbc_shader *sh, const struct dxbc_env *env, struct dxbc_lanes *lanes);
/* compiling straight runs of arithmetic to machine code when shaders are parsed (on by default); how many
   instructions of a shader were compiled */
void dxbc_jit_enable(bool on);
bool dxbc_jit_enabled(void);
int dxbc_jit_count(const struct dxbc_shader *sh);

/* the shader as TGSI text for the host's renderer (virgl); tex_sampler[t]: the sampler the shader uses with t#,
   which the renderer must have at slot t (0xff: none). False and error when it has no translation. */
struct dxbc_tgsi {
    char *text;
    size_t len;
    uint8_t tex_sampler[DXBC_MAX_RES];
    uint32_t used_cb; /* the constant buffer slots the shader reads, declared or not */
    uint32_t used_uav, used_srv_buf; /* u# and raw or structured t# the shader reaches */
    bool icb;         /* reads the immediate constant buffer, as constant buffer 14 (CONST[15]) */
    char error[64];
};
/* uav_format: the host format (virgl numbering) of the view bound at each typed u#, or NULL / 0 for a 32-bit format of
   the declared return type */
bool dxbc_tgsi(const struct dxbc_shader *sh, const uint16_t *uav_format, struct dxbc_tgsi *out);
void dxbc_tgsi_free(struct dxbc_tgsi *out);

#endif
