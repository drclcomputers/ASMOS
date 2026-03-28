CC = i686-elf-gcc
LD = i686-elf-ld
AS = nasm

CFLAGS = -std=gnu11 -ffreestanding -m32 -O2 -Wall -Wextra -fno-stack-protector -fno-pie -nostdlib -fno-builtin-memset -fno-builtin-memcpy -I. -c
LDFLAGS = -m elf_i386 -Ttext 0x8000 --oformat binary

BUILD_DIR = make

OBJ = $(BUILD_DIR)/loader.o \
      $(BUILD_DIR)/kernel.o \
      $(BUILD_DIR)/lib/primitive_graphics.o \
	  $(BUILD_DIR)/lib/utils.o \
	  $(BUILD_DIR)/lib/types.o \
	  $(BUILD_DIR)/lib/mem.o \
	  $(BUILD_DIR)/lib/string.o \
	  $(BUILD_DIR)/lib/math.o \
	  $(BUILD_DIR)/fonts/fonts.o \
      $(BUILD_DIR)/io/mouse.o \
      $(BUILD_DIR)/io/keyboard.o

all: os_image.bin
	 @rm boot.bin kernel.bin

kernel.bin: $(OBJ)
	$(LD) $(LDFLAGS) -o $@ $^

$(BUILD_DIR)/loader.o: loader.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

boot.bin: boot.asm
	$(AS) -f bin $< -o $@

os_image.bin: boot.bin kernel.bin
	cat boot.bin kernel.bin > os_image.bin

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

run: all
	qemu-system-i386 -drive format=raw,file=os_image.bin

clean:
	rm -rf $(BUILD_DIR)
	rm -f *.bin os_image.bin
