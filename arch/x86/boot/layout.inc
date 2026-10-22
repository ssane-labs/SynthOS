; Disk and memory layout shared by the boot stages.

STAGE2_ADDR     equ 0x7e00
STAGE2_SECTORS  equ 16
KERNEL_LBA      equ 1 + STAGE2_SECTORS
KERNEL_ADDR     equ 0x100000
BUFFER_SEG      equ 0x1000
BUFFER_ADDR     equ 0x10000
CHUNK_SECTORS   equ 64
BOOTINFO_ADDR   equ 0x4f00
MMAP_ADDR       equ 0x5000
MMAP_MAX        equ 64
BOOT_MAGIC      equ 0x5359424f
KERNEL_MAGIC    equ 0x4b4e5953
ESP_LBA         equ 8192
ESP_SECTORS     equ 8192
DATA_LBA        equ ESP_LBA + ESP_SECTORS
INSTALL_RECORD  equ 416
BOOT_SYSTEM     equ 2

%ifndef IMAGE_SECTORS
IMAGE_SECTORS   equ 8192
%endif
