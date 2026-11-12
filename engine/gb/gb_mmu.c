#include <string.h>
#include "gb_mmu.h"

void gb_mmu_init(gb_mmu_t *mmu, uint8_t *rom_data, uint32_t rom_size, uint8_t *sram_data, uint32_t sram_size) {
    memset(mmu, 0, sizeof(gb_mmu_t));
    mmu->rom = rom_data;
    mmu->rom_size = rom_size;
    mmu->sram = sram_data;
    mmu->sram_size = sram_size;
    mmu->rom_bank = 1;
    mmu->ram_bank = 0;
    mmu->ram_enabled = false;

    if (rom_size > 0x147) {
        uint8_t cart_type = rom_data[0x0147];
        if (cart_type >= 0x01 && cart_type <= 0x03) mmu->mbc_type = 1;
        else if (cart_type >= 0x0F && cart_type <= 0x13) mmu->mbc_type = 3;
        else if (cart_type >= 0x19 && cart_type <= 0x1E) mmu->mbc_type = 5;
        else mmu->mbc_type = 0;
    }
}

void gb_mmu_free(gb_mmu_t *mmu) {
    (void)mmu;
}

uint8_t gb_mmu_read(void *user, uint16_t addr) {
    gb_mmu_t *mmu = (gb_mmu_t *)user;

    if (addr < 0x4000) {
        /* ROM Bank 0 */
        return mmu->rom ? mmu->rom[addr % mmu->rom_size] : 0xFF;
    }
    if (addr < 0x8000) {
        /* ROM Bank 1-N */
        uint32_t bank = mmu->rom_bank == 0 ? 1 : mmu->rom_bank;
        uint32_t offset = (bank * 0x4000) + (addr - 0x4000);
        return mmu->rom ? mmu->rom[offset % mmu->rom_size] : 0xFF;
    }
    if (addr < 0xA000) {
        /* VRAM (0x8000 - 0x9FFF) */
        uint8_t vram_bank = (mmu->io[0x4F & 0x7F] & 0x01); /* VBK register at 0xFF4F */
        uint32_t offset = (vram_bank * 0x2000) + (addr - 0x8000);
        return mmu->vram[offset];
    }
    if (addr < 0xC000) {
        /* External SRAM (0xA000 - 0xBFFF) */
        if (!mmu->ram_enabled || !mmu->sram || mmu->sram_size == 0) return 0xFF;
        uint32_t offset = (mmu->ram_bank * 0x2000) + (addr - 0xA000);
        return offset < mmu->sram_size ? mmu->sram[offset] : 0xFF;
    }
    if (addr < 0xD000) {
        /* WRAM Bank 0 (0xC000 - 0xCFFF) */
        return mmu->wram[addr - 0xC000];
    }
    if (addr < 0xE000) {
        /* WRAM Bank 1-7 (0xD000 - 0xDFFF) */
        uint8_t wram_bank = mmu->io[0x70 & 0x7F] & 0x07; /* SVBK register at 0xFF70 */
        if (wram_bank == 0) wram_bank = 1;
        uint32_t offset = (wram_bank * 0x1000) + (addr - 0xD000);
        return mmu->wram[offset];
    }
    if (addr < 0xFE00) {
        /* Echo RAM (0xE000 - 0xFDFF) -> maps to WRAM */
        return gb_mmu_read(mmu, addr - 0x2000);
    }
    if (addr < 0xFEA0) {
        /* OAM (0xFE00 - 0xFE9F) */
        return mmu->oam[addr - 0xFE00];
    }
    if (addr < 0xFF00) {
        /* Unusable memory */
        return 0xFF;
    }
    if (addr < 0xFF80) {
        /* I/O Registers (0xFF00 - 0xFF7F) */
        uint8_t reg = addr & 0x7F;
        if (reg == 0x00) {
            /* P1 / JOYPAD */
            uint8_t p1 = mmu->io[0x00];
            uint8_t res = 0xCF;
            if (!(p1 & 0x20)) res |= (mmu->joypad_state >> 4); /* Action buttons */
            if (!(p1 & 0x10)) res |= (mmu->joypad_state & 0x0F); /* Direction buttons */
            return res | p1;
        }
        return mmu->io[reg];
    }
    if (addr < 0xFFFF) {
        /* High RAM / HRAM (0xFF80 - 0xFFFE) */
        return mmu->hram[addr - 0xFF80];
    }
    if (addr == 0xFFFF) {
        /* Interrupt Enable register */
        return mmu->ie;
    }
    return 0xFF;
}

