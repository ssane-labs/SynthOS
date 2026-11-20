#ifndef SYNTH_AIBACKEND_H
#define SYNTH_AIBACKEND_H

#include <synth/types.h>

/* A compute backend for the assistant's models: a module that drives a GPU or an NPU offers this as the service
   "ai-backend" (struct module_ext.service_name). When a model is loaded, the assistant hands every matrix to upload;
   the ones the backend keeps (a handle back) are multiplied by it, the rest by the processors. A backend may keep
   only the types and sizes it handles and refuse the rest (NULL): the model still runs. */
struct ai_backend {
    const char *name;  /* shown by ai info, e.g. "Intel Arc A380 (Vulkan)" */
    uint64_t memory;   /* bytes of its own memory, 0 if it shares the system's */
    /* a matrix of rows x cols in the model file's own layout: type is the GGUF type number (0 F32, 1 F16, 8 Q8_0,
       12 Q4_K...), row_bytes the size of one row; data stays valid until release */
    void *(*upload)(const void *data, uint32_t type, int cols, int rows, size_t row_bytes);
    void (*release)(void *matrix);
    /* out[t * out_stride + r] = row r of the matrix . in[t * in_stride ..] for t < batch, r < rows;
       false: not done (busy, lost), the processors do it */
    bool (*matmul)(void *matrix, const float *in, int in_stride, float *out, int out_stride, int batch);
};

#endif
