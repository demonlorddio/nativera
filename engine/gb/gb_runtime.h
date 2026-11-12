#ifndef NATIVERA_GB_RUNTIME_H
#define NATIVERA_GB_RUNTIME_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_cpu.h"
#include "gb_mmu.h"

typedef struct {
    gb_cpu_t cpu;
    gb_mmu_t mmu;
    bool running;
} gb_runtime_t;

void gb_runtime_init(gb_runtime_t *runtime, uint8_t *rom_data, uint32_t rom_size, uint8_t *sram_data, uint32_t sram_size);
void gb_runtime_free(gb_runtime_t *runtime);

/* Step the system by one CPU instruction */
int gb_runtime_step(gb_runtime_t *runtime);

/* Run for a specific number of CPU cycles */
void gb_runtime_run_cycles(gb_runtime_t *runtime, uint64_t target_cycles);

#endif /* NATIVERA_GB_RUNTIME_H */
