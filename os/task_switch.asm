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

    mov  ecx, [esp + 28]       ; new_esp — read BEFORE we move esp
    mov  esp, ecx

    mov  ecx, [esp + 28]
    mov  edx, [esp + 32]
    mov  esp, ecx

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
