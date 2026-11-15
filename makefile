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

# Must match SECTORS_TO_LOAD in boot/boot.asm.
KERNEL_SECTORS_LOADED = 1000

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
ASM_SRC = boot/loader.asm interrupts/interrupt.asm os/task_switch.asm

OBJ = $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SRC)) \
      $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRC))

# ── Build ────────────────────────────────────────────────────────────────────
all: os_image.bin
	@KSIZE=$$(wc -c < kernel.bin); \
	 MAX=$$(($(KERNEL_SECTORS_LOADED) * 512)); \
	 echo ""; \
	 echo "Kernel size  : $$KSIZE bytes"; \
	 echo "Max loadable : $$MAX bytes  ($(KERNEL_SECTORS_LOADED) sectors)"; \
	 if [ $$KSIZE -gt $$MAX ]; then \
	   echo "*** ERROR: kernel too large — increase sector count in boot/boot.asm ***"; \
	   exit 1; \
	 fi; \
	 echo ""

kernel.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

%.bin: boot/%.asm
	$(AS) -f bin $< -o $@

os_image.bin: boot.bin boot_stage2.bin kernel.bin
	@# 66 cyl x 16 heads x 63 spt = 66528 sectors: a whole number of
	@# cylinders, so emulators/BIOSes that derive the geometry from the
	@# file size (86Box) see all 65536 sectors the FAT16 volume uses.
	dd if=/dev/zero        of=$@ bs=512 count=66528 2>/dev/null
	dd if=boot.bin         of=$@ bs=512 seek=0 conv=notrunc 2>/dev/null
	dd if=boot_stage2.bin  of=$@ bs=512 seek=1 conv=notrunc 2>/dev/null
	dd if=kernel.bin       of=$@ bs=512 seek=9 conv=notrunc 2>/dev/null

-include $(OBJ:.o=.d)

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
	qemu-system-i386 $(QEMU_FLAGS) -drive format=raw,file=floppy.img,if=floppy

bochs: all
	bochs -f bochs/bochssrc.txt -q

# ── Misc ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)
	rm -f *.bin *.log *.map qemulog.txt floppy.img

kernelmap: $(OBJ)
	$(LD) $(LDFLAGS) -Map=kernel.map -o kernel.bin $^

bloat: $(OBJ)
	$(NM) --print-size --size-sort --radix=d $(OBJ) | awk '$$3 ~ /[bBcC]/' | sort -k2 -n -r | head -30

.PHONY: all qemu qemu-fdd bochs clean kernelmap bloat
