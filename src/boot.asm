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
MBR_SECTOR_ADDR equ 0x0600
GPT_HEADER_ADDR equ 0x0800
GPT_ENTRY_SECTOR_ADDR equ 0x0A00
CHAINLOAD_BUFFER_ADDR equ 0x7E00
BOOT_TARGET_LINUX equ 0x01
BOOT_TARGET_WINDOWS equ 0x02
BOOT_INFO_FLAG_MBR_VALID equ 0x01
BOOT_INFO_FLAG_GPT_HEADER_VALID equ 0x02
BOOT_INFO_FLAG_GPT_ENTRY_VALID equ 0x04
BOOT_INFO_FLAG_CHAINLOAD_MATCH equ 0x08
BOOT_INFO_FLAG_CHAINLOAD_READ_OK equ 0x10
BOOT_INFO_FLAG_LINUX_LAYOUT_VALID equ 0x20

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; BIOS passes the boot drive number in DL. Keep it so disk reads use
    ; the same drive that booted us.
    mov [boot_drive], dl

main_menu:
    mov si, menu_msg
    call print_string

.wait_for_key:
    xor ah, ah
    int 0x16
    and al, 11011111b

    cmp al, 'L'
    je linux_selected
    cmp al, 'W'
    je windows_selected
    jmp .wait_for_key

linux_selected:
    mov byte [selected_target], BOOT_TARGET_LINUX
    jmp start_menu_loader

windows_selected:
    mov byte [selected_target], BOOT_TARGET_WINDOWS

start_menu_loader:
    call inspect_boot_disk
    call try_mbr_chainload
    call write_boot_info
    call load_menu_loader
    jmp enter_protected_mode

load_menu_loader:
    ; Use INT 13h extensions so we can read by LBA instead of older CHS math.
    mov si, menu_loader_dap
    call read_with_dap
    jc disk_error
    ret

inspect_boot_disk:
    mov byte [inspection_flags], 0

    mov si, boot_disk_dap
    call read_with_dap
    jc .skip_mbr
    or byte [inspection_flags], BOOT_INFO_FLAG_MBR_VALID

.skip_mbr:
    mov word [boot_disk_dap + 4], GPT_HEADER_ADDR
    inc word [boot_disk_dap + 8]
    call read_with_dap
    jc .done
    or byte [inspection_flags], BOOT_INFO_FLAG_GPT_HEADER_VALID

    mov word [boot_disk_dap + 4], GPT_ENTRY_SECTOR_ADDR
    inc word [boot_disk_dap + 8]
    call read_with_dap
    jc .done
    or byte [inspection_flags], BOOT_INFO_FLAG_GPT_ENTRY_VALID

.done:
    ret

try_mbr_chainload:
    test byte [inspection_flags], BOOT_INFO_FLAG_MBR_VALID
    jz .no_match

    cmp word [MBR_SECTOR_ADDR + 510], 0xAA55
    jne .no_match

    mov si, MBR_SECTOR_ADDR + 446
    mov cx, 4

.find_candidate:
    mov al, [selected_target]
    cmp al, BOOT_TARGET_LINUX
    je .check_linux

    cmp byte [si + 4], 0x07
    je .candidate_found
    cmp byte [si + 4], 0x0B
    je .candidate_found
    cmp byte [si + 4], 0x0C
    je .candidate_found
    jmp .next_partition

.check_linux:
    cmp byte [si + 4], 0x83
    je .candidate_found

.next_partition:
    add si, 16
    loop .find_candidate

.no_match:
    ret

.candidate_found:
    or byte [inspection_flags], BOOT_INFO_FLAG_CHAINLOAD_MATCH

    ; Reuse this DAP slot as temporary scratch space for chainloading.
    mov word [boot_disk_dap + 4], CHAINLOAD_BUFFER_ADDR

    mov ax, [si + 8]
    mov [boot_disk_dap + 8], ax
    mov ax, [si + 10]
    mov [boot_disk_dap + 10], ax
    xor ax, ax
    mov [boot_disk_dap + 12], ax
    mov [boot_disk_dap + 14], ax

    mov si, boot_disk_dap
    call read_with_dap
    jc .no_match
    or byte [inspection_flags], BOOT_INFO_FLAG_CHAINLOAD_READ_OK

    cmp byte [selected_target], BOOT_TARGET_LINUX
    jne .check_signature

    mov word [boot_disk_dap + 4], GPT_ENTRY_SECTOR_ADDR
    add word [boot_disk_dap + 8], 2
    adc word [boot_disk_dap + 10], 0
    call read_with_dap
    jc .check_signature
    and byte [inspection_flags], ~BOOT_INFO_FLAG_GPT_ENTRY_VALID
    or byte [inspection_flags], BOOT_INFO_FLAG_LINUX_LAYOUT_VALID

    mov word [boot_disk_dap + 4], GPT_HEADER_ADDR
    add word [boot_disk_dap + 8], 2
    adc word [boot_disk_dap + 10], 0
    call read_with_dap

.check_signature:
    cmp word [CHAINLOAD_BUFFER_ADDR + 510], 0xAA55
    jne .no_match

    mov si, CHAINLOAD_BUFFER_ADDR
    mov di, 0x7C00
    mov cx, 256
    rep movsw

    mov dl, [boot_drive]
    mov sp, 0x7C00
    jmp 0x0000:0x7C00

read_with_dap:
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    ret

enter_protected_mode:
    in al, 0x92
    or al, 00000010b
    out 0x92, al
    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp CODE_SELECTOR:protected_mode_start

write_boot_info:
    ; Stage 2 reads this structure from low memory so it knows what the
    ; user picked in the BIOS menu and which drive booted the loader.
    mov word [BOOT_INFO_ADDR + 0], 0x4253
    mov word [BOOT_INFO_ADDR + 2], 0x3149
    mov byte [BOOT_INFO_ADDR + 4], 1

    mov al, [selected_target]
    mov byte [BOOT_INFO_ADDR + 5], al

    mov al, [boot_drive]
    mov byte [BOOT_INFO_ADDR + 6], al

    mov al, [inspection_flags]
    mov byte [BOOT_INFO_ADDR + 7], al
    ret

disk_error:
.hang:
    jmp $

print_string:
.next_char:
    lodsb
    test al, al
    jz .done

    mov ah, 0x0E
    int 0x10
    jmp .next_char

.done:
    ret

menu_msg db "L/W>", 0
boot_drive db 0
selected_target db 0
inspection_flags db 0

align 4
menu_loader_dap:
    db 0x10
    db 0x00
    dw MENU_LOADER_SECTORS
    dw 0x0000
    dw MENU_LOAD_SEG
    dq 0x0000000000000001

boot_disk_dap:
    db 0x10
    db 0x00
    dw 1
    dw MBR_SECTOR_ADDR
    dw 0x0000
    dq 0x0000000000000000

align 4
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
