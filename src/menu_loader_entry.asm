[BITS 16]

section .text

global menu_loader_entry
extern boot_menu_main

CODE_SELECTOR equ 0x08
DATA_SELECTOR equ 0x10
BOOT_INFO_ADDR equ 0x0500
BOOT_INFO_EXTRA_FLAGS_ADDR equ 0x050C
BOOT_INFO_WINDOWS_FLAGS_ADDR equ 0x050D
BOOT_INFO_WINDOWS_DRIVE_ADDR equ 0x050E
GPT_HEADER_ADDR equ 0x0800
GPT_ENTRY_SECTOR_ADDR equ 0x0A00
CHILD_INODE_SECTOR_ADDR equ 0x0C00
CHILD_DIR_SECTOR_ADDR equ 0x0E00
GRANDCHILD_INODE_SECTOR_ADDR equ 0x1200
GRANDCHILD_DIR_SECTOR_ADDR equ 0x1400
CONFIG_FILE_INODE_SECTOR_ADDR equ 0x1600
LINUX_KERNEL_SECTOR_ADDR equ 0x1800
LINUX_INITRD_SECTOR_ADDR equ 0x1C00
FAT_EFI_DIR_SECTOR_ADDR equ 0x2000
FAT_EFI_BOOT_DIR_SECTOR_ADDR equ 0x2200
FAT_EFI_SYSTEMD_DIR_SECTOR_ADDR equ 0x2400
FAT_EFI_BOOT_FILE_SECTOR_ADDR equ 0x2600
FAT_EFI_SYSTEMD_FILE_SECTOR_ADDR equ 0x2800
WINDOWS_PROBE_MBR_ADDR equ 0x2A00
WINDOWS_PROBE_GPT_HEADER_ADDR equ 0x2C00
WINDOWS_PROBE_GPT_ENTRY_SECTOR_ADDR equ 0x2E00
WINDOWS_PROBE_ESP_VBR_ADDR equ 0x3000
WINDOWS_PROBE_DATA_VBR_ADDR equ 0x3200
CHAINLOAD_BUFFER_ADDR equ 0x7E00
BOOT_TARGET_LINUX equ 0x01
BOOT_INFO_FLAG_GPT_HEADER_VALID equ 0x02
BOOT_INFO_FLAG_CHAINLOAD_READ_OK equ 0x10
BOOT_INFO_FLAG_ROOT_DIR_VALID equ 0x40
BOOT_INFO_FLAG_CHILD_DIR_VALID equ 0x80
BOOT_INFO_DETAIL_GRUB_DIR_VALID equ 0x01
BOOT_INFO_DETAIL_GRUB_CFG_VALID equ 0x02
BOOT_INFO_DETAIL_FAT_ENTRIES_VALID equ 0x04
BOOT_INFO_DETAIL_FAT_ENTRY_FILE_VALID equ 0x08
BOOT_INFO_DETAIL_FAT_KERNEL_VALID equ 0x10
BOOT_INFO_DETAIL_FAT_INITRD_VALID equ 0x20
BOOT_INFO_EXTRA_FAT_EFI_DIR_VALID equ 0x01
BOOT_INFO_EXTRA_FAT_EFI_BOOT_DIR_VALID equ 0x02
BOOT_INFO_EXTRA_FAT_EFI_SYSTEMD_DIR_VALID equ 0x04
BOOT_INFO_EXTRA_FAT_EFI_BOOT_FILE_VALID equ 0x08
BOOT_INFO_EXTRA_FAT_EFI_SYSTEMD_FILE_VALID equ 0x10
BOOT_INFO_WINDOWS_FLAG_MBR_VALID equ 0x01
BOOT_INFO_WINDOWS_FLAG_GPT_HEADER_VALID equ 0x02
BOOT_INFO_WINDOWS_FLAG_GPT_ENTRY_VALID equ 0x04
BOOT_INFO_WINDOWS_FLAG_ESP_VBR_VALID equ 0x08
BOOT_INFO_WINDOWS_FLAG_DATA_VBR_VALID equ 0x10
MENU_LOADER_STACK_TOP equ 0x90000

; Stage 2 starts in real mode now, while BIOS services still work.
; That lets us do larger inspection reads here before switching to
; protected mode for the C fallback screen.
menu_loader_entry:
    cli
    mov ax, cs
    mov ds, ax
    xor ax, ax
    mov es, ax
    mov byte [es:BOOT_INFO_EXTRA_FLAGS_ADDR], 0
    mov byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], 0
    mov byte [es:BOOT_INFO_WINDOWS_DRIVE_ADDR], 0

    call maybe_read_fat_root_dir
    call maybe_read_root_dir
    call maybe_probe_windows_drive

    in al, 0x92
    or al, 00000010b
    out 0x92, al
    lgdt [gdt_descriptor - menu_loader_entry]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp dword CODE_SELECTOR:protected_mode_start

maybe_read_root_dir:
    cmp byte [es:BOOT_INFO_ADDR + 0], BOOT_TARGET_LINUX
    jne .done
    test byte [es:BOOT_INFO_ADDR + 2], BOOT_INFO_FLAG_GPT_HEADER_VALID
    jz .done
    cmp word [es:GPT_ENTRY_SECTOR_ADDR + 88], 256
    jne .done

    test word [es:GPT_HEADER_ADDR + 290], 0x0008
    jnz .extent_target
    mov ax, [es:GPT_HEADER_ADDR + 296]
    mov dx, [es:GPT_HEADER_ADDR + 298]
    jmp short .have_block

.extent_target:
    mov ax, [es:GPT_HEADER_ADDR + 316]
    mov dx, [es:GPT_HEADER_ADDR + 318]

.have_block:
    or ax, dx
    jz .done
    cmp byte [es:GPT_ENTRY_SECTOR_ADDR + 24], 0
    jne .block_2k
    shl ax, 1
    rcl dx, 1
    jmp short .have_lba

