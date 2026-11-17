bits 32
start:
    push ebp
    mov  ebp, esp
    push ebx
    push esi
    call .here
.here:
    pop  esi
    sub  esi, .here - start
    mov  ebx, [ebp + 8]
    lea  eax, [esi + prompt - start]
    push eax
    call [ebx + 0]
    add  esp, 4
    lea  eax, [esi + buf - start]
    push 32
    push eax
    call [ebx + 8]
    add  esp, 8
    lea  eax, [esi + hi - start]
    push eax
    call [ebx + 0]
    add  esp, 4
    lea  eax, [esi + buf - start]
    push eax
    call [ebx + 0]
    add  esp, 4
    push '!'
    call [ebx + 4]
    add  esp, 4
    push 10
    call [ebx + 4]
    add  esp, 4
    pop  esi
    pop  ebx
    pop  ebp
    ret
prompt db "Name? ", 0
hi     db "Hello, ", 0
buf    times 32 db 0
