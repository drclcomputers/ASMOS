/* linker.ld
   Memory layout (x86 protected mode, 32-bit):

   0x08000          Kernel code start
   +text            Code sections
   +data            Initialized data
   +rodata          Read-only data
   +bss             Uninitialized data (zeros)
   _heap_start      Heap allocation begins here (grows upward)
   ...heap...

   0xA0000          VGA/BIOS/ROM region (reserved, no allocation here)
   0xC0000          Kernel stack top (grows downward from here)
   ...kernel stack (32KB, to 0xB8000)...
   _kstack_bottom   Bottom of kernel stack (0xB8000)

   With 32 tasks × 8KB = 256KB max task stacks + kernel overhead,
   and heap starting at ~0x36000, max heap usage ~0x4D000 = 0x83000.
   This safely fits below VGA at 0xA0000.

   Kernel stack at 0xC0000 gives 0xC0000-0xB8000 = 0x8000 (32KB) for the stack.
   Stack grows down from 0xC0000 toward 0xB8000 (away from heap).
*/
ENTRY(_start)
SECTIONS {
    . = 0x8000;

    .text : {
        build/loader.o(.text)
        build/interrupts/interrupt.o(.text)
        *(.text)
    }

    .data : {
        *(.data)
    }

    .rodata : {
        *(.rodata)
    }

    .bss : {
        _bss_start = .;
        *(.bss)
        *(COMMON)
        _bss_end = .;
    }

    _heap_start = .;

    /* Kernel stack: placed at fixed address 0xC0000 (above VGA region at 0xA0000-0xBFFFF).
       Grows downward from _kstack_top toward _kstack_bottom.
       32KB stack (0xC0000 - 0xB8000 = 0x8000).
       This section is NOLOAD because the loader doesn't include it in the binary;
       it's just a fixed address range we reserve for the kernel stack.
    */
    .kstack 0x88000 (NOLOAD) : { /* Moves stack top to 0x90000 */
        _kstack_bottom = .;
        . += 0x8000;
        _kstack_top = .;
    }

    /* Safety check: ensure heap doesn't grow into the VGA region or kernel stack.
       Heap grows from _heap_start upward.
       With MAX_TASKS=32 and TASK_STACK_SIZE=8KB, allow up to 0x50000 (320KB) for heap.
       This puts the heap limit at _heap_start + 0x50000, which must be < 0xA0000 (VGA start).
       If _heap_start ≈ 0x36000, then 0x36000 + 0x50000 = 0x86000 < 0xA0000 ✓
    */
    ASSERT(_bss_end < 0x90000, "ERROR: Kernel BSS too large")
}
