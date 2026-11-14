; Page fault handler (interrupt vector 14).
;
; The CPU pushes (before we get control):
;   [esp+0]  error code   (uint32_t)
;   [esp+4]  EIP of the faulting instruction
;   [esp+8]  CS
;   [esp+12] EFLAGS
;   (if privilege change: ESP, SS follow above)
;
; CR2 holds the faulting virtual address.
;
; We save the full register context, call pf_handler_c with three args:
;   (uint32_t fault_addr, uint32_t error_code, uint32_t eip)
; then either iret (if the C handler patched things up) or halt.

[bits 32]

global isr_page_fault
extern pf_handler_c

isr_page_fault:
    ; error code already on stack from CPU
    pusha                       ; save eax ecx edx ebx esp ebp esi edi

    mov  eax, cr2               ; faulting virtual address
    push eax                    ; arg 3: fault_addr  (note: pushed last = lowest addr)

    ; Error code sits at esp + 4*8 + 4 = esp+36 after pusha + push eax.
    ; Actually: pusha pushes 8 regs × 4 = 32 bytes, then we pushed eax = 4 bytes.
    ; Original error code was at the TOP of the stack on entry, so after
    ; those 36 bytes it's at [esp + 36 - 4] ... let's be explicit.
    ;
    ; Stack at isr_page_fault entry (top → bottom):
    ;   +0   error_code   (CPU pushed)
    ;   +4   fault EIP    (CPU pushed)
    ;   ...
    ; After pusha (+32) and push eax (+4):
    ;   +0   fault_addr  (our push)
    ;   +4   edi         (pusha)
    ;   ...
    ;   +36  error_code  (original CPU push)
    ;   +40  fault EIP

    mov  eax, [esp + 36]        ; error code
    push eax                    ; arg 2: error_code

    mov  eax, [esp + 44]        ; fault EIP  (36 + 4 (our push) + 4 (new push))
    push eax                    ; arg 1: eip

    call pf_handler_c
    add  esp, 12                ; pop 3 args

    popa
    add  esp, 4                 ; pop error code
    iret
