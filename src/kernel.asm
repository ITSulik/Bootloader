[BITS 32]

global _start ; Entry point for the kernel

export kernel_main ; Export the kernel_main function so it can be called from assembly

_start:
    call kernel_main ; Call the kernel main function

    jmp $


times 510 - ($ - $$) db 0 ; Pad the rest of the boot sector with zeros