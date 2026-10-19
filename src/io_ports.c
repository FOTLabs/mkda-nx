#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Xbox ACPI GPIO starts at PMBASE + 0xC0. xemu's hw/xbox/acpi_xbox.c
 * reads the TV field pin in bit 5 and ignores writes at offset zero.
 * All other ports remain traps until their device behavior is established. */
uint32_t recomp_port_read(uint32_t port, unsigned width, uint32_t guest_pc)
{
    static unsigned field;
    if (port == 0x80c0u && width == 1) {
        field ^= 1;
        return field << 5;
    }
    fprintf(stderr, "[MKDA] Unknown IN port=%04X width=%u at %08X\n",
            port, width, guest_pc);
    abort();
}

void recomp_port_write(uint32_t port, uint32_t value, unsigned width, uint32_t guest_pc)
{
    if (port == 0x80c0u && width == 1) {
        fprintf(stderr, "[MKDA] ACPI field GPIO write=%02X at %08X\n", value, guest_pc);
        return;
    }
    fprintf(stderr, "[MKDA] Unknown OUT port=%04X value=%08X width=%u at %08X\n",
            port, value, width, guest_pc);
    abort();
}
