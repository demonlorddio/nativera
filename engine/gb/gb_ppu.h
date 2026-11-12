#ifndef NATIVERA_GB_PPU_H
#define NATIVERA_GB_PPU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t ly;       /* Current scanline (0-153) */
    uint32_t dot;     /* Cycles elapsed in current scanline (0-455) */
    uint8_t mode;     /* PPU mode: 0=HBlank, 1=VBlank, 2=OAM, 3=Pixel Transfer */
    bool frame_ready; /* True when a complete frame has been rendered */

    /* Framebuffer: 160x144 RGB888 pixels */
    uint32_t framebuffer[160 * 144];
} gb_ppu_t;

void gb_ppu_init(gb_ppu_t *ppu);
/* Advance PPU by cpu_cycles and return interrupt flags triggered (e.g. STAT / VBlank interrupts) */
uint8_t gb_ppu_step(gb_ppu_t *ppu, uint32_t cpu_cycles, uint8_t *io_regs);

#endif /* NATIVERA_GB_PPU_H */
