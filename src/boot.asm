[BITS 16]
[ORG 0x7c00]

start:
    cli; Clear interrupts
      ; Set up the stack

    mov ax, 0x00
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00

    sti; Enable interrupts
    mov si, msg

print:
    lodsb ; Load byte at [si] into al and increment si
    cmp al, 0
    je done
    mov ah, 0x0E ; BIOS teletype output function
    int 0x10
    jmp print

done:
    cli
    hlt; stop the CPU

msg: db 'test test test', 0

times 510 - ($ - $$) db 0 ; Pad the rest of the boot sector with zeros


dw 0xAA55 ; Boot signature