void gb_mmu_write(void *user, uint16_t addr, uint8_t val) {
    gb_mmu_t *mmu = (gb_mmu_t *)user;

    if (addr < 0x2000) {
        /* MBC RAM Enable */
        if (mmu->mbc_type > 0) {
            mmu->ram_enabled = ((val & 0x0F) == 0x0A);
        }
    } else if (addr < 0x4000) {
        /* MBC ROM Bank Number (lower 5 bits) */
        if (mmu->mbc_type == 1 || mmu->mbc_type == 3 || mmu->mbc_type == 5) {
            uint8_t bank = val & (mmu->mbc_type == 5 ? 0xFF : 0x1F);
            if (bank == 0) bank = 1;
            mmu->rom_bank = (mmu->rom_bank & 0xE0) | bank;
        }
    } else if (addr < 0x6000) {
        /* MBC RAM Bank or Upper ROM Bank bits */
        if (mmu->mbc_type == 1) {
            if (mmu->adv_banking) {
                mmu->ram_bank = val & 0x03;
            } else {
                mmu->rom_bank = (mmu->rom_bank & 0x1F) | ((val & 0x03) << 5);
            }
        } else if (mmu->mbc_type == 3) {
            mmu->ram_bank = val & 0x03;
        } else if (mmu->mbc_type == 5) {
            if (addr < 0x5000) {
                mmu->rom_bank = (mmu->rom_bank & 0x100) | val;
            } else {
                mmu->ram_bank = val & 0x0F;
            }
        }
    } else if (addr < 0x8000) {
        /* MBC Banking Mode Select */
        if (mmu->mbc_type == 1) {
            mmu->adv_banking = (val & 0x01) != 0;
        }
    } else if (addr < 0xA000) {
        /* VRAM (0x8000 - 0x9FFF) */
        uint8_t vram_bank = (mmu->io[0x4F & 0x7F] & 0x01);
        uint32_t offset = (vram_bank * 0x2000) + (addr - 0x8000);
        mmu->vram[offset] = val;
    } else if (addr < 0xC000) {
        /* External SRAM (0xA000 - 0xBFFF) */
        if (mmu->ram_enabled && mmu->sram && mmu->sram_size > 0) {
            uint32_t offset = (mmu->ram_bank * 0x2000) + (addr - 0xA000);
            if (offset < mmu->sram_size) mmu->sram[offset] = val;
        }
    } else if (addr < 0xD000) {
        /* WRAM Bank 0 (0xC000 - 0xCFFF) */
        mmu->wram[addr - 0xC000] = val;
    } else if (addr < 0xE000) {
        /* WRAM Bank 1-7 (0xD000 - 0xDFFF) */
        uint8_t wram_bank = mmu->io[0x70 & 0x7F] & 0x07;
        if (wram_bank == 0) wram_bank = 1;
        uint32_t offset = (wram_bank * 0x1000) + (addr - 0xD000);
        mmu->wram[offset] = val;
    } else if (addr < 0xFE00) {
        /* Echo RAM -> WRAM */
        gb_mmu_write(mmu, addr - 0x2000, val);
    } else if (addr < 0xFEA0) {
        /* OAM */
        mmu->oam[addr - 0xFE00] = val;
    } else if (addr < 0xFF00) {
        /* Unusable */
    } else if (addr < 0xFF80) {
        /* I/O Registers */
        uint8_t reg = addr & 0x7F;
        mmu->io[reg] = val;
    } else if (addr < 0xFFFF) {
        /* HRAM */
        mmu->hram[addr - 0xFF80] = val;
    } else if (addr == 0xFFFF) {
        /* Interrupt Enable */
        mmu->ie = val;
    }
}
