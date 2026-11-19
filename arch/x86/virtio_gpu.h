#ifndef ARCH_X86_VIRTIO_GPU_H
#define ARCH_X86_VIRTIO_GPU_H

#include <synth/types.h>

struct pci_device;
struct framebuffer;

/* one display of a virtio-gpu and the picture it shows */
struct virtio_screen {
    int gpu, scanout;
    uint32_t resource;
    uint8_t *pixels;
    int w, h;
};

bool virtio_gpu_probe(const struct pci_device *d, int *scanouts, uint16_t *w, uint16_t *h);
int virtio_gpu_last(void);
void virtio_gpu_preferred(int gpu, int scanout, uint16_t *w, uint16_t *h);
bool virtio_gpu_set(int gpu, int scanout, int w, int h, struct virtio_screen *vs, struct framebuffer *fb);
void virtio_gpu_present(struct virtio_screen *vs, int x, int y, int w, int h);
bool virtio_gpu_cursor_image(int gpu, int scanout, const uint32_t *argb, int w, int h, int hot_x, int hot_y, int x,
                             int y);
bool virtio_gpu_cursor_move(int gpu, int scanout, int x, int y, bool show);

/* 3D (virgl): the host renders command streams of a context into resources in its memory */
struct virtio_res3d {
    uint32_t target; /* 0 buffer, 2 2D texture, ... (gallium's pipe_texture_target) */
    uint32_t format; /* virgl format */
    uint32_t bind;   /* VIRGL_BIND_* */
    uint32_t w, h, depth, array, levels, samples;
};

bool virtio_gpu_has_3d(int gpu);
int virtio_gpu_capsets(int gpu, uint32_t (*out)[3], int max);
uint32_t virtio_gpu_ctx_create(int gpu, const char *name);
void virtio_gpu_ctx_destroy(int gpu, uint32_t ctx);
uint32_t virtio_gpu_resource_3d(int gpu, uint32_t ctx, const struct virtio_res3d *r, void *backing, size_t bytes);
void virtio_gpu_resource_unref(int gpu, uint32_t ctx, uint32_t id);
bool virtio_gpu_transfer_3d(int gpu, uint32_t ctx, uint32_t id, bool to_host, uint32_t x, uint32_t y, uint32_t w,
                            uint32_t h, uint32_t stride);
bool virtio_gpu_transfer_box(int gpu, uint32_t ctx, uint32_t id, bool to_host, uint32_t level, const uint32_t box[6],
                             uint64_t offset, uint32_t stride, uint32_t layer_stride, bool wait);
int virtio_gpu_find_3d(void);
bool virtio_gpu_submit_3d(int gpu, uint32_t ctx, const uint32_t *cmd, uint32_t dwords, bool wait);

#endif
