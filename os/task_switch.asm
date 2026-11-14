[bits 32]
global task_switch

task_switch:
    pushfd
    push ebp
    push ebx
    push esi
    push edi

    mov  eax, [esp + 24]       ; old_esp_ptr
    mov  [eax], esp            ; *old_esp_ptr = current ESP

    ; Read BOTH args from the current (old) stack before switching.
    ; (Previously the second read came from the NEW stack, so ESP became the
    ; new task's saved arg — e.g. its slot number — and every task crashed.)
    mov  ecx, [esp + 28]       ; new_esp
    mov  edx, [esp + 32]       ; new_cr3 (0 = keep current)
    mov  esp, ecx

    test edx, edx
    jz   .no_cr3_switch
    mov  cr3, edx

.no_cr3_switch:
    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    popfd
    ret

; ── Task trampoline (unchanged) ───────────────────────────────────────────────

global task_trampoline
extern task_trampoline_c

task_trampoline:
    jmp  task_trampoline_c

.hang:
    cli
    hlt
    jmp  .hang
