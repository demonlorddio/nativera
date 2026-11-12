#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb_runtime.h"
#include "gb_ppu.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <rom.gb>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        fprintf(stderr, "Error opening ROM file: %s\n", argv[1]);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *rom = malloc(size);
    if (!rom) {
        fclose(f);
        fprintf(stderr, "Memory allocation failed for ROM\n");
        return 1;
    }

    fread(rom, 1, size, f);
    fclose(f);

    printf("Loaded ROM: %s (%ld bytes)\n", argv[1], size);

    gb_runtime_t runtime;
    gb_ppu_t ppu;

    gb_runtime_init(&runtime, rom, size, NULL, 0);
    gb_ppu_init(&ppu);

    printf("Starting NativeRa Game Boy Emulator loop...\n");

    /* Run for 60 frames (approx 1 second of emulation) */
    for (int frame = 0; frame < 60; frame++) {
        uint64_t target_cycles = runtime.cpu.cycles + 70224;
        while (runtime.cpu.cycles < target_cycles) {
            int cycles = gb_runtime_step(&runtime);
            gb_ppu_step(&ppu, cycles, runtime.mmu.io);
        }
    }

    printf("Emulation executed successfully for 60 frames (PC: 0x%04X, Cycles: %lu)\n",
           runtime.cpu.pc, (unsigned long)runtime.cpu.cycles);

    gb_runtime_free(&runtime);
    free(rom);
    return 0;
}
