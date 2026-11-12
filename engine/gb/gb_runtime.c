#include "gb_runtime.h"

void gb_runtime_init(gb_runtime_t *runtime, uint8_t *rom_data, uint32_t rom_size, uint8_t *sram_data, uint32_t sram_size) {
    gb_cpu_init(&runtime->cpu);
    gb_mmu_init(&runtime->mmu, rom_data, rom_size, sram_data, sram_size);
    runtime->running = true;
}

void gb_runtime_free(gb_runtime_t *runtime) {
    gb_mmu_free(&runtime->mmu);
    runtime->running = false;
}

int gb_runtime_step(gb_runtime_t *runtime) {
    gb_context_t ctx = {
        .cpu = &runtime->cpu,
        .mmu = &runtime->mmu
    };
    return gb_cpu_step(&runtime->cpu, &ctx, gb_mmu_read, gb_mmu_write);
}

void gb_runtime_run_cycles(gb_runtime_t *runtime, uint64_t target_cycles) {
    uint64_t start_cycles = runtime->cpu.cycles;
    while (runtime->running && (runtime->cpu.cycles - start_cycles) < target_cycles) {
        gb_runtime_step(runtime);
    }
}
