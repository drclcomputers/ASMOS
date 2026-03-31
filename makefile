CC = i686-elf-gcc
LD = i686-elf-ld
AS = nasm

CFLAGS  = -std=gnu11 -ffreestanding -m32 -O2 -Wall -Wextra \
          -fno-stack-protector -fno-pie -nostdlib \
          -fno-builtin-memset -fno-builtin-memcpy \
          -I. -c
LDFLAGS = -m elf_i386 -Ttext 0x8000 --oformat binary

BUILD_DIR = build

# loader.o MUST be first — the bootloader jumps to 0x8000 which must be _start
OBJ = $(BUILD_DIR)/loader.o             \
      $(BUILD_DIR)/kernel.o             \
      \
      $(BUILD_DIR)/os/os.o              \
      \
      $(BUILD_DIR)/shell/cli.o          \
      \
      $(BUILD_DIR)/apps/filefinder.o        \
      $(BUILD_DIR)/apps/terminal.o      \
      \
      $(BUILD_DIR)/config/config.o      \
      \
      $(BUILD_DIR)/fonts/fonts.o        \
      \
      $(BUILD_DIR)/fs/ata.o             \
      $(BUILD_DIR)/fs/fat16.o           \
      \
      $(BUILD_DIR)/io/ps2.o             \
      $(BUILD_DIR)/io/mouse.o           \
      $(BUILD_DIR)/io/keyboard.o        \
      \
      $(BUILD_DIR)/lib/io.o             \
      $(BUILD_DIR)/lib/math.o           \
      $(BUILD_DIR)/lib/mem.o            \
      $(BUILD_DIR)/lib/primitive_graphics.o \
      $(BUILD_DIR)/lib/string.o         \
      $(BUILD_DIR)/lib/types.o          \
      \
      $(BUILD_DIR)/ui/cursor.o          \
      $(BUILD_DIR)/ui/menubar.o         \
      $(BUILD_DIR)/ui/ui.o              \
      $(BUILD_DIR)/ui/widgets.o         \
      $(BUILD_DIR)/ui/window.o

all: os_image.bin
	@echo ""
	@echo "Kernel size : $$(wc -c < kernel.bin) bytes"
	@echo "Max safe    : $$(( 60 * 512 )) bytes (60 sectors)"
	@echo ""

kernel.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/loader.o: loader.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

boot.bin: boot.asm
	$(AS) -f bin $< -o $@

# Create a clean 32MB image, write boot sector, then kernel starting at sector 1
os_image.bin: boot.bin kernel.bin
	dd if=/dev/zero        of=os_image.bin bs=512 count=65536  2>/dev/null
	dd if=boot.bin         of=os_image.bin bs=512 seek=0  conv=notrunc 2>/dev/null
	dd if=kernel.bin       of=os_image.bin bs=512 seek=1  conv=notrunc 2>/dev/null

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: all
	qemu-system-i386 -drive format=raw,file=os_image.bin \
	                 -m 4M -machine pc

clean:
	rm -rf $(BUILD_DIR)
	rm -f *.bin os_image.bin
