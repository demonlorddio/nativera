#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "gb_runtime.h"

int main(void) {
    printf("Starting NativeRa Runtime Integration Test...\n");

    gb_runtime_t runtime;
    uint8_t rom[0x1000];
    memset(rom, 0, sizeof(rom));

    /* LD B, 0x12; NOP */
    rom[0x100] = 0x06;
    rom[0x101] = 0x12;
    rom[0x102] = 0x00;

    gb_runtime_init(&runtime, rom, sizeof(rom), NULL, 0);

    /* Run 2 instructions: LD B, 0x12 (8 cycles) and NOP (4 cycles) */
    gb_runtime_run_cycles(&runtime, 12);

    printf("cpu.b = 0x%02X, cpu.c = 0x%02X, cpu.bc = 0x%04X\n", runtime.cpu.b, runtime.cpu.c, runtime.cpu.bc);

    assert(runtime.cpu.b == 0x12);
    assert(runtime.cpu.pc == 0x103);
    assert(runtime.cpu.cycles == 12);

    printf("Runtime Integration Test PASSED!\n");
    return 0;
}
