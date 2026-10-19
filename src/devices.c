#include <stdio.h>
#include <stdlib.h>
#include "apu.h"
#include "xbox_memory_layout.h"
#include "usb/ohci.h"

extern MCPXAPUState *g_apu_state;

static uint32_t apu_read(void *device, uint32_t offset, unsigned size)
{
    return (uint32_t)mcpx_apu_mmio_read(device, offset, size);
}

static void apu_write(void *device, uint32_t offset, uint32_t value, unsigned size)
{
    mcpx_apu_mmio_write(device, offset, value, size);
}

int mkda_devices_init(void)
{
    g_apu_state = mcpx_apu_init_standalone(xbox_GetMemoryBase());
    if (!g_apu_state) {
        fprintf(stderr, "[MKDA] APU initialization failed\n");
        return 0;
    }
    if (xbox_MmioRegister(0xfe800000u, 0xfe880000u, apu_read, apu_write, g_apu_state)) {
        fprintf(stderr, "[MKDA] APU MMIO registration failed\n");
        return 0;
    }
    xbox_OhciInit();
    fprintf(stderr, "[MKDA] APU and USB models initialized\n");
    return 1;
}

void mkda_devices_shutdown(void)
{
    if (g_apu_state) {
        mcpx_apu_shutdown(g_apu_state);
        g_apu_state = NULL;
    }
}
