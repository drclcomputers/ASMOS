# How ASMOS boots, and how to install it

## The disks

A hard disk with ASMOS on it, the way Setup (or `make`) leaves it:

```
LBA 0          MBR: boot code (boot/mbr.asm) + partition table:
               one FAT16 partition, active, type 06 (04 under 32 MB)
LBA 1..62      empty (the rest of the first track, as DOS leaves it)
LBA 63         the partition:
  sector 0       VBR: BPB + boot code (boot/vbr.asm)
  sectors 1..15  stage 2 (boot/stage2.asm), in the reserved sectors
  then           FAT, FAT, root folder (512 entries), files:
                 SYSTEM\KERNEL.SYS   the kernel (hidden, system, read-only)
                 APPS\...            the apps and the Apps menu
                 DESKTOP, ASMOS.CFG  made at the first start
```

The partition can be any size from 5 MB to 2 GB (FAT16's limit). On a
bigger disk the rest stays free. Cluster sizes are the ones MS-DOS FORMAT
picks (2 KB up to 128 MB ... 32 KB up to 2 GB), so DOS and Windows 9x read
the disk too.

The setup floppy (`setup.img`, 1.44 MB, FAT12) boots the same way but has no
MBR. The BIOS starts its VBR directly:

```
sector 0       VBR (the same code, a floppy BPB)
sectors 1..4   stage 2
               SYSTEM\KERNEL.SYS   the same kernel (this one boots the floppy)
               SETUP\SETUP.INF     the apps Setup lets the user choose
                                   (tools/mkdisk.py, from extapps/MANIFEST:
                                   app|name|size|group|path in APPS|KB)
               SETUP\ASMOS.ZIP     what Setup puts on the hard disk: the
                                   kernel again, compressed (half as much
                                   floppy to read), and APPS
               README.TXT
```

## Booting, step by step

1. **BIOS.** After the POST, the BIOS reads sector 0 of the boot drive to
   0x7C00 and jumps there with DL set to the drive (00h for A:, 80h for the
   first hard disk).

2. **MBR** (hard disk only; `boot/mbr.asm`, 446 bytes).
    - It moves itself to 0x0600 to make room.
    - It looks through the partition table for the active partition
      (flag 80h).
    - It reads that partition's first sector, the VBR, to 0x7C00. It uses
      INT 13h AH=42h (LBA) if the BIOS has it. Otherwise it uses AH=02h
      (CHS), with the geometry the BIOS reports through AH=08h, or failing
      that the CHS values in the partition entry, as DOS does.
    - It jumps to the VBR.
    - Messages: `No active partition`, `Disk error`, `Partition not bootable`.

3. **VBR** (`boot/vbr.asm`, one sector, at 0x7C00).
    - Its first 62 bytes are the volume's BPB.
    - It works out how to read this drive: LBA if the BIOS has it, else CHS.
      For CHS a hard disk uses the BIOS's geometry and a floppy uses the
      BPB's.
    - It reads stage 2, which is the reserved sectors that follow it, to
      0x7E00. The BPB's `hidden_sectors` says where the volume starts on the
      disk: 63 on the hard disk, 0 on the floppy.
    - It checks that stage 2 starts with "VM", then jumps to it.
    - It leaves what it found out at 0x7C3E for stage 2: the drive, LBA or
      not, sectors per track, heads.
    - Message: `Not an ASMOS disk. Press a key` (the BIOS then tries the next
      device).

4. **Stage 2** (`boot/stage2.asm`, about 2 KB, at 0x7E00). It works in
   the order below, and prints `Starting ASMOS` followed by a dot for
   each piece it reads.
    1. **RAM.** It asks the BIOS how much there is (INT 15h AH=88h) and
       writes the size to 0x0500.
       It also looks for APM (INT 15h AX=5300h). If the BIOS has the
       32-bit interface, it connects it (AX=5303h) and leaves the version
       and segments at 0x0610. Only real mode can connect it; the kernel
       then calls it from protected mode to switch the PC off at
       Shut Down (lib/apm.c).
    2. **A20.** It turns on the A20 line (BIOS call, then the keyboard
       controller, then port 92h). Without it, memory past 1 MB wraps
       around to 0.
    3. **Find the kernel.** It reads the volume's own FAT:
        - It works out where the FATs, the root folder and the data are,
          and whether the volume is FAT12 or FAT16.
        - It finds `SYSTEM` in the root, then `KERNEL.SYS` inside it.
    4. **Load the kernel.** It follows the file's cluster chain in the FAT.
        - Clusters that sit next to each other are read together, up to
          64 KB at a time, into 0x10000. The file doesn't have to be in one
          piece.
        - Each piece is copied to 1 MB and up in **unreal mode**: real mode,
          but with DS and ES given 4 GB limits by a brief switch to protected
          mode. That way a real-mode loader can write above 1 MB.
        - There is **no size limit** on the kernel any more, other than RAM.
    5. **Hand-off values.** It writes the boot drive to 0x0608, whether LBA
       reads worked to 0x0609, and the volume's start to 0x060C.
    6. **Screen mode.** It sets the mode stored in byte 4 of its own first
       sector (`config/video_cfg.h`): 0 = 320x200x256, 1-3 = 640x400,
       640x480, 800x600 in 16 colours, 4 = 320x200x16 (BIOS mode 0Dh),
       5-7 = 640x400, 640x480, 800x600 in 256 colours. Settings > Display
       changes that byte, and so does Setup. It never sets more than the
       machine can take:
        - under 3 MB of RAM only 320x200; a 386 (it can't set the AC flag)
          up to 640x480; 800x600 with a 486 and 4 MB.
        - 800x600x16: a card that accepts VESA mode 102h or mode 6Ah.
        - 256 colours above 320x200: a VESA BIOS (4F00h) with an 8-bit
          packed-pixel graphics mode of that size (4F01h): the standard
          number (100h, 101h, 103h) first, then the card's mode list. It
          needs a linear frame buffer, or a writable window at A000h.
          4F02h sets it, with the linear buffer if there is one (if that
          is refused, with the window).
        - If the mode is refused, the same colours at a smaller size; at
          320x200, 256 colours if it was a bigger size that failed.
          What it allowed goes to 0x0603 (a bit per mode) for Setup and
          Settings, and what it found on the card to 0x0620-0x0631 (the
          VESA mode, line length, frame buffer, window: `boot/bootdefs.inc`).
          The kernel maps the linear buffer uncached, or moves the window
          with 4F05h through `lib/realmode.asm` (a brief drop to real mode).
    7. **Protected mode.** It switches to 32-bit protected mode and jumps
       to the kernel at 1 MB.

    Messages: `SYSTEM\KERNEL.SYS is missing.`, `... is damaged.` (the FAT
    chain is shorter than the file), `Disk error.`, then `Press a key to
restart.`

5. **Kernel** (`boot/loader.asm`, then `kmain` in `kernel.c`).
    - It clears the BSS and sets up the stack, the heap, interrupts and
      drivers.
    - `ata_init` makes **HDA** the disk with ASMOS on it. That's the one
      whose active partition (or sector 0, on old images) carries an ASMOS
      boot sector.
    - `fs_mount` reads sector 0 of each hard disk:
        - **a BPB:** the volume starts at sector 0 (an old one-volume image,
          which still works);
        - **an MBR:** it finds the FAT partition, preferring the one booted
          from (0x060C), then the active one, then the first. Every sector
          number after that is counted from the partition's start
          (`base_lba` in fs/fs.c).
    - **Setup mode:** if it booted from a floppy (drive < 80h) that has
      `SETUP\ASMOS.ZIP`, the kernel runs in setup mode (`g_setup_mode`):
        - nothing is written to the hard disk (no ASMOS.CFG, no DESKTOP, no
          APPS);
        - the desktop is empty;
        - the Apps menu lists the built-in apps;
        - Setup opens by itself.
    - Otherwise the desktop starts as usual.

## Installing (what you do)

### In 86Box (or on a real PC)

1. Build: `make` gives `setup.img` (the setup floppy) and `os_image.bin` (a
   64 MB disk already installed, for development: `make HDD_MB=200` for
   another size).
2. Give the machine a hard disk of any size. In 86Box, Settings > Storage >
   Hard disks > New: 20 MB, 100 MB, 500 MB, whatever you like. Old BIOSes
   need the disk set up in their CMOS setup (type "Auto" or "User" with the
   values 86Box shows).
3. Put `setup.img` in drive A: (86Box: the floppy icon > Existing image)
   and make sure the BIOS boots from A: first (the usual "A:, C:" order).
4. Start the machine. ASMOS starts from the floppy and **ASMOS Setup**
   opens:
    1. **Welcome.** Press Continue.
    2. **Which hard disk?** HDA and HDB are listed with their model, size
       and what's on them (empty, ASMOS, FAT, other). Pick one.
    3. **What to do:**
        - **Erase the disk, then install:** a new partition table and one
          FAT16 partition, as big as the disk allows up to 2 GB. Everything
          on the disk is lost.
        - **Keep the files on it:** only possible on a disk that already has
          ASMOS, or a FAT volume with room for the loader. It updates the
          boot code, KERNEL.SYS and the apps. Your files and settings stay.
        - Also pick the screen mode the disk will start in (only the ones
          this machine can take are listed).
    4. **What do you want on it?** Network (off: World View is left out,
       and ASMOS.CFG says not to start the network card: more memory
       free), pictures and sound (Picture Viewer, Audio Player), games.
    5. **How much of ASMOS?** Minimal (the built-in apps, Clock,
       Calculator), Medium (and Calendar, ASMDraw, ClipView, Picture
       Viewer, Puzzle), Full (every app), Custom (a tick for each app):
       each within the answers on page 4. The sizes come from SETUP.INF
       (extapps/MANIFEST's third column). On a disk that has ASMOS, Custom
       starts with the apps it has.
    6. **Sound and printer:** sound on or off, the start-up chime, the
       printer's kind and port, into the disk's ASMOS.CFG.
    7. **Ready to install.** This page says what will happen, and names the
       disk when it is going to be erased. Press **Install**.
    8. **Installing.** The steps are: partition and format, boot code, then
       KERNEL.SYS and the chosen apps, both from ASMOS.ZIP, then the
       settings. A bar shows the
       progress. Nearly all the time goes on reading the floppy: about
       300 KB, around 10-15 seconds.
    9. **Done.** Take the floppy out (86Box: the floppy icon > Eject) and
       press **Restart**.
5. The computer now starts from the hard disk: BIOS, MBR, VBR, stage 2,
   KERNEL.SYS. The first start makes ASMOS.CFG and the DESKTOP folder.

### From a running ASMOS

- **Adding and taking out apps:** start from the setup floppy with
  ASMOS on HDA. Setup's first page asks: change the apps of the ASMOS on
  HDA, or install. Changing shows the same ticks (what is unticked is
  deleted from HDA's /APPS, an emptied group folder too; nothing else is
  touched), and the network question sets HDA's ASMOS.CFG. Then take the
  floppy out and Restart.
- **`app run Setup`** in ASMTerm, with the setup floppy in A:, does the
  same from a running ASMOS, or installs on the other disk (HDB). It won't
  erase the disk ASMOS is running from. Setup is in the Apps menu only when
  started from the setup floppy.
- **`sys hdb`** (or `sys hda`) in ASMTerm makes a disk with a FAT16 volume
  start ASMOS, like DOS's `SYS C:`. It writes the boot code (keeping the
  partition table and the BPB) and copies KERNEL.SYS from this system or
  from the setup floppy. Its files stay.

### A new kernel

Replace `SYSTEM\KERNEL.SYS`: run Setup with "keep the files", or `sys`, or
use `make`, which builds a new `os_image.bin`. FileF won't let you delete,
move or rename anything in SYSTEM: without KERNEL.SYS the disk doesn't start.

## Testing without a machine

`make boottest` boots `os_image.bin` and `setup.img` in `tools/bootemu.py`.
That's a small x86 emulator (16- and 32-bit code, unreal and protected mode)
with a strict fake BIOS. It runs them under many kinds of BIOS: LBA, CHS
only with several geometries, a BIOS that won't report geometry, failing
reads, one sector per call. Each time it checks that the kernel arrives at
1 MB byte for byte. Then it breaks the disk in each of the ways above and
checks the message.
