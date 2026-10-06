#  SynthOS build
#
#  make            build the bootable disk image (build/synthos.img)
#  make run        boot it in QEMU
#  make test       build a serial-console image and run the automated tests
#  make clean      remove build output

include config.mk

BUILD    ?= build
OUT      := $(BUILD)/$(if $(SERIAL),serial,vga)
IMAGE    := $(OUT)/synthos.img

ifneq ($(shell command -v i686-elf-gcc 2>/dev/null),)
CROSS    ?= i686-elf-
endif

CC        = $(CROSS)gcc
LD        = $(CROSS)ld
OBJCOPY   = $(CROSS)objcopy
NASM     ?= nasm
HOSTCC   ?= cc
QEMU     ?= qemu-system-i386

ARCH_FLAGS := $(if $(CROSS),,-m32) -march=i686 -mno-sse -mno-mmx -mno-80387
CFLAGS := $(ARCH_FLAGS) -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-pic -fno-pie -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
          -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
          -Wall -Wextra -Wno-override-init -Wno-unused-parameter \
          -Iinclude $(CONFIG_DEFINES) $(if $(SERIAL),-DCONFIG_SERIAL_CONSOLE)
LDFLAGS := $(if $(CROSS),,-m elf_i386) -nostdlib -T arch/x86/linker.ld $(shell $(LD) --help 2>/dev/null | grep -q no-warn-rwx && echo --no-warn-rwx-segments)
HOSTCFLAGS := -std=gnu11 -O2 -Wall -Wextra -Iinclude -DSYNTH_TOOL

KERNEL_SRC := $(shell find kernel -name '*.c') $(wildcard arch/x86/*.c)
KERNEL_ASM := arch/x86/entry.asm arch/x86/isr.asm
KERNEL_OBJ := $(KERNEL_SRC:%.c=$(OUT)/obj/%.o) $(KERNEL_ASM:%.asm=$(OUT)/obj/%.o) $(OUT)/obj/ramdisk.o

SX_SRC   := $(wildcard programs/sx/*.sasm)
SX_BIN   := $(SX_SRC:programs/sx/%.sasm=$(BUILD)/rootfs/bin/%.sx)
DOS_SRC  := $(wildcard programs/dos/*.asm)
DOS_BIN  := $(DOS_SRC:programs/dos/%.asm=$(BUILD)/rootfs/dos/%.com)
ROOTFS_SRC := $(shell find rootfs -type f)

TOOLS := $(BUILD)/tools/mkrd $(BUILD)/tools/sasm
SASM_SRC := tools/sasm.c kernel/vm/asm.c kernel/vm/opcodes.c kernel/lib/strutil.c kernel/lib/fmt.c

.PHONY: all run run-serial test clean tools programs
all: $(IMAGE)

$(IMAGE): $(OUT)/stage1.bin $(OUT)/stage2.bin $(OUT)/kernel.bin
	@echo "  IMAGE   $@"
	@cat $^ > $@
	@truncate -s $$(( $(IMAGE_SECTORS) * 512 )) $@
	@echo "  ready   $@ ($$(( $$(stat -c %s $(OUT)/kernel.bin) / 1024 )) KB kernel)"

$(OUT)/stage1.bin: arch/x86/boot/stage1.asm arch/x86/boot/layout.inc
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -Iarch/x86/boot/ -DIMAGE_SECTORS=$(IMAGE_SECTORS) -o $@ $<

$(OUT)/stage2.bin: arch/x86/boot/stage2.asm arch/x86/boot/layout.inc
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -Iarch/x86/boot/ -o $@ $<

$(OUT)/kernel.bin: $(OUT)/kernel.elf
	@$(OBJCOPY) -O binary $< $@

$(OUT)/kernel.elf: $(KERNEL_OBJ) arch/x86/linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJ)

$(OUT)/obj/%.o: %.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/obj/%.o: %.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f elf32 -o $@ $<

$(OUT)/obj/ramdisk.o: arch/x86/ramdisk.asm $(BUILD)/ramdisk.bin
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f elf32 -DRAMDISK_FILE='"$(BUILD)/ramdisk.bin"' -o $@ $<

$(BUILD)/ramdisk.bin: $(BUILD)/tools/mkrd $(ROOTFS_SRC) $(SX_BIN) $(DOS_BIN)
	@mkdir -p $(BUILD)/rootfs
	@cp -r rootfs/. $(BUILD)/rootfs/
	@$(BUILD)/tools/mkrd $(BUILD)/rootfs $@

$(BUILD)/rootfs/bin/%.sx: programs/sx/%.sasm $(BUILD)/tools/sasm
	@mkdir -p $(@D)
	@echo "  SASM    $<"
	@$(BUILD)/tools/sasm -o $@ $<

$(BUILD)/rootfs/dos/%.com: programs/dos/%.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -o $@ $<

$(BUILD)/tools/mkrd: tools/mkrd.c
	@mkdir -p $(@D)
	@echo "  HOSTCC  $<"
	@$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

$(BUILD)/tools/sasm: $(SASM_SRC) $(wildcard include/synth/*.h)
	@mkdir -p $(@D)
	@echo "  HOSTCC  tools/sasm"
	@$(HOSTCC) $(HOSTCFLAGS) -o $@ $(SASM_SRC)

tools: $(TOOLS)
programs: $(SX_BIN) $(DOS_BIN)

run: $(IMAGE)
	$(QEMU) -drive format=raw,file=$(IMAGE) -m 64 $(QEMU_FLAGS)

run-serial:
	@$(MAKE) --no-print-directory SERIAL=1
	$(QEMU) -drive format=raw,file=$(BUILD)/serial/synthos.img -m 64 -nographic $(QEMU_FLAGS)

test:
	@$(MAKE) --no-print-directory SERIAL=1
	@tests/run.sh $(BUILD)/serial/synthos.img

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