.block_2k:
    shl ax, 1
    rcl dx, 1
    shl ax, 1
    rcl dx, 1

.have_lba:
    add ax, [es:BOOT_INFO_ADDR + 4]
    adc dx, [es:BOOT_INFO_ADDR + 6]
    mov word [boot_probe_dap - menu_loader_entry + 4], CHAINLOAD_BUFFER_ADDR
    mov [boot_probe_dap - menu_loader_entry + 8], ax
    mov [boot_probe_dap - menu_loader_entry + 10], dx

    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done
    or byte [es:BOOT_INFO_ADDR + 2], BOOT_INFO_FLAG_ROOT_DIR_VALID
    call maybe_follow_boot_dir

.done:
    ret

maybe_read_fat_root_dir:
    cmp byte [es:BOOT_INFO_ADDR + 0], BOOT_TARGET_LINUX
    jne .done
    test byte [es:BOOT_INFO_ADDR + 2], BOOT_INFO_FLAG_CHAINLOAD_READ_OK
    jz .done
    cmp word [es:CHAINLOAD_BUFFER_ADDR + 11], 512
    jne .done
    cmp byte [es:CHAINLOAD_BUFFER_ADDR + 13], 0
    je .done
    cmp byte [es:CHAINLOAD_BUFFER_ADDR + 16], 0
    je .done
    cmp dword [es:CHAINLOAD_BUFFER_ADDR + 36], 0
    je .done
    cmp dword [es:CHAINLOAD_BUFFER_ADDR + 44], 2
    jb .done

    movzx eax, word [es:CHAINLOAD_BUFFER_ADDR + 14]
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 16]
    mov edx, [es:CHAINLOAD_BUFFER_ADDR + 36]
    imul ecx, edx
    add eax, ecx

    mov edx, [es:CHAINLOAD_BUFFER_ADDR + 44]
    sub edx, 2
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 13]
    imul edx, ecx
    add eax, edx

    xor edx, edx
    mov dx, [es:BOOT_INFO_ADDR + 6]
    shl edx, 16
    mov dx, [es:BOOT_INFO_ADDR + 4]
    add eax, edx

    mov word [boot_probe_dap - menu_loader_entry + 4], GPT_HEADER_ADDR
    mov word [boot_probe_dap - menu_loader_entry + 8], ax
    shr eax, 16
    mov word [boot_probe_dap - menu_loader_entry + 10], ax

    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done
    or byte [es:BOOT_INFO_ADDR + 2], BOOT_INFO_FLAG_ROOT_DIR_VALID
    call maybe_follow_fat_loader
    call maybe_follow_fat_efi

.done:
    ret

maybe_follow_fat_loader:
    mov di, GPT_HEADER_ADDR

.find_entry:
    cmp di, GPT_HEADER_ADDR + 512
    jae .done
    cmp byte [es:di], 0x00
    je .done
    cmp byte [es:di], 0xE5
    je .next_entry
    cmp byte [es:di + 11], 0x0F
    je .next_entry
    test byte [es:di + 11], 0x10
    jz .next_entry
    cmp byte [es:di + 0], 'L'
    jne .next_entry
    cmp byte [es:di + 1], 'O'
    jne .next_entry
    cmp byte [es:di + 2], 'A'
    jne .next_entry
    cmp byte [es:di + 3], 'D'
    jne .next_entry
    cmp byte [es:di + 4], 'E'
    jne .next_entry
    cmp byte [es:di + 5], 'R'
    jne .next_entry
    cmp byte [es:di + 6], ' '
    jne .next_entry
    cmp byte [es:di + 7], ' '
    jne .next_entry
    cmp byte [es:di + 8], ' '
    jne .next_entry
    cmp byte [es:di + 9], ' '
    jne .next_entry
    cmp byte [es:di + 10], ' '
    je .loader_found

.next_entry:
    add di, 32
    jmp .find_entry

.loader_found:
    xor eax, eax
    mov ax, [es:di + 26]
    xor edx, edx
    mov dx, [es:di + 20]
    shl edx, 16
    or eax, edx
    cmp eax, 2
    jb .done
    sub eax, 2
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 13]
    imul eax, ecx
    movzx ecx, word [es:CHAINLOAD_BUFFER_ADDR + 14]
    add eax, ecx
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 16]
    mov edx, [es:CHAINLOAD_BUFFER_ADDR + 36]
    imul ecx, edx
    add eax, ecx
    xor edx, edx
    mov dx, [es:BOOT_INFO_ADDR + 6]
    shl edx, 16
    mov dx, [es:BOOT_INFO_ADDR + 4]
    add eax, edx

    mov word [boot_probe_dap - menu_loader_entry + 4], CHILD_DIR_SECTOR_ADDR
    mov word [boot_probe_dap - menu_loader_entry + 8], ax
    shr eax, 16
    mov word [boot_probe_dap - menu_loader_entry + 10], ax

    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done
    or byte [es:BOOT_INFO_ADDR + 2], BOOT_INFO_FLAG_CHILD_DIR_VALID
    call maybe_follow_fat_entries

.done:
    ret

maybe_follow_fat_entries:
    mov di, CHILD_DIR_SECTOR_ADDR

.find_entry:
    cmp di, CHILD_DIR_SECTOR_ADDR + 512
    jae .done
    cmp byte [es:di], 0x00
    je .done
    cmp byte [es:di], 0xE5
    je .next_entry
    cmp byte [es:di + 11], 0x0F
    je .next_entry
    test byte [es:di + 11], 0x10
    jz .next_entry
    cmp byte [es:di + 0], 'E'
    jne .next_entry
    cmp byte [es:di + 1], 'N'
    jne .next_entry
    cmp byte [es:di + 2], 'T'
    jne .next_entry
    cmp byte [es:di + 3], 'R'
    jne .next_entry
    cmp byte [es:di + 4], 'I'
    jne .next_entry
    cmp byte [es:di + 5], 'E'
    jne .next_entry
    cmp byte [es:di + 6], 'S'
    jne .next_entry
    cmp byte [es:di + 7], ' '
    jne .next_entry
    cmp byte [es:di + 8], ' '
    jne .next_entry
    cmp byte [es:di + 9], ' '
    jne .next_entry
    cmp byte [es:di + 10], ' '
    je .entries_found

