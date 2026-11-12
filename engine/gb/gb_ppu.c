#include "gb_ppu.h"
#include <string.h>

void gb_ppu_init(gb_ppu_t *ppu) {
    memset(ppu, 0, sizeof(gb_ppu_t));
    ppu->mode = 2; /* Start in OAM scan */
}

uint8_t gb_ppu_step(gb_ppu_t *ppu, uint32_t cpu_cycles, uint8_t *io_regs) {
    uint8_t triggered_ints = 0;

    /* Check if LCD is enabled (LCDC bit 7) */
    uint8_t lcdc = io_regs[0x40 & 0x7F];
    if (!(lcdc & 0x80)) {
        ppu->ly = 0;
        ppu->dot = 0;
        ppu->mode = 0;
        ppu->frame_ready = false;
        io_regs[0x44 & 0x7F] = 0; /* LY = 0 */
        return 0;
    }

    ppu->dot += cpu_cycles;
    ppu->frame_ready = false;

    if (ppu->ly < 144) {
        /* Visible scanlines */
        if (ppu->dot < 80) {
            ppu->mode = 2; /* OAM Scan */
        } else if (ppu->dot < 80 + 172) {
            ppu->mode = 3; /* Pixel Transfer */
        } else {
            if (ppu->mode != 0) {
                ppu->mode = 0; /* H-Blank */
                /* Check H-Blank STAT interrupt (bit 3) */
                if (io_regs[0x41 & 0x7F] & 0x08) {
                    triggered_ints |= 0x02; /* STAT interrupt */
                }
            }
        }

        if (ppu->dot >= 456) {
            ppu->dot -= 456;
            ppu->ly++;
            io_regs[0x44 & 0x7F] = ppu->ly;

            /* Check LYC == LY interrupt */
            uint8_t lyc = io_regs[0x45 & 0x7F];
            if (ppu->ly == lyc) {
                io_regs[0x41 & 0x7F] |= 0x04; /* Coincidence flag */
                if (io_regs[0x41 & 0x7F] & 0x40) {
                    triggered_ints |= 0x02; /* STAT interrupt */
                }
            } else {
                io_regs[0x41 & 0x7F] &= ~0x04;
            }

            if (ppu->ly == 144) {
                ppu->mode = 1; /* V-Blank */
                triggered_ints |= 0x01; /* VBlank interrupt request */
                ppu->frame_ready = true;
            }
        }
    } else {
        /* V-Blank scanlines (144 - 153) */
        ppu->mode = 1;
        if (ppu->dot >= 456) {
            ppu->dot -= 456;
            ppu->ly++;
            io_regs[0x44 & 0x7F] = ppu->ly;

            uint8_t lyc = io_regs[0x45 & 0x7F];
            if (ppu->ly == lyc) {
                io_regs[0x41 & 0x7F] |= 0x04;
                if (io_regs[0x41 & 0x7F] & 0x40) {
                    triggered_ints |= 0x02;
                }
            } else {
                io_regs[0x41 & 0x7F] &= ~0x04;
            }

            if (ppu->ly > 153) {
                ppu->ly = 0;
                io_regs[0x44 & 0x7F] = 0;
                ppu->mode = 2; /* OAM Scan for next frame */
            }
        }
    }

    /* Update STAT mode bits (bits 0-1) */
    uint8_t stat = io_regs[0x41 & 0x7F] & 0xFC;
    stat |= (ppu->mode & 0x03);
    io_regs[0x41 & 0x7F] = stat;

    return triggered_ints;
}
