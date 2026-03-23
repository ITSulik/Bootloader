[BITS 32]

section .text ; Code section for the menu loader, which will be loaded by the boot sector and run in 32-bit protected mode

global menu_loader_entry
extern boot_menu_main

MENU_LOADER_STACK_TOP equ 0x90000

; Stage 2 will eventually run after the 16-bit boot sector has:
; 1. read a larger loader from disk
; 2. switched the CPU into 32-bit protected mode
; 3. jumped to this entry point
menu_loader_entry:
    cli

    ; Give the C code a clean stack before we hand control over to it.
    mov esp, MENU_LOADER_STACK_TOP ; Set up a stack for the menu loader
    mov ebp, esp ; Set the base pointer to the same value for good measure

    call boot_menu_main ; Jump to the C code that will display the menu and handle user input

.hang:
    ; There is nowhere to return to yet, so stop here until the machine resets.
    hlt ; Halt the CPU until the next external interrupt (e.g. a reset)
    jmp .hang ; Just in case an interrupt does happen, loop back to the halt instruction