.next_entry:
    add di, 32
    jmp .find_entry

.entries_found:
    xor eax, eax
    mov ax, [es:di + 26]
    xor edx, edx
    mov dx, [es:di + 20]
    shl edx, 16
    or eax, edx
    cmp eax, 2
    jb .done
    sub eax, 2
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 13]
    imul eax, ecx
    movzx ecx, word [es:CHAINLOAD_BUFFER_ADDR + 14]
    add eax, ecx
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 16]
    mov edx, [es:CHAINLOAD_BUFFER_ADDR + 36]
    imul ecx, edx
    add eax, ecx
    xor edx, edx
    mov dx, [es:BOOT_INFO_ADDR + 6]
    shl edx, 16
    mov dx, [es:BOOT_INFO_ADDR + 4]
    add eax, edx

    mov word [boot_probe_dap - menu_loader_entry + 4], GRANDCHILD_DIR_SECTOR_ADDR
    mov word [boot_probe_dap - menu_loader_entry + 8], ax
    shr eax, 16
    mov word [boot_probe_dap - menu_loader_entry + 10], ax

    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done
    or byte [es:BOOT_INFO_ADDR + 3], BOOT_INFO_DETAIL_FAT_ENTRIES_VALID
    call maybe_read_fat_entry_file

.done:
    ret

maybe_read_fat_entry_file:
    mov di, GRANDCHILD_DIR_SECTOR_ADDR

.find_entry:
    cmp di, GRANDCHILD_DIR_SECTOR_ADDR + 512
    jae .done
    cmp byte [es:di], 0x00
    je .done
    cmp byte [es:di], 0xE5
    je .next_entry
    cmp byte [es:di + 11], 0x0F
    je .next_entry
    test byte [es:di + 11], 0x10
    jnz .next_entry
    cmp byte [es:di + 8], 'C'
    jne .next_entry
    cmp byte [es:di + 9], 'O'
    jne .next_entry
    cmp byte [es:di + 10], 'N'
    je .file_found

.next_entry:
    add di, 32
    jmp .find_entry

.file_found:
    xor eax, eax
    mov ax, [es:di + 26]
    xor edx, edx
    mov dx, [es:di + 20]
    shl edx, 16
    or eax, edx
    cmp eax, 2
    jb .done
    sub eax, 2
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 13]
    imul eax, ecx
    movzx ecx, word [es:CHAINLOAD_BUFFER_ADDR + 14]
    add eax, ecx
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 16]
    mov edx, [es:CHAINLOAD_BUFFER_ADDR + 36]
    imul ecx, edx
    add eax, ecx
    xor edx, edx
    mov dx, [es:BOOT_INFO_ADDR + 6]
    shl edx, 16
    mov dx, [es:BOOT_INFO_ADDR + 4]
    add eax, edx

    mov word [boot_probe_dap - menu_loader_entry + 4], CONFIG_FILE_INODE_SECTOR_ADDR
    mov word [boot_probe_dap - menu_loader_entry + 8], ax
    shr eax, 16
    mov word [boot_probe_dap - menu_loader_entry + 10], ax

    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done
    or byte [es:BOOT_INFO_ADDR + 3], BOOT_INFO_DETAIL_FAT_ENTRY_FILE_VALID
    call maybe_follow_fat_entry_targets

.done:
    ret

maybe_follow_fat_entry_targets:
    mov si, linux_entry_keyword - menu_loader_entry
    mov di, entry_path_buffer - menu_loader_entry
    call extract_entry_path_component
    jc .try_initrd
    mov bx, LINUX_KERNEL_SECTOR_ADDR
    mov cx, 2
    mov al, BOOT_INFO_DETAIL_FAT_KERNEL_VALID
    call read_fat_root_file_for_entry_path

.try_initrd:
    mov si, initrd_entry_keyword - menu_loader_entry
    mov di, entry_path_buffer - menu_loader_entry
    call extract_entry_path_component
    jc .done
    mov bx, LINUX_INITRD_SECTOR_ADDR
    mov cx, 1
    mov al, BOOT_INFO_DETAIL_FAT_INITRD_VALID
    call read_fat_root_file_for_entry_path

.done:
    ret

maybe_follow_fat_efi:
    mov si, fat_efi_dir_name - menu_loader_entry
    mov di, GPT_HEADER_ADDR
    mov bx, FAT_EFI_DIR_SECTOR_ADDR
    mov cx, 1
    mov dx, BOOT_INFO_EXTRA_FLAGS_ADDR
    mov al, BOOT_INFO_EXTRA_FAT_EFI_DIR_VALID
    call read_fat_named_sector
    jc .done
    call maybe_follow_fat_efi_boot
    call maybe_follow_fat_efi_systemd

.done:
    ret

maybe_follow_fat_efi_boot:
    mov si, fat_boot_dir_name - menu_loader_entry
    mov di, FAT_EFI_DIR_SECTOR_ADDR
    mov bx, FAT_EFI_BOOT_DIR_SECTOR_ADDR
    mov cx, 1
    mov dx, BOOT_INFO_EXTRA_FLAGS_ADDR
    mov al, BOOT_INFO_EXTRA_FAT_EFI_BOOT_DIR_VALID
    call read_fat_named_sector
    jc .done

    mov si, fat_bootx64_file_name - menu_loader_entry
    mov di, FAT_EFI_BOOT_DIR_SECTOR_ADDR
    mov bx, FAT_EFI_BOOT_FILE_SECTOR_ADDR
    mov cx, 1
    mov dx, BOOT_INFO_EXTRA_FLAGS_ADDR
    mov al, BOOT_INFO_EXTRA_FAT_EFI_BOOT_FILE_VALID
    call read_fat_named_sector

