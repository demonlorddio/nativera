#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "gb_mmu.h"

int main(void) {
    printf("Starting NativeRa MMU Known Answer Tests (KATs)...\n");

    gb_mmu_t mmu;
    uint8_t rom[0x8000];
    memset(rom, 0xAA, sizeof(rom));

    /* Mock ROM type for MBC1 (0x01) */
    rom[0x0147] = 0x01;

    gb_mmu_init(&mmu, rom, sizeof(rom), NULL, 0);

    /* Test ROM read */
    assert(gb_mmu_read(&mmu, 0x0100) == 0xAA);

    /* Test Banking */
    gb_mmu_write(&mmu, 0x2000, 0x02); /* Set ROM bank to 2 */
    assert(mmu.rom_bank == 2);

    printf("ALL NATIVERA MMU KATs PASSED SUCCESSFULLY!\n");
    return 0;
}
