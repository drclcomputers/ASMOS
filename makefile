CC = i686-elf-gcc
LD = i686-elf-ld
NM = i686-elf-nm
AS = nasm

# -march=i386: runs on 386/486/Pentium (gcc's default i686 emits CMOV,
# which faults on anything older than a Pentium Pro).
CFLAGS  = -std=gnu11 -ffreestanding -m32 -march=i386 -mtune=i486 -O2 -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra \
          -fno-stack-protector -fno-pie -nostdlib \
          -fno-builtin-memset -fno-builtin-memcpy \
          -MMD -MP -I. -c
LDFLAGS = -m elf_i386 -T linker.ld --oformat binary

BUILD_DIR = build

# The hard disk image `make` builds (installed and ready, like Setup makes
# it): its size in MB. 64 MB = 130 cylinders of 16 heads x 63 sectors.
HDD_MB ?= 64

# ── Sources ──────────────────────────────────────────────────────────────────
# Every .c in these dirs is built automatically; just drop a new file in.
SRC_DIRS = apps config drivers fonts fs interrupts lib network os shell ui

# Kept in-tree but deliberately not built:
#   apps/asmusic.c      – app retired from the registry (still gitignored WIP)
#   lib/libc_compat.c   – libc shim for future TCC/NASM ports; pulled from the
#                         build in 7d3b781 ("OS now boots"), not needed yet
EXCLUDE = apps/asmusic.c lib/libc_compat.c

C_SRC   = kernel.c $(filter-out $(EXCLUDE),$(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c)))
# boot/loader.asm must stay first (entry point); linker.ld also pins it.
# boot/bootcode.asm: the boot sectors below, inside the kernel (Setup, SYS).
ASM_SRC = boot/loader.asm interrupts/interrupt.asm os/task_switch.asm \
          boot/bootcode.asm

# The boot sectors (flat binaries): MBR, the volume's boot sector, stage 2.
BOOT_DIR = $(BUILD_DIR)/boot
BOOT_BIN = $(BOOT_DIR)/mbr.bin $(BOOT_DIR)/vbr.bin $(BOOT_DIR)/stage2.bin

OBJ = $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SRC)) \
      $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRC))

# ── Bundled apps: .APP packages put in /APPS on the disk image ──────────────
# extapps/<name>.c + extapps/shim.c (kernel calls -> app API) + the string
# and number helpers, linked at 0 with only what is used (--gc-sections),
# packed by tools/mkapp.py (name, state, heap from APP_META). Where each goes
# in /APPS, and the shortcuts to the built-in apps: extapps/MANIFEST.
EXT_APPS    = puzzle sudoku calendar clock calculator clipview hexview asmdraw audioplayer
EXT_DIR     = $(BUILD_DIR)/extapps
EXT_LIB_SRC = extapps/shim.c lib/string.c lib/math.c lib/mem.c
EXT_LIB_OBJ = $(patsubst %.c,$(EXT_DIR)/%.o,$(EXT_LIB_SRC))
EXT_CFLAGS  = $(CFLAGS) -ffunction-sections -fdata-sections
EXT_PKGS    = $(patsubst %,$(EXT_DIR)/%.APP,$(EXT_APPS))

$(EXT_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(EXT_CFLAGS) $< -o $@

$(EXT_DIR)/%.APP: $(EXT_DIR)/extapps/%.o $(EXT_LIB_OBJ)
	$(LD) -m elf_i386 -N -Ttext 0 -nostdlib --emit-relocs --gc-sections \
	      -e init -u on_frame -u destroy -u app_meta -o $(EXT_DIR)/$*.elf $^
	python3 tools/mkapp.py --elf $(EXT_DIR)/$*.elf --meta --out $@ >/dev/null

apps: $(EXT_PKGS)

.DEFAULT_GOAL := all

# ── Build ────────────────────────────────────────────────────────────────────
# os_image.bin  a hard disk with ASMOS installed (the 86Box/QEMU disk)
# setup.img     the 1.44 MB setup floppy: boot it to install on any disk
all: os_image.bin setup.img
	@echo ""
	@echo "Kernel size  : $$(wc -c < kernel.bin) bytes (/SYSTEM/KERNEL.SYS)"
	@echo ""

kernel.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 -I. $< -o $@

$(BOOT_DIR)/%.bin: boot/%.asm boot/bootdefs.inc
	@mkdir -p $(dir $@)
	$(AS) -f bin -I. $< -o $@

$(BUILD_DIR)/boot/bootcode.o: $(BOOT_BIN)

# the FPU emulator is big; below 640 KB size matters more than its speed
$(BUILD_DIR)/lib/fpu_emu.o: CFLAGS += -Os

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

MKDISK = python3 tools/mkdisk.py
MKDISK_IN = $(BOOT_BIN) kernel.bin $(EXT_PKGS) extapps/MANIFEST tools/mkdisk.py

os_image.bin: $(MKDISK_IN)
	$(MKDISK) hdd $@ --mb $(HDD_MB) --boot $(BOOT_DIR) --kernel kernel.bin \
	          --apps extapps/MANIFEST $(EXT_DIR)

setup.img: $(MKDISK_IN)
	$(MKDISK) floppy $@ --boot $(BOOT_DIR) --kernel kernel.bin \
	          --apps extapps/MANIFEST $(EXT_DIR)

-include $(OBJ:.o=.d)
-include $(EXT_LIB_OBJ:.o=.d) $(patsubst %,$(EXT_DIR)/extapps/%.d,$(EXT_APPS))

# ── Run ──────────────────────────────────────────────────────────────────────
QEMU_FLAGS = -drive format=raw,file=os_image.bin \
             -m 4M -machine pc \
             -audiodev coreaudio,id=snd0 \
             -machine pcspk-audiodev=snd0 \
             -device sb16,audiodev=snd0 \
             -device adlib,audiodev=snd0 \
             -vga std \
             -display cocoa,zoom-to-fit=on \
             -device ne2k_isa,irq=3,iobase=0x300,netdev=net0 \
             -netdev user,id=net0

qemu: all
	qemu-system-i386 $(QEMU_FLAGS)

qemu-fdd: all
	qemu-system-i386 $(QEMU_FLAGS) -drive format=raw,file=setup.img,if=floppy

# Setup on a new, empty 100 MB disk, from the setup floppy
qemu-setup: setup.img
	@[ -f blank.img ] || dd if=/dev/zero of=blank.img bs=1M count=100 2>/dev/null
	qemu-system-i386 -drive format=raw,file=blank.img \
	    -drive format=raw,file=setup.img,if=floppy -boot a -m 4M -vga std

bochs: all
	bochs -f bochs/bochssrc.txt -q

# boot both images in tools/bootemu.py (an x86 + BIOS emulator) under many
# BIOS kinds, and broken ones: checks the boot code without a real machine
boottest: os_image.bin setup.img
	python3 tools/boottest.py os_image.bin setup.img kernel.bin

# ── Misc ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)
	rm -f *.bin *.log *.map qemulog.txt floppy.img setup.img

kernelmap: $(OBJ)
	$(LD) $(LDFLAGS) -Map=kernel.map -o kernel.bin $^

bloat: $(OBJ)
	$(NM) --print-size --size-sort --radix=d $(OBJ) | awk '$$3 ~ /[bBcC]/' | sort -k2 -n -r | head -30

.PHONY: all apps qemu qemu-fdd qemu-setup boottest bochs clean kernelmap bloat