.done:
    ret

maybe_follow_fat_efi_systemd:
    mov si, fat_systemd_dir_name - menu_loader_entry
    mov di, FAT_EFI_DIR_SECTOR_ADDR
    mov bx, FAT_EFI_SYSTEMD_DIR_SECTOR_ADDR
    mov cx, 1
    mov dx, BOOT_INFO_EXTRA_FLAGS_ADDR
    mov al, BOOT_INFO_EXTRA_FAT_EFI_SYSTEMD_DIR_VALID
    call read_fat_named_sector
    jc .done

    mov si, fat_systemd_boot_file_name - menu_loader_entry
    mov di, FAT_EFI_SYSTEMD_DIR_SECTOR_ADDR
    mov bx, FAT_EFI_SYSTEMD_FILE_SECTOR_ADDR
    mov cx, 1
    mov dx, BOOT_INFO_EXTRA_FLAGS_ADDR
    mov al, BOOT_INFO_EXTRA_FAT_EFI_SYSTEMD_FILE_VALID
    call read_fat_named_sector

.done:
    ret

maybe_probe_windows_drive:
    mov bl, 0x80

.scan_drive:
    cmp bl, 0x84
    jae .done
    cmp bl, [es:BOOT_INFO_ADDR + 1]
    je .next_drive

    mov byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], 0
    mov byte [es:BOOT_INFO_WINDOWS_DRIVE_ADDR], 0
    mov [windows_probe_drive - menu_loader_entry], bl

    xor ax, ax
    xor dx, dx
    mov bx, WINDOWS_PROBE_MBR_ADDR
    mov cx, 1
    call read_windows_probe_lba
    jc .next_drive
    cmp word [es:WINDOWS_PROBE_MBR_ADDR + 510], 0xAA55
    jne .next_drive

    mov byte [es:BOOT_INFO_WINDOWS_DRIVE_ADDR], bl
    or byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_MBR_VALID
    call maybe_read_windows_mbr_vbrs
    call maybe_read_windows_gpt

    test byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], 0x1C
    jnz .done

.next_drive:
    inc bl
    jmp .scan_drive

.done:
    ret

maybe_read_windows_mbr_vbrs:
    mov di, WINDOWS_PROBE_MBR_ADDR + 446
    mov cx, 4

.find_entry:
    cmp byte [es:di + 4], 0x0B
    je .maybe_esp
    cmp byte [es:di + 4], 0x0C
    je .maybe_esp
    cmp byte [es:di + 4], 0x07
    je .maybe_data
    jmp .next_entry

.maybe_esp:
    test byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_ESP_VBR_VALID
    jnz .next_entry
    mov ax, [es:di + 8]
    mov dx, [es:di + 10]
    mov bx, WINDOWS_PROBE_ESP_VBR_ADDR
    mov cx, 1
    call read_windows_probe_lba
    jc .next_entry
    or byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_ESP_VBR_VALID
    jmp .next_entry

.maybe_data:
    test byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_DATA_VBR_VALID
    jnz .next_entry
    mov ax, [es:di + 8]
    mov dx, [es:di + 10]
    mov bx, WINDOWS_PROBE_DATA_VBR_ADDR
    mov cx, 1
    call read_windows_probe_lba
    jc .next_entry
    or byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_DATA_VBR_VALID

.next_entry:
    add di, 16
    loop .find_entry
    ret

maybe_read_windows_gpt:
    mov di, WINDOWS_PROBE_MBR_ADDR + 446
    mov cx, 4

.find_protective:
    cmp byte [es:di + 4], 0xEE
    je .read_header
    add di, 16
    loop .find_protective
    ret

.read_header:
    mov ax, 1
    xor dx, dx
    mov bx, WINDOWS_PROBE_GPT_HEADER_ADDR
    mov cx, 1
    call read_windows_probe_lba
    jc .done
    cmp byte [es:WINDOWS_PROBE_GPT_HEADER_ADDR + 0], 'E'
    jne .done
    cmp byte [es:WINDOWS_PROBE_GPT_HEADER_ADDR + 1], 'F'
    jne .done
    cmp byte [es:WINDOWS_PROBE_GPT_HEADER_ADDR + 2], 'I'
    jne .done
    cmp byte [es:WINDOWS_PROBE_GPT_HEADER_ADDR + 3], ' '
    jne .done
    cmp byte [es:WINDOWS_PROBE_GPT_HEADER_ADDR + 4], 'P'
    jne .done
    cmp byte [es:WINDOWS_PROBE_GPT_HEADER_ADDR + 5], 'A'
    jne .done
    cmp byte [es:WINDOWS_PROBE_GPT_HEADER_ADDR + 6], 'R'
    jne .done
    cmp byte [es:WINDOWS_PROBE_GPT_HEADER_ADDR + 7], 'T'
    jne .done
    or byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_GPT_HEADER_VALID

    mov ax, 2
    xor dx, dx
    mov bx, WINDOWS_PROBE_GPT_ENTRY_SECTOR_ADDR
    mov cx, 1
    call read_windows_probe_lba
    jc .done
    or byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_GPT_ENTRY_VALID
    call maybe_read_windows_gpt_vbrs

.done:
    ret

maybe_read_windows_gpt_vbrs:
    mov di, WINDOWS_PROBE_GPT_ENTRY_SECTOR_ADDR
    mov cx, 4

