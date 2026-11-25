CC = i686-elf-gcc
LD = i686-elf-ld
NM = i686-elf-nm
AS = nasm

CFLAGS  = -std=gnu11 -ffreestanding -m32 -march=i386 -mtune=i486 -O2 -fno-asynchronous-unwind-tables -fno-unwind-tables -Wall -Wextra \
          -fno-stack-protector -fno-pie -nostdlib \
          -fno-builtin-memset -fno-builtin-memcpy \
          -MMD -MP -I. -c
LDFLAGS = -m elf_i386 -T linker.ld --oformat binary

BUILD_DIR = build

HDD_MB ?= 64

# Sources
# Every .c in these dirs is built automatically; just drop a new file in.
SRC_DIRS = apps config drivers fonts fs interrupts lib network os shell ui

EXCLUDE = apps/asmusic.c lib/libc_compat.c

C_SRC   = kernel.c $(filter-out $(EXCLUDE),$(foreach d,$(SRC_DIRS),$(wildcard $(d)/*.c)))
# boot/loader.asm must stay first (entry point); linker.ld also pins it.
# boot/bootcode.asm: the boot sectors below, inside the kernel (Setup, SYS).
ASM_SRC = boot/loader.asm interrupts/interrupt.asm os/task_switch.asm \
          boot/bootcode.asm lib/realmode.asm

# The boot sectors (flat binaries): MBR, the volume's boot sector, stage 2.
BOOT_DIR = $(BUILD_DIR)/boot
BOOT_BIN = $(BOOT_DIR)/mbr.bin $(BOOT_DIR)/vbr.bin $(BOOT_DIR)/stage2.bin

OBJ = $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SRC)) \
      $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRC))

# Bundled apps: .APP packages put in /APPS on the disk image
# extapps/<name>.c + extapps/shim.c (kernel calls -> app API) + the string
# and number helpers, linked at 0 with only what is used (--gc-sections),
# packed by tools/mkapp.py (name, state, heap from APP_META). Where each goes
# in /APPS, and the shortcuts to the built-in apps: extapps/MANIFEST.
EXT_APPS    = puzzle sudoku solitaire calendar clock calculator clipview hexview asmdraw audioplayer bmpview
EXT_DIR     = $(BUILD_DIR)/extapps
EXT_LIB_SRC = extapps/shim.c extapps/shim_api.c lib/string.c lib/math.c lib/mem.c
EXT_LIB_OBJ = $(patsubst %.c,$(EXT_DIR)/%.o,$(EXT_LIB_SRC))
EXT_CFLAGS  = $(CFLAGS) -ffunction-sections -fdata-sections
EXT_PKGS    = $(patsubst %,$(EXT_DIR)/%.APP,$(EXT_APPS))

$(EXT_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(EXT_CFLAGS) $< -o $@

# an app's help pages, if it has any: extapps/help/<name>/ (carried in the
# .APP, copied to /HELP the first time it runs: os/help.c)
EXT_HELP    = $(wildcard extapps/help/*/*)

$(EXT_DIR)/%.APP: $(EXT_DIR)/extapps/%.o $(EXT_LIB_OBJ) $(EXT_HELP)
	$(LD) -m elf_i386 -N -Ttext 0 -nostdlib --emit-relocs --gc-sections \
	      -e init -u on_frame -u destroy -u app_meta -o $(EXT_DIR)/$*.elf \
	      $(EXT_DIR)/extapps/$*.o $(EXT_LIB_OBJ)
	python3 tools/mkapp.py --elf $(EXT_DIR)/$*.elf --meta --out $@ \
	      $(if $(wildcard extapps/help/$*),--help-dir extapps/help/$*) >/dev/null

apps: $(EXT_PKGS)

# SDK: building apps without the source tree (tools/mksdk.py, sdk/)
# build/sdk/ASMOS-SDK (and .zip): headers, libasmos.a (the objects above),
# mkapp.py, mkiso.py, app.mk, an example. Not part of `all`: make sdk.
SDK_DIR = build/sdk/ASMOS-SDK

sdk: $(EXT_LIB_OBJ) tools/mksdk.py
	python3 tools/mksdk.py --out $(SDK_DIR) --cc "$(CC)" --ld "$(LD)" \
	      --cflags "$(EXT_CFLAGS)" $(EXT_LIB_OBJ)
	cd build/sdk && rm -f ASMOS-SDK.zip && python3 -m zipfile -c ASMOS-SDK.zip ASMOS-SDK
	@echo "SDK zip: build/sdk/ASMOS-SDK.zip"

.DEFAULT_GOAL := all

# Build
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
# BIOS calls from the kernel: the real-mode half, copied to 0x1000
$(BUILD_DIR)/lib/realmode.o: $(BOOT_DIR)/rmstub.bin boot/rmdefs.inc
$(BOOT_DIR)/rmstub.bin: boot/rmdefs.inc

# the FPU emulator is big; below 640 KB size matters more than its speed
$(BUILD_DIR)/lib/fpu_emu.o: CFLAGS += -Os

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

MKDISK = python3 tools/mkdisk.py
# help/: the built-in apps' help pages (and ASMOS's), to /HELP
HELP_PAGES = $(wildcard help/*/*)
MKDISK_IN = $(BOOT_BIN) kernel.bin $(EXT_PKGS) extapps/MANIFEST tools/mkdisk.py \
            $(HELP_PAGES)

os_image.bin: $(MKDISK_IN)
	$(MKDISK) hdd $@ --mb $(HDD_MB) --boot $(BOOT_DIR) --kernel kernel.bin \
	          --apps extapps/MANIFEST $(EXT_DIR) --help help

setup.img: $(MKDISK_IN)
	$(MKDISK) floppy $@ --boot $(BOOT_DIR) --kernel kernel.bin \
	          --apps extapps/MANIFEST $(EXT_DIR) --help help

-include $(OBJ:.o=.d)
-include $(EXT_LIB_OBJ:.o=.d) $(patsubst %,$(EXT_DIR)/extapps/%.d,$(EXT_APPS))

# Run
QEMU_FLAGS = -drive format=raw,file=os_image.bin \
             -m 1.8M -machine pc \
             -audiodev coreaudio,id=snd0 \
             -machine pcspk-audiodev=snd0 \
             -device sb16,audiodev=snd0 \
             -device adlib,audiodev=snd0 \
             -vga std \
             -display cocoa,zoom-to-fit=on \
             -device ne2k_isa,irq=3,iobase=0x300,netdev=net0 \
             -netdev user,id=net0$(FTP_FWD)
# make qemu FTP=1: the Mac reaches ASMOS's FTP server (ftp startsrv) at
# localhost:2121, and its data ports 50000-50009
FTP_DATA = 50000 50001 50002 50003 50004 50005 50006 50007 50008 50009
FTP_FWD = $(if $(FTP),$(comma)hostfwd=tcp::2121-:21$(subst $(space),,$(foreach \
          p,$(FTP_DATA),$(comma)hostfwd=tcp::$(p)-:$(p))))
comma := ,
space := $(subst ,, )

qemu: all
	qemu-system-i386 $(QEMU_FLAGS)

qemu-fdd: all
	qemu-system-i386 $(QEMU_FLAGS) -drive format=raw,file=setup.img,if=floppy

# with a CD in the drive (secondary master, ATAPI). By default cd.iso: the
# bundled apps in APPS\ (they show in Apps > CDROM)
# Any other disc: make qemu-cd CD_ISO=my.iso
CD_ISO ?= cd.iso
CD_DIR  = $(BUILD_DIR)/cd

cd.iso: $(EXT_PKGS) tools/mkiso.py
	rm -rf $(CD_DIR)
	mkdir -p $(CD_DIR)/APPS
	cp $(EXT_PKGS) $(CD_DIR)/APPS/
	printf 'This file is on the ASMOS test CD.\r\n' > "$(CD_DIR)/A file with a long name.txt"
	python3 tools/mkiso.py $(CD_DIR) $@ --label "ASMOS apps"

qemu-cd: all $(CD_ISO)
	qemu-system-i386 $(QEMU_FLAGS) -cdrom $(CD_ISO)

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
	python3 tools/boottest.py os_image.bin setup.img kernel.bin build/lib/realmode.o $(LD)

# Misc
clean:
	rm -rf $(BUILD_DIR)
	rm -f *.bin *.log *.map qemulog.txt floppy.img setup.img cd.iso

kernelmap: $(OBJ)
	$(LD) $(LDFLAGS) -Map=kernel.map -o kernel.bin $^

bloat: $(OBJ)
	$(NM) --print-size --size-sort --radix=d $(OBJ) | awk '$$3 ~ /[bBcC]/' | sort -k2 -n -r | head -30

.PHONY: all apps sdk qemu qemu-fdd qemu-cd qemu-setup boottest bochs clean kernelmap bloat
