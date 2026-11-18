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

#endif