.find_entry:
    cmp dword [es:di + 0], 0
    jne .check_guid
    cmp dword [es:di + 4], 0
    jne .check_guid
    cmp dword [es:di + 8], 0
    jne .check_guid
    cmp dword [es:di + 12], 0
    je .next_entry

.check_guid:
    test byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_ESP_VBR_VALID
    jnz .maybe_data
    mov si, windows_efi_system_guid - menu_loader_entry
    call windows_gpt_entry_matches_guid
    jc .maybe_data
    mov ax, [es:di + 32]
    mov dx, [es:di + 34]
    mov bx, WINDOWS_PROBE_ESP_VBR_ADDR
    mov cx, 1
    call read_windows_probe_lba
    jc .maybe_data
    or byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_ESP_VBR_VALID

.maybe_data:
    test byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_DATA_VBR_VALID
    jnz .next_entry
    mov si, windows_microsoft_basic_guid - menu_loader_entry
    call windows_gpt_entry_matches_guid
    jc .next_entry
    mov ax, [es:di + 32]
    mov dx, [es:di + 34]
    mov bx, WINDOWS_PROBE_DATA_VBR_ADDR
    mov cx, 1
    call read_windows_probe_lba
    jc .next_entry
    or byte [es:BOOT_INFO_WINDOWS_FLAGS_ADDR], BOOT_INFO_WINDOWS_FLAG_DATA_VBR_VALID

.next_entry:
    add di, 128
    loop .find_entry
    ret

read_windows_probe_lba:
    mov word [boot_probe_dap - menu_loader_entry + 2], cx
    mov word [boot_probe_dap - menu_loader_entry + 4], bx
    mov word [boot_probe_dap - menu_loader_entry + 6], 0
    mov word [boot_probe_dap - menu_loader_entry + 8], ax
    mov word [boot_probe_dap - menu_loader_entry + 10], dx
    mov word [boot_probe_dap - menu_loader_entry + 12], 0
    mov word [boot_probe_dap - menu_loader_entry + 14], 0

    mov ah, 0x42
    mov dl, [windows_probe_drive - menu_loader_entry]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    ret

windows_gpt_entry_matches_guid:
    push si
    push di
    push bx
    push cx
    mov bx, di
    mov cx, 16

.compare_bytes:
    mov al, [si]
    cmp al, [es:bx]
    jne .mismatch
    inc si
    inc bx
    loop .compare_bytes
    clc
    jmp .done

.mismatch:
    stc

.done:
    pop cx
    pop bx
    pop di
    pop si
    ret

extract_entry_path_component:
    push bp
    mov bp, di
    mov cx, 64

.clear_output:
    mov byte [di], 0
    inc di
    loop .clear_output

    xor bx, bx

.find_line:
    cmp bx, 512
    jae .not_found
    cmp bx, 0
    je .check_keyword
    mov al, [es:CONFIG_FILE_INODE_SECTOR_ADDR + bx - 1]
    cmp al, 0x0A
    je .check_keyword
    cmp al, 0x0D
    jne .next_offset

.check_keyword:
    push bx
    push si
    mov di, bx

.compare_keyword:
    lodsb
    test al, al
    jz .keyword_found
    cmp al, [es:CONFIG_FILE_INODE_SECTOR_ADDR + di]
    jne .keyword_retry
    inc di
    jmp .compare_keyword

.keyword_found:
    mov bx, di

.skip_padding:
    cmp bx, 512
    jae .keyword_retry
    mov al, [es:CONFIG_FILE_INODE_SECTOR_ADDR + bx]
    cmp al, ' '
    je .skip_padding_char
    cmp al, 0x09
    jne .value_ready

.skip_padding_char:
    inc bx
    jmp .skip_padding

.value_ready:
    cmp bx, 512
    jae .keyword_retry
    mov al, [es:CONFIG_FILE_INODE_SECTOR_ADDR + bx]
    cmp al, '/'
    jne .copy_component
    inc bx

.copy_component:
    mov di, bp
    xor cx, cx

.copy_loop:
    cmp bx, 512
    jae .finish_component
    mov al, [es:CONFIG_FILE_INODE_SECTOR_ADDR + bx]
    cmp al, 0
    je .finish_component
    cmp al, 0x0A
    je .finish_component
    cmp al, 0x0D
    je .finish_component
    cmp al, ' '
    je .finish_component
    cmp al, 0x09
    je .finish_component
    cmp al, '/'
    je .finish_component
    cmp cx, 63
    jae .finish_component
    mov [di], al
    inc di
    inc bx
    inc cx
    jmp .copy_loop

.finish_component:
    mov byte [di], 0
    cmp cx, 0
    je .keyword_retry
    pop si
    pop bx
    clc
    pop bp
    ret

.keyword_retry:
    pop si
    pop bx

.next_offset:
    inc bx
    jmp .find_line

.not_found:
    mov byte [bp], 0
    stc
    pop bp
    ret

read_fat_root_file_for_entry_path:
    mov si, entry_path_buffer - menu_loader_entry
    mov di, GPT_HEADER_ADDR
    mov dx, BOOT_INFO_ADDR + 3
    jmp read_fat_named_sector

read_fat_named_sector:
    mov word [fat_target_name_ptr - menu_loader_entry], si
    mov word [fat_target_buffer - menu_loader_entry], bx
    mov word [fat_target_sector_count - menu_loader_entry], cx
    mov word [fat_target_flag_addr - menu_loader_entry], dx
    mov byte [fat_target_detail_flag - menu_loader_entry], al
    call clear_fat_long_name_buffer
    mov bp, di
    add bp, 512

