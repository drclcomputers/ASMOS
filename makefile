CC = i686-elf-gcc
LD = i686-elf-ld
AS = nasm

CFLAGS  = -std=gnu11 -ffreestanding -m32 -O2 -Wall -Wextra \
          -fno-stack-protector -fno-pie -nostdlib \
          -fno-builtin-memset -fno-builtin-memcpy \
          -I. -c
LDFLAGS = -m elf_i386 -T linker.ld --oformat binary

BUILD_DIR = build

OBJ = $(BUILD_DIR)/loader.o                     \
      $(BUILD_DIR)/kernel.o                     \
      \
	  $(BUILD_DIR)/os/app_registry.o            \
	  $(BUILD_DIR)/os/error.o                   \
      $(BUILD_DIR)/os/os.o                      \
	  $(BUILD_DIR)/os/scheduler.o               \
      $(BUILD_DIR)/os/task_switch.o             \
	  \
	  $(BUILD_DIR)/interrupts/interrupt.o  		\
      $(BUILD_DIR)/interrupts/idt.o        		\
      \
      $(BUILD_DIR)/shell/asm/asm.o              \
      $(BUILD_DIR)/shell/binrun.o               \
      $(BUILD_DIR)/shell/cli.o                  \
      $(BUILD_DIR)/shell/cmds.o                 \
      $(BUILD_DIR)/shell/term_buf.o             \
      \
	  $(BUILD_DIR)/apps/asmdraw.o               \
	  $(BUILD_DIR)/apps/asmterm.o               \
	  $(BUILD_DIR)/apps/asmusic.o               \
	  $(BUILD_DIR)/apps/calculator.o            \
	  $(BUILD_DIR)/apps/clock.o                 \
      $(BUILD_DIR)/apps/filef.o                 \
      $(BUILD_DIR)/apps/hexview.o               \
	  $(BUILD_DIR)/apps/monitor.o               \
	  $(BUILD_DIR)/apps/settings.o              \
	  $(BUILD_DIR)/apps/teditor.o 				\
      \
	  $(BUILD_DIR)/config/config.o              \
	  $(BUILD_DIR)/config/runtime_config.o      \
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
      $(BUILD_DIR)/lib/core/io.o                \
	  $(BUILD_DIR)/lib/core/types.o             \
      \
      $(BUILD_DIR)/lib/memory/mem.o             \
      $(BUILD_DIR)/lib/memory/alloc.o           \
      \
      $(BUILD_DIR)/lib/string/string.o          \
      \
      $(BUILD_DIR)/lib/math/math.o              \
      \
      $(BUILD_DIR)/lib/time/time.o              \
      \
      $(BUILD_DIR)/lib/device/speaker.o         \
      $(BUILD_DIR)/lib/device/serial.o          \
      \
      $(BUILD_DIR)/lib/graphics/primitive_graphics.o \
      \
      $(BUILD_DIR)/lib/compat/libcore.o         \
      $(BUILD_DIR)/lib/compat/libc_compat.o     \
      \
      $(BUILD_DIR)/ui/cursor.o                  \
	  $(BUILD_DIR)/ui/desktop.o                 \
	  $(BUILD_DIR)/ui/desktop_fs.o              \
	  $(BUILD_DIR)/ui/icons.o                   \
      $(BUILD_DIR)/ui/menubar.o                 \
	  $(BUILD_DIR)/ui/modal.o                   \
      $(BUILD_DIR)/ui/widgets.o                 \
      $(BUILD_DIR)/ui/window.o

KERNEL_SECTORS_LOADED = 1000

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

$(BUILD_DIR)/os/task_switch.o: os/task_switch.asm | $(BUILD_DIR)
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
	                 -m 4M -machine pc \
	                 -audiodev coreaudio,id=snd0 \
	                 -machine pcspk-audiodev=snd0

bochs: all
	bochs -f bochs/bochssrc.txt -q

clean:
	rm -rf $(BUILD_DIR)
	rm -f *.bin os_image.bin
	rm -f qemu.log
