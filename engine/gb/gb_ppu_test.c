#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "gb_ppu.h"

int main(void) {
    printf("Starting NativeRa PPU Timing Tests...\n");

    gb_ppu_t ppu;
    gb_ppu_init(&ppu);

    uint8_t io_regs[0x80];
    memset(io_regs, 0, sizeof(io_regs));
    io_regs[0x40 & 0x7F] = 0x80; /* Enable LCD */

    /* Step PPU by 456 dots (1 scanline) */
    uint8_t ints = gb_ppu_step(&ppu, 456, io_regs);
    (void)ints;
    assert(ppu.ly == 1);

    printf("PPU Timing Tests PASSED!\n");
    return 0;
}
