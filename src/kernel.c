#include "kernel.h"

void kernel_main() {
    // Initialize the terminal interface
    terminal_initialize();

    // Print a welcome message
    terminal_writestring("Welcome to the kernel!\n");
}