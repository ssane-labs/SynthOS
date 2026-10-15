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
QEMU     ?= qemu-system-x86_64

ARCH_FLAGS := $(if $(CROSS),,-m32) -march=i486 -mtune=i686 -mno-sse -mno-mmx -mno-80387
CFLAGS := $(ARCH_FLAGS) -std=gnu11 -O2 -ffreestanding -fno-builtin -fno-stack-protector \
          -fno-pic -fno-pie -fno-strict-aliasing -fno-tree-loop-distribute-patterns \
          -fno-asynchronous-unwind-tables -fno-omit-frame-pointer \
          -Wall -Wextra -Wno-override-init -Wno-unused-parameter \
          -Iinclude $(CONFIG_DEFINES) $(if $(SERIAL),-DCONFIG_SERIAL_CONSOLE)
LDFLAGS := $(if $(CROSS),,-m elf_i386) -nostdlib -T arch/x86/linker.ld $(shell $(LD) --help 2>/dev/null | grep -q no-warn-rwx && echo --no-warn-rwx-segments)
HOSTCFLAGS := -std=gnu11 -O2 -Wall -Wextra -Iinclude -DSYNTH_TOOL

KERNEL_SRC := $(shell find kernel -name '*.c') $(wildcard arch/x86/*.c)
ifeq ($(CONFIG_DOS),0)
KERNEL_SRC := $(filter-out kernel/dos/%,$(KERNEL_SRC))
endif
ifeq ($(CONFIG_SXVM),0)
KERNEL_SRC := $(filter-out kernel/vm/%,$(KERNEL_SRC))
endif
ifeq ($(CONFIG_WIN32),0)
KERNEL_SRC := $(filter-out kernel/win32/%,$(KERNEL_SRC))
endif
ifeq ($(CONFIG_GUI),0)
KERNEL_SRC := $(filter-out kernel/gui/%,$(KERNEL_SRC))
endif
ifeq ($(CONFIG_EDITOR),0)
KERNEL_SRC := $(filter-out kernel/apps/edit.c,$(KERNEL_SRC))
endif
KERNEL_ASM := arch/x86/entry.asm arch/x86/isr.asm arch/x86/win32.asm arch/x86/long.asm
KERNEL_OBJ := $(KERNEL_SRC:%.c=$(OUT)/obj/%.o) $(KERNEL_ASM:%.asm=$(OUT)/obj/%.o) $(OUT)/obj/ramdisk.o $(OUT)/obj/biosblob.o

SX_SRC   := $(wildcard programs/sx/*.sasm)
SX_BIN   := $(SX_SRC:programs/sx/%.sasm=$(BUILD)/rootfs/bin/%.sx)
DOS_SRC  := $(wildcard programs/dos/*.asm)
EXE_SRC  := $(wildcard programs/dos/exe/*.asm)
DOS_BIN  := $(DOS_SRC:programs/dos/%.asm=$(BUILD)/rootfs/dos/%.com) $(EXE_SRC:programs/dos/exe/%.asm=$(BUILD)/rootfs/dos/%.exe)
W32CC    ?= $(shell command -v i686-w64-mingw32-gcc 2>/dev/null)
W32CXX   ?= $(if $(W32CC),$(W32CC:%gcc=%g++))
W32_SRC  := $(wildcard programs/win32/*.c programs/win32/*.cpp)
W32_DLL  := $(patsubst programs/win32/dll/%.c,$(BUILD)/rootfs/apps/%.dll,$(wildcard programs/win32/dll/*.c))
W32_BIN  := $(patsubst programs/win32/%,$(BUILD)/rootfs/apps/%.exe,$(basename $(W32_SRC))) $(W32_DLL)
W64CC    ?= $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
W64CXX   ?= $(shell command -v x86_64-w64-mingw32-g++ 2>/dev/null)
W64_SRC  := $(wildcard programs/win64/*.c programs/win64/*.cpp)
W32_BIN  += $(patsubst programs/win64/%,$(BUILD)/rootfs/apps/%.exe,$(basename $(W64_SRC))) $(BUILD)/rootfs/apps/hello64u.exe
ROOTFS_SRC := $(shell find rootfs -type f)

TOOLS := $(BUILD)/tools/mkrd $(BUILD)/tools/sasm
SASM_SRC := tools/sasm.c kernel/vm/asm.c kernel/vm/opcodes.c kernel/lib/strutil.c kernel/lib/fmt.c

.DELETE_ON_ERROR:
.PHONY: all run run-serial run-uefi test test-hw test-crypto clean tools programs
all: $(IMAGE)

ESP_LBA     := 8192
ESP_SECTORS := 8192
EFICC       ?= $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
HAVE_ESP    := $(and $(shell command -v mkfs.fat 2>/dev/null),$(shell command -v mcopy 2>/dev/null))

$(IMAGE): $(OUT)/stage1.bin $(OUT)/stage2.bin $(OUT)/kernel.bin $(if $(HAVE_ESP),$(OUT)/esp.img)
	@echo "  IMAGE   $@"
	@test $$(( $$(wc -c < $(OUT)/kernel.bin) / 512 + 17 )) -le $(ESP_LBA) || (echo "kernel too large for the disk layout"; exit 1)
	@cat $(OUT)/stage1.bin $(OUT)/stage2.bin $(OUT)/kernel.bin > $@
	@dd if=/dev/zero of=$@ bs=512 count=0 seek=$(IMAGE_SECTORS) 2>/dev/null
ifneq ($(HAVE_ESP),)
	@dd if=$(OUT)/esp.img of=$@ bs=512 seek=$(ESP_LBA) conv=notrunc 2>/dev/null
	@echo "  ready   $@ ($$(( $$(wc -c < $(OUT)/kernel.bin) / 1024 )) KB kernel, BIOS + UEFI)"
else
	@echo "  ready   $@ ($$(( $$(wc -c < $(OUT)/kernel.bin) / 1024 )) KB kernel, BIOS only: install dosfstools and mtools for UEFI)"
endif

$(OUT)/esp.img: $(OUT)/kernel.bin $(BUILD)/efi/BOOTX64.EFI
	@echo "  ESP     $@"
	@rm -f $@
	@mkfs.fat -F 12 -n SYNTHOS -C $@ $$(( $(ESP_SECTORS) / 2 )) >/dev/null
	@mmd -i $@ ::/EFI ::/EFI/BOOT ::/SYNTHOS
	@mcopy -i $@ $(BUILD)/efi/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	@mcopy -i $@ $(OUT)/kernel.bin ::/SYNTHOS/KERNEL.BIN

$(BUILD)/efi/BOOTX64.EFI: arch/x86/efi/loader.c arch/x86/efi/trampoline.asm arch/x86/efi/blob.asm
	@mkdir -p $(@D)
ifneq ($(EFICC),)
	@echo "  EFI     $@"
	@$(NASM) -f bin -o $(BUILD)/efi/trampoline.bin arch/x86/efi/trampoline.asm
	@$(NASM) -f win64 -DTRAMPOLINE_FILE='"$(BUILD)/efi/trampoline.bin"' -o $(BUILD)/efi/blob.o arch/x86/efi/blob.asm
	@$(EFICC) -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone \
	    -nostdlib -shared -Wl,--subsystem,10 -e efi_main -o $@ arch/x86/efi/loader.c $(BUILD)/efi/blob.o
	@mkdir -p arch/x86/efi/prebuilt && cp $@ arch/x86/efi/prebuilt/
else
	@echo "  COPY    arch/x86/efi/prebuilt/BOOTX64.EFI"
	@cp arch/x86/efi/prebuilt/BOOTX64.EFI $@
endif

$(OUT)/stage1.bin: arch/x86/boot/stage1.asm arch/x86/boot/layout.inc config.mk
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -Iarch/x86/boot/ -DIMAGE_SECTORS=$(IMAGE_SECTORS) $(BOOT_DEFS) -o $@ $<

$(OUT)/stage2.bin: arch/x86/boot/stage2.asm arch/x86/boot/layout.inc
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -Iarch/x86/boot/ $(BOOT_DEFS) -o $@ $<

$(OUT)/kernel.bin: $(OUT)/kernel.elf
	@$(OBJCOPY) -O binary $< $@

$(OUT)/kernel.elf: $(KERNEL_OBJ) arch/x86/linker.ld
	@echo "  LD      $@"
	@$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJ)

$(OUT)/obj/%_fp.o: CFLAGS += -m80387 -mfpmath=387
$(OUT)/obj/kernel/win32/%.o: CFLAGS += -Os
$(OUT)/obj/kernel/win32/crt/%.o: CFLAGS += -O2
$(OUT)/obj/kernel/win32/thread.o $(OUT)/obj/kernel/win32/process.o $(OUT)/obj/kernel/win32/files.o $(OUT)/obj/kernel/win32/pe.o: CFLAGS += -O2

$(OUT)/obj/%.o: %.c
	@mkdir -p $(@D)
	@echo "  CC      $<"
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OUT)/obj/%.o: %.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f elf32 -o $@ $<

$(OUT)/biosthunk.bin: arch/x86/biosthunk.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -o $@ $<

$(OUT)/obj/biosblob.o: arch/x86/biosblob.asm $(OUT)/biosthunk.bin
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f elf32 -DTHUNK_FILE='"$(OUT)/biosthunk.bin"' -o $@ $<

$(OUT)/obj/ramdisk.o: arch/x86/ramdisk.asm $(BUILD)/ramdisk.bin
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f elf32 -DRAMDISK_FILE='"$(BUILD)/ramdisk.bin"' -o $@ $<

$(BUILD)/ramdisk.bin: $(BUILD)/tools/mkrd $(ROOTFS_SRC) $(SX_BIN) $(DOS_BIN) $(W32_BIN) $(wildcard programs/sx/*)
	@mkdir -p $(BUILD)/rootfs
	@cp -r rootfs/. $(BUILD)/rootfs/
	@mkdir -p $(BUILD)/rootfs/home/src
	@cp programs/sx/*.sasm programs/sx/*.inc $(BUILD)/rootfs/home/src/
	@$(BUILD)/tools/mkrd $(BUILD)/rootfs $@

$(BUILD)/rootfs/bin/%.sx: programs/sx/%.sasm programs/sx/synth.inc $(BUILD)/tools/sasm
	@mkdir -p $(@D)
	@echo "  SASM    $<"
	@$(BUILD)/tools/sasm -o $@ $<

$(BUILD)/rootfs/dos/%.com: programs/dos/%.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -o $@ $<

$(BUILD)/rootfs/dos/%.exe: programs/dos/exe/%.asm
	@mkdir -p $(@D)
	@echo "  NASM    $<"
	@$(NASM) -f bin -o $@ $<

$(BUILD)/rootfs/apps/windll.exe: $(BUILD)/rootfs/apps/greet.dll

$(BUILD)/rootfs/apps/%.dll: programs/win32/dll/%.c
	@mkdir -p $(@D)
ifneq ($(W32CC),)
	@echo "  W32CC   $<"
	@$(W32CC) -Os -s -shared -o $@ $<
	@mkdir -p programs/win32/prebuilt && cp $@ programs/win32/prebuilt/
else
	@echo "  COPY    programs/win32/prebuilt/$(@F)"
	@cp programs/win32/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/%.exe: programs/win32/%.c
	@mkdir -p $(@D)
ifneq ($(W32CC),)
	@echo "  W32CC   $<"
	@$(if $(wildcard programs/win32/$*.rc),$(W32CC:%gcc=%windres) -O coff -o $(BUILD)/$*.res.o programs/win32/$*.rc)
	@$(W32CC) -Os -s -o $@ $< $(filter %.dll,$^) $(if $(wildcard programs/win32/$*.rc),$(BUILD)/$*.res.o) -lgdi32 -lws2_32 -lwininet -lwinhttp -lurlmon -lcrypt32
	@mkdir -p programs/win32/prebuilt && cp $@ programs/win32/prebuilt/
else
	@echo "  COPY    programs/win32/prebuilt/$(@F)"
	@cp programs/win32/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/hello64u.exe: programs/win64/hello64.c
	@mkdir -p $(@D)
ifneq ($(W64CC),)
	@echo "  W64CC   $< (UCRT)"
	@$(W64CC) -dumpspecs | sed 's/-lmsvcrt/-lucrt/g' > $(BUILD)/ucrt64.specs
	@$(W64CC) -specs=$(BUILD)/ucrt64.specs -D_UCRT -O2 -s -o $@ $< -lws2_32
	@mkdir -p programs/win64/prebuilt && cp $@ programs/win64/prebuilt/
else
	@echo "  COPY    programs/win64/prebuilt/$(@F)"
	@cp programs/win64/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/%.exe: programs/win64/%.c
	@mkdir -p $(@D)
ifneq ($(W64CC),)
	@echo "  W64CC   $<"
	@$(W64CC) -O2 -s -o $@ $< -lgdi32 -lws2_32 -lwininet -lwinhttp -lurlmon
	@mkdir -p programs/win64/prebuilt && cp $@ programs/win64/prebuilt/
else
	@echo "  COPY    programs/win64/prebuilt/$(@F)"
	@cp programs/win64/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/%.exe: programs/win64/%.cpp
	@mkdir -p $(@D)
ifneq ($(W64CXX),)
	@echo "  W64CXX  $<"
	@$(W64CXX) -Os -s -static -o $@ $<
	@mkdir -p programs/win64/prebuilt && cp $@ programs/win64/prebuilt/
else
	@echo "  COPY    programs/win64/prebuilt/$(@F)"
	@cp programs/win64/prebuilt/$(@F) $@
endif

$(BUILD)/rootfs/apps/%.exe: programs/win32/%.cpp
	@mkdir -p $(@D)
ifneq ($(W32CC),)
	@echo "  W32CXX  $<"
	@$(W32CXX) -Os -s -static -o $@ $<
	@mkdir -p programs/win32/prebuilt && cp $@ programs/win32/prebuilt/
else
	@echo "  COPY    programs/win32/prebuilt/$(@F)"
	@cp programs/win32/prebuilt/$(@F) $@
endif

$(BUILD)/tools/mkrd: tools/mkrd.c
	@mkdir -p $(@D)
	@echo "  HOSTCC  $<"
	@$(HOSTCC) $(HOSTCFLAGS) -o $@ $<

$(BUILD)/tools/sasm: $(SASM_SRC) $(wildcard include/synth/*.h)
	@mkdir -p $(@D)
	@echo "  HOSTCC  tools/sasm"
	@$(HOSTCC) $(HOSTCFLAGS) -o $@ $(SASM_SRC)

tools: $(TOOLS)
programs: $(SX_BIN) $(DOS_BIN) $(W32_BIN)

run: $(IMAGE)
	$(QEMU) -drive format=raw,file=$(IMAGE) -m 256 $(QEMU_FLAGS)

run-serial:
	@$(MAKE) --no-print-directory SERIAL=1
	$(QEMU) -drive format=raw,file=$(BUILD)/serial/synthos.img -m 256 -nographic $(QEMU_FLAGS)

test:
	@$(MAKE) --no-print-directory SERIAL=1
	@tests/run.sh $(BUILD)/serial/synthos.img

test-crypto:
	@mkdir -p $(BUILD)/tools
	@$(HOSTCC) -O2 -Iinclude -o $(BUILD)/tools/cryptotest tests/crypto/driver.c kernel/crypto/hash.c kernel/crypto/aes.c \
	    kernel/crypto/chacha.c kernel/crypto/x25519.c kernel/crypto/pk.c kernel/lib/sha256.c
	@python3 -I tests/crypto/check.py $(BUILD)/tools/cryptotest

test-hw:
	@$(MAKE) --no-print-directory SERIAL=1
	@tests/hw.sh $(BUILD)/serial/synthos.img

OVMF ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd))

run-uefi: $(IMAGE)
	qemu-system-x86_64 -bios $(OVMF) -drive format=raw,file=$(IMAGE) -m 256 $(QEMU_FLAGS)

clean:
	rm -rf $(BUILD)

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