.find_entry:
    cmp di, bp
    jae .done
    cmp byte [es:di], 0x00
    je .done
    cmp byte [es:di], 0xE5
    je .next_entry
    cmp byte [es:di + 11], 0x0F
    je .lfn_entry
    test byte [es:di + 11], 0x18
    jnz .next_entry
    call fat_entry_matches_target_name
    jc .next_entry

    xor eax, eax
    mov ax, [es:di + 26]
    xor edx, edx
    mov dx, [es:di + 20]
    shl edx, 16
    or eax, edx
    cmp eax, 2
    jb .next_entry
    sub eax, 2
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 13]
    imul eax, ecx
    movzx ecx, word [es:CHAINLOAD_BUFFER_ADDR + 14]
    add eax, ecx
    movzx ecx, byte [es:CHAINLOAD_BUFFER_ADDR + 16]
    mov edx, [es:CHAINLOAD_BUFFER_ADDR + 36]
    imul ecx, edx
    add eax, ecx
    xor edx, edx
    mov dx, [es:BOOT_INFO_ADDR + 6]
    shl edx, 16
    mov dx, [es:BOOT_INFO_ADDR + 4]
    add eax, edx

    mov word [boot_probe_dap - menu_loader_entry + 8], ax
    shr eax, 16
    mov word [boot_probe_dap - menu_loader_entry + 10], ax
    mov ax, [fat_target_sector_count - menu_loader_entry]
    mov word [boot_probe_dap - menu_loader_entry + 2], ax
    mov ax, [fat_target_buffer - menu_loader_entry]
    mov word [boot_probe_dap - menu_loader_entry + 4], ax

    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .next_entry
    mov bx, [fat_target_flag_addr - menu_loader_entry]
    mov al, [fat_target_detail_flag - menu_loader_entry]
    or byte [es:bx], al
    clc
    ret

.lfn_entry:
    call append_lfn_entry_to_name_buffer
    add di, 32
    jmp .find_entry

.next_entry:
    call clear_fat_long_name_buffer
    add di, 32
    jmp .find_entry

.done:
    stc
    ret

fat_entry_matches_target_name:
    cmp byte [fat_long_name_buffer - menu_loader_entry], 0
    je .short_name
    call fat_target_name_matches_long_name
    jnc .match

.short_name:
    call fat_short_name_matches_target_name
    ret

.match:
    clc
    ret

fat_target_name_matches_long_name:
    mov si, [fat_target_name_ptr - menu_loader_entry]
    mov bx, fat_long_name_buffer - menu_loader_entry

.compare_chars:
    mov al, [si]
    cmp al, 'A'
    jb .path_ready
    cmp al, 'Z'
    ja .path_ready
    add al, 'a' - 'A'

.path_ready:
    mov ah, [bx]
    cmp ah, 'A'
    jb .name_ready
    cmp ah, 'Z'
    ja .name_ready
    add ah, 'a' - 'A'

.name_ready:
    cmp al, ah
    jne .mismatch
    test al, al
    je .match
    inc si
    inc bx
    jmp .compare_chars

.match:
    clc
    ret

.mismatch:
    stc
    ret

fat_short_name_matches_target_name:
    mov si, [fat_target_name_ptr - menu_loader_entry]
    xor bx, bx

.base_chars:
    mov al, [si]
    cmp al, 0
    je .base_done
    cmp al, '.'
    je .base_done
    cmp bx, 8
    jae .mismatch
    mov ah, [es:di + bx]
    cmp ah, ' '
    je .mismatch
    cmp al, 'A'
    jb .base_path_ready
    cmp al, 'Z'
    ja .base_path_ready
    add al, 'a' - 'A'

.base_path_ready:
    cmp ah, 'A'
    jb .base_name_ready
    cmp ah, 'Z'
    ja .base_name_ready
    add ah, 'a' - 'A'

.base_name_ready:
    cmp al, ah
    jne .mismatch
    inc si
    inc bx
    jmp .base_chars

.base_done:
.base_spaces:
    cmp bx, 8
    jae .check_extension
    cmp byte [es:di + bx], ' '
    jne .mismatch
    inc bx
    jmp .base_spaces

.check_extension:
    cmp byte [si], '.'
    je .have_extension
    cmp byte [si], 0
    jne .mismatch
    xor bx, bx

.no_extension:
    cmp bx, 3
    jae .match
    cmp byte [es:di + bx + 8], ' '
    jne .mismatch
    inc bx
    jmp .no_extension

.have_extension:
    inc si
    xor bx, bx

.extension_chars:
    mov al, [si]
    cmp al, 0
    je .extension_done
    cmp bx, 3
    jae .mismatch
    mov ah, [es:di + bx + 8]
    cmp ah, ' '
    je .mismatch
    cmp al, 'A'
    jb .ext_path_ready
    cmp al, 'Z'
    ja .ext_path_ready
    add al, 'a' - 'A'

.ext_path_ready:
    cmp ah, 'A'
    jb .ext_name_ready
    cmp ah, 'Z'
    ja .ext_name_ready
    add ah, 'a' - 'A'

.ext_name_ready:
    cmp al, ah
    jne .mismatch
    inc si
    inc bx
    jmp .extension_chars

.extension_done:
.extension_spaces:
    cmp bx, 3
    jae .match
    cmp byte [es:di + bx + 8], ' '
    jne .mismatch
    inc bx
    jmp .extension_spaces

.match:
    clc
    ret

.mismatch:
    stc
    ret

clear_fat_long_name_buffer:
    push ax
    push cx
    push di
    mov di, fat_long_name_buffer - menu_loader_entry
    mov cx, 80

.clear_char:
    mov byte [di], 0
    inc di
    loop .clear_char

    pop di
    pop cx
    pop ax
    ret

append_lfn_entry_to_name_buffer:
    test byte [es:di], 0x40
    jz .have_sequence
    call clear_fat_long_name_buffer

