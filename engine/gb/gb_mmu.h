#ifndef NATIVERA_GB_MMU_H
#define NATIVERA_GB_MMU_H

#include <stdint.h>
#include <stdbool.h>
#include "gb_cpu.h"

typedef struct {
    uint8_t *rom;         /* Full ROM image */
    uint32_t rom_size;
    uint8_t *sram;        /* Battery backed external RAM */
    uint32_t sram_size;
    uint8_t vram[0x2000 * 2]; /* VRAM banks 0 and 1 (CGB) */
    uint8_t wram[0x1000 * 8]; /* WRAM banks 0-7 (CGB) */
    uint8_t oam[0xA0];    /* Object Attribute Memory */
    uint8_t hram[0x7F];    /* High RAM */
    uint8_t io[0x80];     /* I/O Registers */
    uint8_t ie;           /* Interrupt Enable (0xFFFF) */

    /* MBC state */
    int mbc_type;         /* 0 = ROM, 1 = MBC1, 3 = MBC3, 5 = MBC5 */
    uint16_t rom_bank;
    uint8_t ram_bank;
    bool ram_enabled;
    bool adv_banking;     /* MBC1 banking mode select */

    /* Joypad state */
    uint8_t joypad_sel;   /* P1 / 0xFF00 */
    uint8_t joypad_state; /* Button states */
} gb_mmu_t;

void gb_mmu_init(gb_mmu_t *mmu, uint8_t *rom_data, uint32_t rom_size, uint8_t *sram_data, uint32_t sram_size);
void gb_mmu_free(gb_mmu_t *mmu);

uint8_t gb_mmu_read(void *user, uint16_t addr);
void    gb_mmu_write(void *user, uint16_t addr, uint8_t val);

#endif /* NATIVERA_GB_MMU_H */
