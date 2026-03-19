[BITS 16]
[ORG 0x7c00]


CODE_OFFSET equ 0x8
DATA_OFFSET equ 0x10


KERNEL_LOAD_SEG equ 0x1000 ; Load the kernel at 0x1000:0000 (physical address 0x10000)
KERNEL_START_ADDR equ 0x100000 ; The address where the kernel will be loaded in memory

start:
    cli; Clear interrupts
    mov ax, 0x00 ; Set up segment registers
    mov ds, ax ; Data segment
    mov es, ax ; Extra segment
    mov ss, ax  ; Stack segment
    mov sp, 0x7c00  ; Set stack pointer to the end of the boot sector
    sti; Enable interrupts

; Load the kernel from disk into memory
    mov bx, KERNEL_LOAD_SEG ; Segment where the kernel will be loaded
    mov dh, 0x00 ; Head
    mov dl, 0x80 ; Drive (0 for floppy)
    mov ch, 0x00 ; Cylinder
    mov cl, 0x02 ; Sector (start from sector 2 to skip the boot sector)
    mov ah, 0x02 ; BIOS function: Read sectors into memory
    mov al, 8; Number of sectors to read (32 sectors =
    int 0x13 ; Call BIOS interrupt to read the kernel

    jc disk_error ; Jump to error handling if the disk read fails




load_PM:
    cli; Clear interrupts
    lgdt [gdt_descriptor] ; Load the GDT
    mov eax, cr0 ; Read the current value of CR0
    or al, 1 ; Set the PE (Protection Enable) bit in CR0 to switch to protected mode
    mov cr0, eax ; Update CR0 to enable protected mode
    jmp CODE_OFFSET:PModeMain ; Far jump to flush the instruction pipeline and switch to protected mode 

disk_error:
    hlt ; Halt the CPU if there is a disk error


;gdt implementation
gdt_start:
    dd 0x0 ; Null descriptor
    dd 0x0 ; Null descriptor

    ; Code segment descriptor
    dw 0xFFFF       ; Limit low
    dw 0x0000       ; Base low
    db 0x00         ; Base middle
    db 10011010b    ; Access byte
    db 11001111b    ; Granularity / F lags
    db 0x00         ; Base high

    ; Data segment descriptor
    dw 0xFFFF       ; Limit low
    dw 0x0000       ; Base low
    db 0x00         ; Base middle
    db 10010010b    ; Access byte
    db 11001111b    ; Granularity / F lags
    db 0x00         ; Base high

gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1 ; Limit
    dd gdt_start ; Base


[BITS 32]
PModeMain:
    mov ax, DATA_OFFSET ; Set up data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov ss, ax
    mov gs, ax
    mov ebp, 0x9C00 ; Set up stack pointer
    mov esp, ebp

    in al, 0x92
    or al, 2 ; Set the A20 line
    out 0x92, al

    jmp CODE_OFFSET:KERNEL_START_ADDR ; Jump to the kernel entry point

times 510 - ($ - $$) db 0 ; Pad the rest of the boot sector with zeros


dw 0xAA55 ; Boot signature