.have_sequence:
    mov al, [es:di]
    and al, 0x1F
    jz .done
    dec al
    mov bl, 13
    mul bl
    mov bx, ax

    mov si, 1
    call store_lfn_word
    inc bx
    mov si, 3
    call store_lfn_word
    inc bx
    mov si, 5
    call store_lfn_word
    inc bx
    mov si, 7
    call store_lfn_word
    inc bx
    mov si, 9
    call store_lfn_word
    inc bx
    mov si, 14
    call store_lfn_word
    inc bx
    mov si, 16
    call store_lfn_word
    inc bx
    mov si, 18
    call store_lfn_word
    inc bx
    mov si, 20
    call store_lfn_word
    inc bx
    mov si, 22
    call store_lfn_word
    inc bx
    mov si, 24
    call store_lfn_word
    inc bx
    mov si, 28
    call store_lfn_word
    inc bx
    mov si, 30
    call store_lfn_word

.done:
    ret

store_lfn_word:
    push ax
    cmp bx, 79
    jae .done
    push bx
    mov bx, di
    add bx, si
    mov ax, [es:bx]
    pop bx
    cmp ax, 0x0000
    je .done
    cmp ax, 0xFFFF
    je .done
    cmp ah, 0x00
    jne .not_ascii
    mov [bx + fat_long_name_buffer - menu_loader_entry], al
    jmp .done

.not_ascii:
    mov byte [bx + fat_long_name_buffer - menu_loader_entry], '?'

.done:
    pop ax
    ret

maybe_follow_boot_dir:
    mov di, CHAINLOAD_BUFFER_ADDR

.find_entry:
    cmp di, CHAINLOAD_BUFFER_ADDR + 512
    jae .done
    cmp word [es:di + 4], 8
    jb .done
    mov bx, di
    add bx, [es:di + 4]
    cmp bx, CHAINLOAD_BUFFER_ADDR + 512
    ja .done
    cmp dword [es:di], 0
    je .next_entry
    cmp byte [es:di + 6], 4
    jne .next_entry
    cmp byte [es:di + 8], 'b'
    jne .next_entry
    cmp byte [es:di + 9], 'o'
    jne .next_entry
    cmp byte [es:di + 10], 'o'
    jne .next_entry
    cmp byte [es:di + 11], 't'
    je .boot_found

.next_entry:
    add di, [es:di + 4]
    jmp .find_entry

.boot_found:
    mov ax, [es:di]
    mov dx, [es:di + 2]
    or dx, dx
    jnz .done
    dec ax
    cmp ax, [es:GPT_ENTRY_SECTOR_ADDR + 40]
    jae .done
    mov bx, ax
    and bx, 1
    shl bx, 8
    shr ax, 1
    add ax, [es:BOOT_INFO_ADDR + 8]
    adc dx, [es:BOOT_INFO_ADDR + 10]
    mov word [boot_probe_dap - menu_loader_entry + 4], CHILD_INODE_SECTOR_ADDR
    mov [boot_probe_dap - menu_loader_entry + 8], ax
    mov [boot_probe_dap - menu_loader_entry + 10], dx
    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done

    test word [es:bx + CHILD_INODE_SECTOR_ADDR + 32], 0x0008
    jnz .extent_dir
    mov ax, [es:bx + CHILD_INODE_SECTOR_ADDR + 40]
    mov dx, [es:bx + CHILD_INODE_SECTOR_ADDR + 42]
    jmp short .have_dir_block

.extent_dir:
    cmp word [es:bx + CHILD_INODE_SECTOR_ADDR + 40], 0xF30A
    jne .done
    cmp word [es:bx + CHILD_INODE_SECTOR_ADDR + 46], 0
    jne .done
    mov ax, [es:bx + CHILD_INODE_SECTOR_ADDR + 60]
    mov dx, [es:bx + CHILD_INODE_SECTOR_ADDR + 62]

.have_dir_block:
    or ax, dx
    jz .done
    cmp byte [es:GPT_ENTRY_SECTOR_ADDR + 24], 0
    jne .dir_2k
    shl ax, 1
    rcl dx, 1
    jmp short .dir_lba_ready

.dir_2k:
    shl ax, 1
    rcl dx, 1
    shl ax, 1
    rcl dx, 1

.dir_lba_ready:
    add ax, [es:BOOT_INFO_ADDR + 4]
    adc dx, [es:BOOT_INFO_ADDR + 6]
    mov word [boot_probe_dap - menu_loader_entry + 4], CHILD_DIR_SECTOR_ADDR
    mov [boot_probe_dap - menu_loader_entry + 8], ax
    mov [boot_probe_dap - menu_loader_entry + 10], dx
    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done
    or byte [es:BOOT_INFO_ADDR + 2], BOOT_INFO_FLAG_CHILD_DIR_VALID
    call maybe_follow_grub_dir

.done:
    ret

maybe_follow_grub_dir:
    mov di, CHILD_DIR_SECTOR_ADDR

.find_entry:
    cmp di, CHILD_DIR_SECTOR_ADDR + 512
    jae .done
    cmp word [es:di + 4], 8
    jb .done
    mov bx, di
    add bx, [es:di + 4]
    cmp bx, CHILD_DIR_SECTOR_ADDR + 512
    ja .done
    cmp dword [es:di], 0
    je .next_entry
    cmp byte [es:di + 6], 4
    jne .next_entry
    cmp byte [es:di + 8], 'g'
    jne .next_entry
    cmp byte [es:di + 9], 'r'
    jne .next_entry
    cmp byte [es:di + 10], 'u'
    jne .next_entry
    cmp byte [es:di + 11], 'b'
    je .grub_found

.next_entry:
    add di, [es:di + 4]
    jmp .find_entry

