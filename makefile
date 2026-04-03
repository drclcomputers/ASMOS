CC = i686-elf-gcc
LD = i686-elf-ld
AS = nasm

CFLAGS  = -std=gnu11 -ffreestanding -m32 -O2 -Wall -Wextra \
          -fno-stack-protector -fno-pie -nostdlib \
          -fno-builtin-memset -fno-builtin-memcpy \
          -I. -c
LDFLAGS = -m elf_i386 -Ttext 0x8000 --oformat binary

BUILD_DIR = build

OBJ = $(BUILD_DIR)/loader.o                     \
      $(BUILD_DIR)/kernel.o                     \
      \
      $(BUILD_DIR)/os/os.o                      \
	  \
	  $(BUILD_DIR)/interrupts/interrupt.o  		\
      $(BUILD_DIR)/interrupts/idt.o        		\
      \
      $(BUILD_DIR)/shell/cli.o                  \
      \
	  $(BUILD_DIR)/apps/desktop.o               \
      $(BUILD_DIR)/apps/filefinder.o            \
      $(BUILD_DIR)/apps/terminal.o              \
	  $(BUILD_DIR)/apps/monitor.o               \
      \
	  $(BUILD_DIR)/config/config_enduser.o      \
      $(BUILD_DIR)/config/config.o              \
      \
      $(BUILD_DIR)/fonts/fonts.o                \
      \
      $(BUILD_DIR)/fs/ata.o                     \
      $(BUILD_DIR)/fs/fat16.o                   \
      \
      $(BUILD_DIR)/io/ps2.o                     \
      $(BUILD_DIR)/io/mouse.o                   \
      $(BUILD_DIR)/io/keyboard.o                \
      \
      $(BUILD_DIR)/lib/alloc.o                  \
      $(BUILD_DIR)/lib/io.o                     \
      $(BUILD_DIR)/lib/math.o                   \
      $(BUILD_DIR)/lib/mem.o                    \
      $(BUILD_DIR)/lib/primitive_graphics.o     \
      $(BUILD_DIR)/lib/string.o                 \
	  $(BUILD_DIR)/lib/time.o                   \
      $(BUILD_DIR)/lib/types.o                  \
      \
      $(BUILD_DIR)/ui/cursor.o                  \
      $(BUILD_DIR)/ui/menubar.o                 \
      $(BUILD_DIR)/ui/ui.o                      \
      $(BUILD_DIR)/ui/widgets.o                 \
      $(BUILD_DIR)/ui/window.o

# boot.asm loads 100 sectors (51,200 bytes) starting at KERNEL_OFFSET (0x8000).
# The kernel binary must not exceed this — increase 'al' in boot.asm if needed.
KERNEL_SECTORS_LOADED = 100

all: os_image.bin
	@KSIZE=$$(wc -c < kernel.bin); \
	 MAX=$$(($(KERNEL_SECTORS_LOADED) * 512)); \
	 echo ""; \
	 echo "Kernel size  : $$KSIZE bytes"; \
	 echo "Max loadable : $$MAX bytes  ($(KERNEL_SECTORS_LOADED) sectors)"; \
	 if [ $$KSIZE -gt $$MAX ]; then \
	   echo "*** ERROR: kernel too large — increase sector count in boot.asm ***"; \
	   exit 1; \
	 fi; \
	 echo ""

kernel.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

# Boot sector
$(BUILD_DIR)/loader.o: loader.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/interrupts/interrupt.o: interrupts/interrupt.asm | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(AS) -f elf32 $< -o $@

# All C sources
$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

boot.bin: boot.asm
	$(AS) -f bin $< -o $@

os_image.bin: boot.bin kernel.bin
	dd if=/dev/zero    of=os_image.bin bs=512 count=65536  2>/dev/null
	dd if=boot.bin     of=os_image.bin bs=512 seek=0  conv=notrunc 2>/dev/null
	dd if=kernel.bin   of=os_image.bin bs=512 seek=1  conv=notrunc 2>/dev/null

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

qemu: all
	qemu-system-i386 -drive format=raw,file=os_image.bin \
	                 -m 4M -machine pc

bochs: all
	bochs -f bochs/bochssrc.txt -q

clean:
	rm -rf $(BUILD_DIR)
	rm -f *.bin os_image.bin
