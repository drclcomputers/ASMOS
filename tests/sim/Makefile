# The simulator: the kernel's C code built for Linux (32-bit), with a fake
# screen, disks, floppy controller and timer (sim.c, stubs.c, simfdc.c...).
# run.sh builds it and runs every scenario in scen*.c (scen.c runs them).
R ?= ../..
RA := $(abspath $(R))
CFLAGS = -m32 -std=gnu11 -ffreestanding -fno-pie -fno-pic -fno-stack-protector -nostdlib -O1 -g -fno-builtin -w -MMD -MP -I$(R) -I. -DROOT=\"$(RA)/\" -DFIXDIR=\"fixtures/\" -DOUTDIR=\"out/\"
EXCL = $(R)/lib/apm.c $(R)/lib/cpu.c $(R)/config/runtime_config.c $(R)/lib/io.c $(R)/lib/libc_compat.c $(R)/apps/asmusic.c $(R)/os/scheduler.c $(R)/lib/vmm.c
DIRS = lib ui apps fonts fs config shell network
SRCS = $(filter-out $(EXCL),$(foreach d,$(DIRS),$(wildcard $(R)/$(d)/*.c))) $(R)/os/os.c $(R)/os/app_registry.c $(R)/os/clipboard.c $(R)/os/error.c $(R)/os/appload.c $(R)/os/prefs.c $(R)/os/help.c $(R)/os/watchdog.c $(R)/drivers/pointer.c $(R)/drivers/keyboard.c $(R)/drivers/lpt.c $(R)/os/print.c
OBJS = $(patsubst $(R)/%.c,o/%.o,$(SRCS)) o/drivers/joystick.o o/rtcfg.o o/sim.o o/scen.o o/scen_apps1.o o/scen_apps2.o o/scen_storage.o o/scen_help.o o/scen_menu.o o/scen_fs.o o/scen_zip.o o/scen_setup.o o/scen_misc1.o o/scen_misc2.o o/scen_joy.o o/start.o o/stubs.o o/simvmm.o o/simguard.o o/simtask.o o/task_switch.o o/simfdc.o o/drivers/fdd.o o/bootcode.o
sim: $(OBJS)
	ld -m elf_i386 -static --defsym=_backbuf=0x100000 -o $@ $(OBJS) 2>&1 | grep -v 'missing .note\|NOTE: This' ; true
o/%.o: $(R)/%.c
	@mkdir -p $(dir $@); gcc $(CFLAGS) -c $< -o $@
o/sim.o: sim.c sys.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen.o: scen.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_apps1.o: scen_apps1.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_apps2.o: scen_apps2.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_storage.o: scen_storage.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_help.o: scen_help.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_menu.o: scen_menu.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_fs.o: scen_fs.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_zip.o: scen_zip.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_setup.o: scen_setup.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_misc1.o: scen_misc1.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_misc2.o: scen_misc2.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/scen_joy.o: scen_joy.c scen.h ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/stubs.o: stubs.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/start.o: start.S ; @mkdir -p o; gcc -m32 -c $< -o $@
# the kernel's drivers/gpu.c on a model of the VGA planes (gputest.c)
gputest: o/gputest.o o/gpu_real.o o/lib/mem.o o/lib/string.o o/start.o
	ld -m elf_i386 -static --defsym=_backbuf=0x100000 -o $@ $^ 2>&1 | grep -v 'missing .note\|NOTE: This' ; true
o/gputest.o: gputest.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/gpu_real.o: $(R)/drivers/gpu.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
# the kernel's drivers/ata.c (its CD-ROM part) on a model drive (atapitest.c)
atapitest: o/atapitest.o o/ata_real.o o/lib/mem.o o/lib/string.o o/start.o
	ld -m elf_i386 -static -o $@ $^ 2>&1 | grep -v 'missing .note\|NOTE: This' ; true
o/atapitest.o: atapitest.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/ata_real.o: $(R)/drivers/ata.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
clean: ; rm -rf o sim gputest atapitest rtcfg.c simguard.c out
o/rtcfg.o: $(R)/config/runtime_config.c
	@mkdir -p o; sed 's/^.*0x0602.*$$//' $< > rtcfg.c; gcc $(CFLAGS) -c rtcfg.c -o $@
o/simvmm.o: simvmm.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/simguard.o: $(R)/os/appguard.c ; @mkdir -p o; sed -e 's/"  sti\\n"/"  nop\\n"/' -e 's/outb(0x20, 0x20);//' $< > simguard.c; gcc $(CFLAGS) -c simguard.c -o $@
o/simtask.o: simtask.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/task_switch.o: $(R)/os/task_switch.asm ; @mkdir -p o; nasm -f elf32 $< -o $@
o/drivers/lpt.o: $(R)/drivers/lpt.c
	@mkdir -p o/drivers; sed 's/bios_data_word(/sim_bda(/' $< > o/lpt_sim.c; gcc $(CFLAGS) -include sim_bda.h -c o/lpt_sim.c -o $@
o/drivers/joystick.o: $(R)/drivers/joystick.c
	@mkdir -p o/drivers; sed 's/; cli"/; nop"/' $< > o/joy_sim.c; gcc $(CFLAGS) -c o/joy_sim.c -o $@
o/simfdc.o: simfdc.c ; @mkdir -p o; gcc $(CFLAGS) -c $< -o $@
o/drivers/fdd.o: $(R)/drivers/fdd.c
	@mkdir -p o/drivers; sed -E 's/\((uint8_t|bool|int) \*\)0x([0-9A-F]{4})\)/(\1 *)(sim_low + 0x\2 - 0x7000))/' $< > o/fdd_sim.c; gcc $(CFLAGS) -include sim_low.h -c o/fdd_sim.c -o $@

o/bootcode.o: $(R)/boot/bootcode.asm $(R)/build/boot/mbr.bin $(R)/build/boot/vbr.bin $(R)/build/boot/stage2.bin
	@mkdir -p o; cd $(R) && nasm -f elf32 -I. boot/bootcode.asm -o $(CURDIR)/o/bootcode.o

# headers: a changed struct rebuilds everything that uses it
-include $(shell find o -name "*.d" 2>/dev/null)
o/lib/time.o: $(R)/lib/time.c
	@mkdir -p o/lib; sed 's/__asm__ volatile("hlt");/pit_ticks++;/' $< > o/time_sim.c; gcc $(CFLAGS) -c o/time_sim.c -o $@