.grub_found:
    mov ax, [es:di]
    mov dx, [es:di + 2]
    or dx, dx
    jnz .done
    dec ax
    cmp ax, [es:GPT_ENTRY_SECTOR_ADDR + 40]
    jae .done
    mov bx, ax
    and bx, 1
    shl bx, 8
    shr ax, 1
    add ax, [es:BOOT_INFO_ADDR + 8]
    adc dx, [es:BOOT_INFO_ADDR + 10]
    mov word [boot_probe_dap - menu_loader_entry + 4], GRANDCHILD_INODE_SECTOR_ADDR
    mov [boot_probe_dap - menu_loader_entry + 8], ax
    mov [boot_probe_dap - menu_loader_entry + 10], dx
    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done

    test word [es:bx + GRANDCHILD_INODE_SECTOR_ADDR + 32], 0x0008
    jnz .extent_dir
    mov ax, [es:bx + GRANDCHILD_INODE_SECTOR_ADDR + 40]
    mov dx, [es:bx + GRANDCHILD_INODE_SECTOR_ADDR + 42]
    jmp short .have_dir_block

.extent_dir:
    cmp word [es:bx + GRANDCHILD_INODE_SECTOR_ADDR + 40], 0xF30A
    jne .done
    cmp word [es:bx + GRANDCHILD_INODE_SECTOR_ADDR + 46], 0
    jne .done
    mov ax, [es:bx + GRANDCHILD_INODE_SECTOR_ADDR + 60]
    mov dx, [es:bx + GRANDCHILD_INODE_SECTOR_ADDR + 62]

.have_dir_block:
    or ax, dx
    jz .done
    cmp byte [es:GPT_ENTRY_SECTOR_ADDR + 24], 0
    jne .dir_2k
    shl ax, 1
    rcl dx, 1
    jmp short .dir_lba_ready

.dir_2k:
    shl ax, 1
    rcl dx, 1
    shl ax, 1
    rcl dx, 1

.dir_lba_ready:
    add ax, [es:BOOT_INFO_ADDR + 4]
    adc dx, [es:BOOT_INFO_ADDR + 6]
    mov word [boot_probe_dap - menu_loader_entry + 4], GRANDCHILD_DIR_SECTOR_ADDR
    mov [boot_probe_dap - menu_loader_entry + 8], ax
    mov [boot_probe_dap - menu_loader_entry + 10], dx
    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done
    or byte [es:BOOT_INFO_ADDR + 3], BOOT_INFO_DETAIL_GRUB_DIR_VALID
    call maybe_follow_grub_cfg

.done:
    ret

maybe_follow_grub_cfg:
    mov di, GRANDCHILD_DIR_SECTOR_ADDR

.find_entry:
    cmp di, GRANDCHILD_DIR_SECTOR_ADDR + 512
    jae .done
    cmp word [es:di + 4], 8
    jb .done
    mov bx, di
    add bx, [es:di + 4]
    cmp bx, GRANDCHILD_DIR_SECTOR_ADDR + 512
    ja .done
    cmp dword [es:di], 0
    je .next_entry
    cmp byte [es:di + 6], 8
    jne .next_entry
    cmp byte [es:di + 8], 'g'
    jne .next_entry
    cmp byte [es:di + 9], 'r'
    jne .next_entry
    cmp byte [es:di + 10], 'u'
    jne .next_entry
    cmp byte [es:di + 11], 'b'
    jne .next_entry
    cmp byte [es:di + 12], '.'
    jne .next_entry
    cmp byte [es:di + 13], 'c'
    jne .next_entry
    cmp byte [es:di + 14], 'f'
    jne .next_entry
    cmp byte [es:di + 15], 'g'
    je .cfg_found

.next_entry:
    add di, [es:di + 4]
    jmp .find_entry

.cfg_found:
    mov ax, [es:di]
    mov dx, [es:di + 2]
    or dx, dx
    jnz .done
    dec ax
    cmp ax, [es:GPT_ENTRY_SECTOR_ADDR + 40]
    jae .done
    mov bx, ax
    and bx, 1
    shl bx, 8
    shr ax, 1
    add ax, [es:BOOT_INFO_ADDR + 8]
    adc dx, [es:BOOT_INFO_ADDR + 10]
    mov word [boot_probe_dap - menu_loader_entry + 4], CONFIG_FILE_INODE_SECTOR_ADDR
    mov [boot_probe_dap - menu_loader_entry + 8], ax
    mov [boot_probe_dap - menu_loader_entry + 10], dx
    mov ah, 0x42
    mov dl, [es:BOOT_INFO_ADDR + 1]
    mov si, boot_probe_dap - menu_loader_entry
    int 0x13
    jc .done
    cmp bx, 0
    je .mark_valid
    mov si, CONFIG_FILE_INODE_SECTOR_ADDR + 256
    mov di, CONFIG_FILE_INODE_SECTOR_ADDR
    mov cx, 128
    rep movsw

.mark_valid:
    or byte [es:BOOT_INFO_ADDR + 3], BOOT_INFO_DETAIL_GRUB_CFG_VALID

.done:
    ret

align 4
boot_probe_dap:
    db 0x10
    db 0x00
    dw 1
    dw CHAINLOAD_BUFFER_ADDR
    dw 0x0000
    dq 0x0000000000000000

linux_entry_keyword db "linux ", 0
initrd_entry_keyword db "initrd ", 0
fat_efi_dir_name db "EFI", 0
fat_boot_dir_name db "BOOT", 0
fat_systemd_dir_name db "SYSTEMD", 0
fat_bootx64_file_name db "BOOTX64.EFI", 0
fat_systemd_boot_file_name db "systemd-bootx64.efi", 0
entry_path_buffer times 64 db 0
fat_long_name_buffer times 80 db 0
fat_target_name_ptr dw 0
fat_target_buffer dw 0
fat_target_sector_count dw 0
fat_target_detail_flag db 0
fat_target_flag_addr dw 0
windows_probe_drive db 0
windows_efi_system_guid db 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11
                        db 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
windows_microsoft_basic_guid db 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44
                             db 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7

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
    mov esp, MENU_LOADER_STACK_TOP
    mov ebp, esp

    call boot_menu_main

.hang:
    hlt
    jmp .hang
