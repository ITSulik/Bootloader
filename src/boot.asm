[BITS 16]
[ORG 0x7C00]

%ifndef MENU_LOADER_SECTORS
%define MENU_LOADER_SECTORS 1
%endif

CODE_SELECTOR equ 0x08
DATA_SELECTOR equ 0x10
MENU_LOAD_SEG equ 0x1000
MENU_LOAD_ADDR equ 0x10000
BOOT_INFO_ADDR equ 0x0500
BOOT_TARGET_LINUX equ 0x01
BOOT_TARGET_WINDOWS equ 0x02

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    ; BIOS passes the boot drive number in DL. Keep it so disk reads use
    ; the same drive that booted us.
    mov [boot_drive], dl

main_menu:
    call clear_screen

    mov si, title_msg
    call print_string
    mov si, intro_msg
    call print_string
    mov si, drive_msg
    call print_string
    mov al, [boot_drive]
    call print_hex_byte
    mov si, menu_msg
    call print_string

.wait_for_key:
    xor ah, ah
    int 0x16

    cmp al, 'L'
    je linux_selected
    cmp al, 'l'
    je linux_selected
    cmp al, 'W'
    je windows_selected
    cmp al, 'w'
    je windows_selected
    cmp al, 'R'
    je reboot_selected
    cmp al, 'r'
    je reboot_selected

    mov si, invalid_msg
    call print_string
    jmp .wait_for_key

linux_selected:
    mov byte [selected_target], BOOT_TARGET_LINUX
    jmp start_menu_loader

windows_selected:
    mov byte [selected_target], BOOT_TARGET_WINDOWS
    jmp start_menu_loader

start_menu_loader:
    call write_boot_info
    mov si, loading_msg
    call print_string
    call load_menu_loader
    jmp enter_protected_mode

reboot_selected:
    int 0x19

load_menu_loader:
    ; Use INT 13h extensions so we can read by LBA instead of older CHS math.
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, disk_address_packet
    int 0x13
    jc disk_error
    ret

enter_protected_mode:
    cli
    call enable_a20
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SELECTOR:protected_mode_start

enable_a20:
    in al, 0x92
    or al, 00000010b
    out 0x92, al
    ret

write_boot_info:
    ; Stage 2 reads this structure from low memory so it knows what the
    ; user picked in the BIOS menu and which drive booted the loader.
    mov byte [BOOT_INFO_ADDR + 0], 'S'
    mov byte [BOOT_INFO_ADDR + 1], 'B'
    mov byte [BOOT_INFO_ADDR + 2], 'I'
    mov byte [BOOT_INFO_ADDR + 3], '1'
    mov byte [BOOT_INFO_ADDR + 4], 1

    mov al, [selected_target]
    mov byte [BOOT_INFO_ADDR + 5], al

    mov al, [boot_drive]
    mov byte [BOOT_INFO_ADDR + 6], al
    mov byte [BOOT_INFO_ADDR + 7], 0
    ret

disk_error:
    mov si, disk_error_msg
    call print_string

.hang:
    cli
    hlt
    jmp .hang

clear_screen:
    mov ax, 0x0003
    int 0x10
    ret

print_string:
.next_char:
    lodsb
    test al, al
    jz .done

    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    jmp .next_char

.done:
    ret

print_hex_byte:
    push ax

    mov ah, al
    shr al, 4
    call print_hex_nibble

    mov al, ah
    and al, 0x0F
    call print_hex_nibble

    pop ax
    ret

print_hex_nibble:
    and al, 0x0F
    cmp al, 9
    jbe .digit
    add al, 'A' - 10
    jmp .emit

.digit:
    add al, '0'

.emit:
    mov ah, 0x0E
    mov bh, 0x00
    mov bl, 0x07
    int 0x10
    ret

title_msg db "Soul Boot", 0x0D, 0x0A, 0
intro_msg db "L/W -> menu loader", 0x0D, 0x0A, 0
drive_msg db "DL=0x", 0
menu_msg db 0x0D, 0x0A, "[L] Linux", 0x0D, 0x0A, "[W] Windows", 0x0D, 0x0A, "[R] Reboot", 0x0D, 0x0A, "> ", 0
invalid_msg db 0x0D, 0x0A, "Use L, W, or R.", 0
loading_msg db 0x0D, 0x0A, "Loading menu loader...", 0
disk_error_msg db 0x0D, 0x0A, "Disk read failed.", 0
boot_drive db 0
selected_target db 0

align 4
disk_address_packet:
    db 0x10
    db 0x00
    dw MENU_LOADER_SECTORS
    dw 0x0000
    dw MENU_LOAD_SEG
    dq 0x0000000000000001

align 8
gdt_start:
    dq 0
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

[BITS 32]
protected_mode_start:
    mov ax, DATA_SELECTOR
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000

    jmp CODE_SELECTOR:MENU_LOAD_ADDR

times 510 - ($ - $$) db 0
dw 0xAA55
