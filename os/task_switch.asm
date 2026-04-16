; On entry (cdecl):
;   [esp+0]  ret-addr
;   [esp+4]  old_esp_ptr  — pointer where we save the current ESP
;   [esp+8]  new_esp      — ESP of the task we switch into
;
; We push 5 things (eflags + 4 callee-saved regs) so esp drops 20 bytes.
; After those pushes the args are at:
;   [esp+20] ret-addr
;   [esp+24] old_esp_ptr
;   [esp+28] new_esp
;
; The frame we save / restore (addresses low → high):
;   saved_esp → [ edi | esi | ebx | ebp | eflags | ret-addr | ... ]
;                 ^pop edi first                    ^ret jumps here

[bits 32]
global task_switch

task_switch:
    pushfd
    push ebp
    push ebx
    push esi
    push edi

    mov  eax, [esp + 24]
    mov  [eax], esp

    mov  ecx, [esp + 28]
    mov  esp, ecx

    pop  edi
    pop  esi
    pop  ebx
    pop  ebp
    popfd
    ret


; Every fresh task (including the kernel task) starts here.
; build_initial_stack() lays the new task's stack out as follows
; (highest address first — this is what esp points at after task_switch
;  pops eflags/ebp/ebx/esi/edi and then does ret):
;
;   [ &task_trampoline ]   ← task_switch's ret lands here    (esp after ret)
;   [ slot ]               ← pushed as cdecl arg for task_trampoline_c
;   [ 0 ]                  ← fake return addr from trampoline_c (never used)
;   ── above this line is the frame task_switch pops ──
;   [ edi = 0 ]
;   [ esi = 0 ]
;   [ ebx = 0 ]
;   [ ebp = 0 ]
;   [ eflags = 0x202 ]     ← IF=1
;   ← saved_esp points here
;
; When task_switch does its final ret, esp points at &task_trampoline.
; We fall through into the body below.  At that moment:
;   [esp+0]  = &task_trampoline  (our own address — the "return address"
;                                  we were ret'd to; we'll never use it)
;   [esp+4]  = slot              (cdecl first argument to task_trampoline_c)
;   [esp+8]  = 0                 (fake ret addr that trampoline_c would use)
;
; We push nothing extra; just call task_trampoline_c with slot already
; sitting at [esp+4] in the standard cdecl position.

global task_trampoline
extern task_trampoline_c

task_trampoline:
    ; esp+0  = our own address (ret-addr we were jumped to via ret)
    ; esp+4  = slot  ← already the first cdecl argument
    ; esp+8  = fake ret-addr for task_trampoline_c
    ;
    ; We adjust esp by 4 so that from task_trampoline_c's perspective
    ; the call looks normal (ret-addr at [esp], first arg at [esp+4]):
    add  esp, 4

    ; Now the stack looks exactly like a normal cdecl call frame:
    ;   [esp+0] = fake ret-addr (0)
    ;   [esp+4] = slot
    jmp  task_trampoline_c

.hang:
    cli
    hlt
    jmp .hang
