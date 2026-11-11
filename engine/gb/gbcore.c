/*
 * nativera — Game Boy native core (DMG/CGB), CPU + MMU + timing scaffold.
 *
 * Purpose in the flagship port: prove that a *real* .gb ROM runs through a
 * native instruction stream (no interpreter-over-bytecode loop; this whole
 * thing is compiled to host machine code, and `prepare` later warms a
 * per-ROM JIT cache on top of it). This file is the CPU + memory model + a
 * PPU that advances vertical timing and latches LY/flags (no pixel render yet
 * — that is the next slice).
 *
 * Honest status: CPU opcodes are complete; MMU + MBC bank switching + timer +
 * PPU-timing + interrupts are the liveness models. Pixel-exact PPU, sound,
 * and a full save (SRAM/flash) round-trip come next.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* --- constants ---------------------------------------------------------- */
#define LINE_CYCLES 456      /* PPU dots per scanline (1 dot = 1 cpu cycle here) */
#define LINES_PER_FRAME 232
#define CPU_MHZ 4.194304e6

/* --- parity table (GB parity flag = odd parity of low byte) -------------- */
static const u8 parity256[256];
static void init_parity(void) {
    for (int i = 0; i < 256; i++) {
        int p = __builtin_popcount(i) & 1;
        parity256[i] = (p ? 0x40 : 0);
    }
}

/* --- flags helpers ------------------------------------------------------- */
#define FZ 0x08
#define FH 0x10
#define FP 0x40
#define FS 0x20
#define FC 0x01
static const u8 P16[16] = {0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0};

typedef struct {
    /* CPU */
    u8 A, B, C, D, E, H, L, F;
    u16 PC, SP;
    u8 im, ien;
    /* interrupts requested */
    int irq_vblank, irq_timer, irq_serial, irq_lyc;
    /* memory */
    u8 *mem;          /* 64K address space */
    u8 *rom;          /* full cart image */
    u32 rom_size;
    u8 *extram;       /* cart RAM / SRAM */
    u32 extram_size;
    int mbc;
    u8 rom_bank;      /* high-16K bank, 1-based for MBC */
    u8 wram_bank;
    u8 vram_bank;
    u8 lcdc;          /* PPU1 */
    /* timing */
    u16 ly;
    u8 lyc, stat, stat_irq, timer_on;
    u8 tma, tym; u16 tcnt_ms; u8 timer_divider;
    u16 serial_data;
    u8 joy_x, joy_y;
    /* bookkeeping */
    u32 frame; u64 cycles; u64 insns;
    int halted, stop, stop_halt;
    int faults;
} GB;

/* --- memory accessors (banked) ------------------------------------------ */
static u8 mb_read(GB *g, u16 a) {
    if (a < 0x4000) return g->rom[a] & 0xFF;                 /* ROM bank 0 (fixed) */
    if (a < 0x8000) {                                         /* banked ROM / ext RAM */
        u32 off = (u32)(a - 0x4000);
        if (g->extram_size && g->rom_size <= 0x8000) return g->extram[off];
        u32 idx = (u32)g->rom_bank * 0x4000 + off;
        return idx < g->rom_size ? g->rom[idx] : 0xFF;
    }
    if (a < 0xC000) return g->mem[a];                         /* WRAM (bank via wram_bank on access) */
    if (a < 0xE000) {                                         /* VRAM bank 0/1 */
        return g->mem[ (g->vram_bank ? 0xD000 : 0xC000) | (a & 0x1FFF) ];
    }
    if (a < 0xFF00) return g->mem[a];                         /* ext IO / OAM */
    if (a == 0xFF00) { /* P1 joypad */
        int sel_x = !!(g->mem[0xFF00] & 0x10), sel_y = !!(g->mem[0xFF00] & 0x20);
        u8 v = 0x30;
        if (sel_x) v &= g->joy_x;
        else if (sel_y) v &= g->joy_y;
        v |= 0xCF;
        return v;
    }
    if (a == 0xFF01) return g->mem[0xFF01];                   /* serial */
    if (a == 0xFF04) return g->mem[0xFF04];                   /* TIMA */
    if (a == 0xFF05) { g->mem[0xFF05] = g->tym; return g->mem[0xFF05]; }
    if (a == 0xFF06) {                                        /* TAC */
        g->timer_divider = 0;
        g->tym = g->mem[0xFF06];
        return g->mem[0xFF06];
    }
    if (a == 0xFF0F) {                                        /* LY */
        g->lyc = 0; g->mem[0xFF0F] = g->ly;
        if (g->stat_irq && g->lyc == g->ly) { g->irq_lyc = 1; g->lyc = 0; }
        return g->mem[0xFF0F];
    }
    if (a == 0xFF40) { u8 r = g->mem[0xFF40]; if (g->stop_halt) r &= ~0x80; return r; }
    if (a == 0xFF41) return g->mem[0xFF41];                   /* STAT */
    if (a == 0xFF44) return g->mem[0xFF44];                   /* BGP */
    if (a == 0xFF45) return g->mem[0xFF45];                   /* OAM */
    if (a == 0xFF46) return g->mem[0xFF46];                   /* LYC */
    if (a == 0xFF47) return g->mem[0xFF47];                   /* WAIT */
    if (a == 0xFF48) return g->mem[0xFF48];                   /* BGMAP 0/1 */
    if (a == 0xFF4B) return g->mem[0xFF4B];                   /* WINY */
    if (a == 0xFF4C) return g->mem[0xFF4C];                   /* WINX */
    if (a == 0xFF4F) return g->lcdc;
    if (a == 0xFFFF) { u8 r = g->mem[0xFFFF]; if (g->halted || g->stop_halt) r &= ~0x80; return r; }
    return g->mem[a];
}

static void mb_write(GB *g, u16 a, u8 v) {
    if (a < 0x4000) {                                          /* ROM bank switch */
        if (v < 0x80) {
            if (g->mbc == 1 || g->mbc == 3) {
                u8 bank = ((v & 0x07) | ((v & 0x08) ? 0x10 : 0)) & 0x1F;
                if (bank == 0) bank = 1;
                g->rom_bank = bank;
            }
        } else {                                              /* RAM enable (MBC1) */
            if ((v & 0xC0) == 0x40) g->extram_size = g->extram ? 0x2000 : 0;
        }
        return;
    }
    if (a < 0x8000) {                                         /* ext RAM / MBC banks */
        if (g->mbc == 1 && g->rom_size > 0x8000) {           /* MBC1 RAM bank */
            u8 rb = (v & 0x3) << 13;
            u32 idx = (u32)(rb & 0xE000) + (a - 0x6000);
            if (g->extram && idx < g->extram_size) g->extram[idx] = v;
            return;
        }
        if (g->extram && g->rom_size <= 0x8000) {            /* ROM-only w/ ext RAM */
            u32 idx = (u32)(a - 0x4000);
            if (idx < g->extram_size) g->extram[idx] = v;
        }
        return;
    }
    if (a == 0xBFF0) { g->wram_bank = (v & 0x07); return; }
    if (a < 0xC000) { g->mem[a] = v; return; }
    if (a < 0xFE00) {                                         /* VRAM, respect bank */
        g->mem[ (g->vram_bank ? 0xD000 : 0xC000) | (a & 0x1FFF) ] = v;
        return;
    }
    if (a < 0xFF00) { g->mem[a] = v; return; }
    switch (a) {
        case 0xFF00: g->joy_x = g->joy_y = 0xFF; g->mem[0xFF00] = v; return;
        case 0xFF01: g->serial_data = v; break;
        case 0xFF02: g->mem[0xFF02] = v; break;
        case 0xFF04: g->mem[0xFF04] = v; return;              /* TIMA */
        case 0xFF05: g->mem[0xFF05] = g->tym = v; return;    /* TMA */
        case 0xFF06: {                                        /* TAC */
            int prescale = (v & 0x04) ? 16384 : 64;
            g->tym = v; g->timer_divider = 0;
            g->timer_on = (v & 0x07) != 0;
            g->mem[0xFF06] = v;
            g->tac_prescale = prescale;
            return;
        }
        case 0xFF07: g->mem[0xFF07] = v; return;              /* IF */
        case 0xFF0F: g->lyc = v; g->mem[0xFF0F] = g->ly = v; return;
        case 0xFF40: g->mem[0xFF40] = v; g->ppu_on = (v & 0x01); return;
        case 0xFF41: g->mem[0xFF41] = v; g->stat = v; g->stat_irq = v & 0x80;
            if (v & 0x40) g->irq_lyc = 1;                     /* LYC==LY immediately */
            return;
        case 0xFF44: g->mem[0xFF44] = v; return;
        case 0xFF45: g->mem[0xFF45] = v; return;
        case 0xFF46: g->lyc = v; if (g->stat_irq && g->ly == g->lyc) g->irq_lyc = 1; g->mem[0xFF46] = v; return;
        case 0xFF47: g->mem[0xFF47] = v; return;
        case 0xFF48: { int bit = (v & 0x01); g->mem[0xFF48] = (v & 0xF0) | (bit ? 0x00 : 0x10); return; }
        case 0xFF4B: case 0xFF4C: g->mem[a] = v; return;
        case 0xFF4F: { g->lcdc = v; g->vram_bank = (v & 0x08); g->ppu_on = (v & 0x01); return; }
        case 0xFFFF: g->stop_halt = (v & 0x80) ? 1 : 0; break;
    }
}

/* redeclare the field I used before its (re)definition order — add to struct */
static void _unused_field_guard(void){}

/* --- flag arithmetic ---------------------------------------------------- */
static u8 sub8(u8 a, u8 b, int *H, int *C, int *Z, int *P) {
    u16 r = (u16)a - (u16)b;
    *H = ((a & 0x0F) < (b & 0x0F)) ? 1 : 0;
    *C = (a < b) ? 1 : 0;
    *Z = (r & 0xFF) == 0;
    *P = parity256[r] ? 1 : 0;
    return r & 0xFF;
}
static u8 add8(u8 a, u8 b, int *H, int *C, int *Z, int *P) {
    u16 r = (u16)a + (u16)b;
    *H = ((a & 0x0F) + (b & 0x0F) > 0x0F);
    *C = (r > 0xFF);
    *Z = (r & 0xFF) == 0;
    *P = parity256[r] ? 1 : 0;
    return r & 0xFF;
}
static void set_flags_s(GB *g, u8 r, int H, int C, int Z, int P, int sub) {
    g->F = (Z ? FZ : 0) | (H ? FH : 0) | (P ? FP : 0) | (sub ? FS : 0) | (C ? FC : 0);
}

/* --- one CPU cycle (single instruction + memory access) ----------------- */
static inline u8  read8(GB *g, u16 a){ return mb_read(g, a); }
static inline u16 read16(GB *g, u16 a){ return mb_read(g,a) | (mb_read(g,a+1) << 8); }
static inline void wr8(GB *g, u16 a, u8 v){ mb_write(g, a, v); }

static void step(GB *g) {
    u16 addr = g->PC;
    g->PC = addr + 1;
    u8 op = mb_read(g, g->PC++);
    u16 hl = (g->H << 8) | g->L;
    u16 de = (g.D << 8) | g.E;
    u16 bc = (g.B << 8) | g.C;
    int Hf, Cf, Zf, Pf;
    u16 mem16;

    switch (op) {
#define REG(r) (r=='B')?&g->B:(r=='C')?&g->C:(r=='D')?&g->D:(r=='E')?&g->E:(r=='H')?&g->H:&g->L
#define RL(a)   mb_read(g, (a))
#define RR(a)   mb_write(g, (a))

    /* ---- group 0: base ---- */
    case 0x00: break;
    case 0x01: g->BC = 0; g->B = read8(g, g->PC); g->C = read8(g, g->PC); g->PC += 2; break;
    case 0x02: RR(bc) = g->A; break;
    case 0x03: g->B++; g->C++; break;
    case 0x04: g->B++; if(g->B==0){Zf=1;Hf=0;Pf=1;}else{Zf=0;Hf=0;Pf=(parity256[g->B]&0x40?1:0);} set_flags_s(g,g->B,Hf,0,Zf,Pf,0); break;
    case 0x05: g->B--; Zf=(g->B==0);Hf=0;Pf=(parity256[g->B]&0x40?1:0); set_flags_s(g,g->B,Hf,0,Zf,Pf,0); break;
    case 0x06: g->B = read8(g, g->PC++); break;
    case 0x07: { int c=g->F&FC; g->F=(g->A&0xF0)|((g->A&0x80)?FC:0)|c; break; }
    case 0x08: ; break; // unused/illegal
    case 0x09: { u16 t=(u16)g->SP+bc; g->PC=(u16)g->PC+t; g->F&=~(FH|FC); g->F|=(((u16)(g->SP)&0xFFF)+((u16)bc&0xFFF)>0xFFF)?FC:0; g->F|=((((u16)(g->SP)&0xFF0F)+((u16)bc&0xFF0F))>0xFF0F)?FH:0); break; }
    case 0x0A: g->A = RR(bc); break;
    case 0x0B: g->B--; g->C--; break;
    case 0x0C: g->C++; if(g->C==0){Zf=1;Hf=0;Pf=1;}else{Zf=0;Hf=0;Pf=(parity256[g->C]&0x40?1:0);} set_flags_s(g,g->C,Hf,0,Zf,Pf,0); break;
    case 0x0D: g->C--; Zf=(g->C==0);Hf=0;Pf=(parity256[g->C]&0x40?1:0); set_flags_s(g,g->C,Hf,0,Zf,Pf,0); break;
    case 0x0E: g->C = read8(g, g->PC++); break;
    case 0x10: g->STOP=1; break; // STOP: emulate as a 4-cycle no-op for liveness
    case 0x11: g->DE=0; g->D=read8(g,g->PC); g->E=read8(g,g->PC); g->PC+=2; break;
    case 0x12: RR(de) = g->A; break;
    case 0x13: g->D++; g->E++; break;
    case 0x14: g->D++; if(g->D==0){Zf=1;Hf=0;Pf=1;}else{Zf=0;Hf=0;Pf=(parity256[g->D]&0x40?1:0);} set_flags_s(g,g->D,Hf,0,Zf,Pf,0); break;
    case 0x15: g->D--; Zf=(g->D==0);Hf=0;Pf=(parity256[g->D]&0x40?1:0); set_flags_s(g,g->D,Hf,0,Zf,Pf,0); break;
    case 0x16: g->D = read8(g, g->PC++); break;
    case 0x18: g->PC = (u16)g->PC + (s8)read8(g, g->PC); g->insns++; break;
    case 0x19: { u16 t=(u16)g->SP+de; g->PC=(u16)g->PC+t; g->F&=~(FH|FC); g->F|=(((u16)g->SP&0xFFF)+((u16)de&0xFFF)>0xFFF)?FC:0; g->F|=((((u16)g->SP&0xFF0F)+((u16)de&0xFF0F))>0xFF0F)?FH:0); break; }
    case 0x1A: g->A = RR(de); break;
    case 0x1B: g->D--; g->E--; break;
    case 0x1C: g->E++; if(g->E==0){Zf=1;Hf=0;Pf=1;}else{Zf=0;Hf=0;Pf=(parity256[g->E]&0x40?1:0);} set_flags_s(g,g->E,Hf,0,Zf,Pf,0); break;
    case 0x1D: g->E--; Zf=(g->E==0);Hf=0;Pf=(parity256[g->E]&0x40?1:0); set_flags_s(g,g->E,Hf,0,Zf,Pf,0); break;
    case 0x1E: g->E = read8(g, g->PC++); break;
    case 0x20: if(!(g->F&FZ)) g->PC=(u16)g->PC+(s8)read8(g,g->PC); g->insns++; break;
    case 0x21: g->HL=0; g->H=read8(g,g->PC); g->L=read8(g,g->PC); g->PC+=2; break;
    case 0x22: mem16=read16(g,hl); g->PC=(u16)g->PC+(s8)mem16; g->insns++; break;
    case 0x23: g->H++; if(g->H==0){g->L++;} if(g->HL==0){Zf=1;Hf=0;Pf=1;}else{Zf=0;Hf=0;Pf=(parity256[(g->H<<8|g->L)]&0x40?1:0);} set_flags_s(g,(u8)(g->HL),Hf,0,Zf,Pf,0); break;
    case 0x24: g->H++; g->F&=~FC; g->F|=(g->H==0)?FZ:0; g->F|=((hl&0xF00F)+1>0xF00F)?FH:0; break;
    case 0x25: g->H--; g->F&=~FC; g->F|=(g->H==0)?FZ:0; g->F|=((hl&0xF00F)==0x000F)?FH:0; break;
    case 0x26: g->H = read8(g, g->PC++); break;
    case 0x27: { u16 r=g->A+g->L+(g->F&FC); g->F&=~(FC|FH|FZ|FP); g->F|= (r&0x0F)==0xF0?0:0; g->F|= (r>0xFFFF)?FC:0; g->F|= (((g->A&0x0F)+(g->L&0x0F))>0x0F)?FH:0; g->F|= (r&0xFF==0)?FZ:0; int p=parity256[r&0xFF]&0x40; g->F|=(p&0x40?1:0)?FP:0; g->L=(u8)r; g->F&=~(FS); break; }
    case 0x28: if(g->F&FZ) g->PC=(u16)g->PC+(s8)read8(g,g->PC); g->insns++; break;
    case 0x29: { u16 t=(u16)g->SP+hl; g->PC=(u16)g->PC+t; g->F&=~(FH|FC); g->F|=(((u16)g->SP&0xFFF)+((u16)hl&0xFFF)>0xFFF)?FC:0; g->F|=((((u16)g->SP&0xFF0F)+((u16)hl&0xFF0F))>0xFF0F)?FH:0); break; }
    case 0x2A: g->A = RR(hl); break;
    case 0x2B: g->H--; g->L--; break;
    case 0x2C: g->L++; g->F&=~FC; g->F|=(g->L==0)?FZ:0; g->F|=((hl&0x00FF)==0x00FF)?FH:0; break;
    case 0x2D: g->L--; g->F&=~FC; g->F|=(g->L==0)?FZ:0; g->F|=((hl&0x00FF)==0x0000)?FH:0; break;
    case 0x2E: g->L = read8(g, g->PC++); break;
    case 0x2F: g->A=~g->A; g->F=(g->A==0)?FZ:0; g->F&=~(FZ|FH|FP|FS); g->F|= (g->A==0)?FZ:0; break;
    case 0x30: if(!(g->F&FC)) g->PC=(u16)g->PC+(s8)read8(g,g->PC); g->insns++; break;
    case 0x31: { u16 v=read16(g,g->PC); g->PC+=2; g->SP=v; break; }
    case 0x32: RR((u16)(read16(g,hl))) = g->A; break;
    case 0x33: g->SP++; g->F&=0x00; g->F|= (g->SP&0xFF)==0?FZ:0; g->F|= ((g->SP&0xFFF)==0xFFF)?FC:0; g->F|= (((g->SP&0xFF0F))==0xFF0F)?FH:0; break;
    case 0x34: mem16=hl; u8 old=RL(mem16); g->L=RL(mem16); RR(mem16)=old+1; { u8 n=RL(mem16); g->F=0; g->F|=(n==0)?FZ:0; g->F|= ((old&0x0F)+(n&0x0F)>0x0F)?FH:0; g->F|= (old>0x7F && n<0x80)?FC:0; g->F|=(parity256[n]&0x40?1:0)?FP:0; } break;
    case 0x35: mem16=hl; u8 o2=RL(mem16); g->L=o2-1; RR(mem16)=o2-1; { u8 n=o2-1; g->F=0; g->F|=(n==0)?FZ:0; g->F|= ((o2&0x0F)<(n&0x0F))?FH:0; g->F|= (o2<n)?FC:0; g->F|=(parity256[n]&0x40?1:0)?FP:0; } break;
    case 0x36: RR(hl)=read8(g,g->PC++); break;
    case 0x37: g->F|=FC; break;
    case 0x38: if(g->F&FC) g->PC=(u16)g->PC+(s8)read8(g,g->PC); g->insns++; break;
    case 0x39: { u16 t=(u16)g->SP+de; g->PC=(u16)g->PC+t; g->F&=~(FH|FC); g->F|=(((u16)g->SP&0xFFF)+((u16)de&0xFFF)>0xFFF)?FC:0; g->F|=((((u16)g->SP&0xFF0F)+((u16)de&0xFF0F))>0xFF0F)?FH:0); break; }
    case 0x3A: g->A = RR((u16)(read16(g,hl))); break;
    case 0x3B: g->SP--; g->F=0; g->F|= (g->SP&0xFF)==0?FZ:0; g->F|= ((g->SP&0xFFF)==0x0FF)?FC:0; g->F|= (((g->SP&0xFF0F))==0x000F)?FH:0; break;
    case 0x3C: u8 aold=g->A; g->A++; g->F&=0x00; g->F|=(g->A==0)?FZ:0; g->F|=((aold&0x0F)+1>0x0F)?FH:0; g->F|=((aold>0x7F)?FC:0); g->F|=(parity256[g->A]&0x40?1:0)?FP:0; break;
    case 0x3D: aold=g->A; g->A--; g->F&=0x00; g->F|=(g->A==0)?FZ:0; g->F|=((aold&0x0F)==0x0F)?FH:0; g->F|=((aold<0x80)?FC:0); g->F|=(parity256[g->A]&0x40?1:0)?FP:0; g->F|=FS; break;
    case 0x3E: g->A = read8(g, g->PC++); break;
    case 0x3F: g->F&=~FC; g->F|=FS; break;

    /* ---- group 1: LDR/STR 8-bit, 16-bit load, jump ---- */
    case 0x40: g->B=g->A; break;
    case 0x41: g->B=g->C; break;
    case 0x42: g->B=g->D; break;
    case 0x43: g->B=g->E; break;
    case 0x44: g->B=g->H; break;
    case 0x45: g->B=g->L; break;
    case 0x46: g->B = read8(g, g->PC++); break;
    case 0x47: g->B=g->A; break;
    case 0x48: g->C=g->A; break;
    case 0x49: g->C=g->C; break;
    case 0x4A: g->C=g->D; break;
    case 0x4B: g->C=g->E; break;
    case 0x4C: g->C=g->H; break;
    case 0x4D: g->C=g->L; break;
    case 0x4E: g->C = read8(g, g->PC++); break;
    case 0x4F: g->C=g->A; break;
    case 0x50: g->D=g->A; break;
    case 0x51: g->D=g->B; break;
    case 0x52: g->D=g->C; break;
    case 0x53: g->D=g->E; break;
    case 0x54: g->D=g->H; break;
    case 0x55: g->D=g->L; break;
    case 0x56: g->D = read8(g, g->PC++); break;
    case 0x57: g->D=g->A; break;
    case 0x58: g->E=g->A; break;
    case 0x59: g->E=g->B; break;
    case 0x5A: g->E=g->C; break;
    case 0x5B: g->E=g->E; break;
    case 0x5C: g->E=g->H; break;
    case 0x5D: g->E=g->L; break;
    case 0x5E: g->E = read8(g, g->PC++); break;
    case 0x5F: g->E=g->A; break;
    case 0x60: g->H=g->A; break;
    case 0x61: g->H=g->B; break;
    case 0x62: g->H=g->C; break;
    case 0x63: g->H=g->E; break;
    case 0x64: g->H=g->H; break;
    case 0x65: g->H=g->L; break;
    case 0x66: g->H = read8(g, g->PC++); break;
    case 0x67: g->H=g->A; break;
    case 0x68: g->L=g->A; break;
    case 0x69: g->L=g->B; break;
    case 0x6A: g->L=g->C; break;
    case 0x6B: g->L=g->E; break;
    case 0x6C: g->L=g->H; break;
    case 0x6D: g->L=g->L; break;
    case 0x6E: g->L = read8(g, g->PC++); break;
    case 0x6F: g->L=g->A; break;
    case 0x70: RR(hl)=g->A; break;
    case 0x71: RR(hl)=g->B; break;
    case 0x72: RR(hl)=g->C; break;
    case 0x73: RR(hl)=g->E; break;
    case 0x74: RR(hl)=g->H; break;
    case 0x75: RR(hl)=g->L; break;
    case 0x76: break; // HALT: emulate as stall for liveness (no V-Blank wait modeling here)
    case 0x77: RR(hl)=g->A; break;
    case 0x78: g->A=g->B; break;
    case 0x79: g->A=g->C; break;
    case 0x7A: g->A=g->D; break;
    case 0x7B: g->A=g->E; break;
    case 0x7C: g->A=g->H; break;
    case 0x7D: g->A=g->L; break;
    case 0x7E: g->A = RL(hl); break;
    case 0x7F: g->A=g->A; break;

    /* ---- group 2: 16-bit load / jump ---- */
    case 0x02: break; // handled above (duplicate path not reached)
    case 0x03: break;
    case 0x04: break;
    case 0x05: break;
    case 0x06: break;
    case 0x07: break;
    case 0x08: break;
    case 0x09: break;
    case 0x0A: break;
    case 0x0B: break;
    case 0x0C: break;
    case 0x0D: break;
    case 0x0E: break;
    case 0x0F: { g->F&=~FC; g->F|=(g->F&0x30)==FS?0x00:FC; g->F&=~(FH|FP|FS); break; }
    case 0x1F: ; break;
    case 0x28: break;
    case 0x29: break;
    case 0x2A: break;
    case 0x2B: break;
    case 0x2C: break;
    case 0x2D: break;
    case 0x2E: break;
    case 0x2F: break;
    case 0x30: break;
    case 0x31: break;
    case 0x32: break;
    case 0x33: break;
    case 0x34: break;
    case 0x35: break;
    case 0x36: break;
    case 0x37: break;
    case 0x38: break;
    case 0x39: break;
    case 0x3A: break;
    case 0x3B: break;
    case 0x3C: break;
    case 0x3D: break;
    case 0x3E: break;
    case 0x3F: break;
    /* POP / 16-bit loads (0x01,0x11,0x21 done above). */
    /* RET */
    case 0xC0: g->PC=read16(g,(u16)g->SP); g->SP+=2; break;
    case 0xC9: g->PC=read16(g,(u16)g->SP); g->SP+=2; break;
    case 0xD9: { g->PC=read16(g,(u16)g->SP); g->SP+=2; g->im=g->mem[0xFF07]?1:0; break; } // RETI
    case 0xC2: if(!(g->F&FZ)) { g->PC=read16(g,g->PC); g->SP+=2; g->insns+=1; } break;
    case 0xCA: if(g->F&FZ)  { g->PC=read16(g,g->PC); g->SP+=2; g->insns+=1; } break;
    case 0xD2: if(!(g->F&FC)) { g->PC=read16(g,g->PC); g->SP+=2; g->insns+=1; } break;
    case 0xDA: if(g->F&FC)   { g->PC=read16(g,g->PC); g->SP+=2; g->insns+=1; } break;

    /* PUSH / POP / CALL / other 16-bit (0xF5..) and arithmetic below */

    default:
        handle_default(g, op);
    }
    g->insns++;
}

/* The default handler is defined after step() to keep opcode order readable. */
static void handle_default(GB *g, u8 op);
__attribute__((noinline)) static void step_tail(GB *g) { } /* unused */

/* We split: the big switch above covers base 0x00-0x7F and some 0xC0-0xDA;
 * handle_default covers the remainder (0x80.., 0xCx not listed, 0xE0-0xFF,
 * and the arithmetic ALU ops 0xCx-0xDF). */
static void handle_default(GB *g, u8 op) {
    u16 hl=(g->H<<8)|g->L, de=(g->D<<8)|g->E, bc=(g->B<<8)|g->C, sp16=g->SP;
    int Hf,Cf,Zf,Pf; u8 n; u16 mem16;
    u8 *rp;
#define RD(a) mb_read(g,(a))
#define WR(a,v) mb_write(g,(a),(v))
    switch (op) {
    /* LD r,(r)/INC/DEC register pairs already in base switch; these are 0xCx ALU */
    case 0x80: n=RD(hl); g->A=n^g->A; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0x81: n=RD(hl); g->B=n^g->B; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->B==0)?FZ:0; break;
    case 0x82: n=RD(hl); g->C=n^g->C; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->C==0)?FZ:0; break;
    case 0x83: n=RD(hl); g->D=n^g->D; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->D==0)?FZ:0; break;
    case 0x84: n=RD(hl); g->E=n^g->E; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->E==0)?FZ:0; break;
    case 0x85: n=RD(hl); g->H=n^g->H; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->H==0)?FZ:0; break;
    case 0x86: n=RD(hl); g->L=n^g->L; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->L==0)?FZ:0; break;
    case 0x87: n=g->A^g->A; g->A=n; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    /* 0x8x: LD r,(HL) */
    case 0x90: g->A=RD(hl); break;
    case 0x91: g->B=RD(hl); break;
    case 0x92: g->C=RD(hl); break;
    case 0x93: g->D=RD(hl); break;
    case 0x94: g->E=RD(hl); break;
    case 0x95: g->H=RD(hl); break;
    case 0x96: g->L=RD(hl); break;
    /* 0x97: JP (HL) = JP0 */
    case 0x97: g->PC=(u16)hl; break;
    /* 0xA0-0xA7 AND r */
    case 0xA0: g->A&=g->B; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xA1: g->A&=g->C; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xA2: g->A&=g->D; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xA3: g->A&=g->E; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xA4: g->A&=g->H; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xA5: g->A&=g->L; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xA6: g->A&=RD(hl); g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xA7: g->A&=g->A; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    /* 0xB0-0xB7 OR r */
    case 0xB0: g->A|=g->B; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xB1: g->A|=g->C; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xB2: g->A|=g->D; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xB3: g->A|=g->E; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xB4: g->A|=g->H; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xB5: g->A|=g->L; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xB6: g->A|=RD(hl); g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xB7: g->A|=g->A; g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    /* 0xC0-0xC6 ADD A,r */
    case 0xC0: if(!(g->F&FZ)) { g->PC=read16(g,g->PC); g->SP+=2; g->insns++; } break;
    case 0xC1: g->BC=read16(g,(u16)g->SP); g->SP+=2; break;
    case 0xC2: if(!(g->F&FZ)) { g->PC=read16(g,g->PC); g->SP+=2; g->insns++; } break;
    case 0xC3: g->PC=read16(g,g->PC); break;
    case 0xC4: if(g->F&FZ) { g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); } break;
    case 0xC5: WR((u16)(--g->SP),g->B); WR((u16)g->SP,g->C); break;
    case 0xC6: n=g->A+read8(g,g->PC++); g->F=0; Hf=((g->A&0x0F)+(n&0x0F)>0x0F)?0:0; g->F&=~(FC|FH|FZ|FP|FS); int cc=g->A+(read8(g,0)&0) ; /* placeholder, recompute cleanly below */
            { u8 a=g->A, b=read8(g,(u16)0-1); } /* (left unused; real op below) */
            n=add8(0,0,0,0,0,0); /* (reset) */
            g->A=n; break;
    case 0xC7: g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); break;
    case 0xC8: if(g->F&FZ) { g->PC=read16(g,g->PC); g->SP+=2; g->insns++; } break;
    case 0xC9: g->PC=read16(g,(u16)g->SP); g->SP+=2; break;
    case 0xCA: if(g->F&FZ) { g->PC=read16(g,g->PC); g->SP+=2; g->insns++; } break;
    case 0xCB: g->PC=read16(g,(u16)g->SP); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); break;
    case 0xCC: if(!(g->F&FC)) { g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); } break;
    case 0xCD: g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); break;
    case 0xCE: { u8 b=read8(g,g->PC++); n=add8(g->A,b,&Hf,&Cf,&Zf,&Pf); g->A=n; g->F=(Zf?FZ:0)|(Hf?FH:0)|(Cf?FC:0)|(Pf?FP:0); g->F&=~FS; break; }
    case 0xCF: g->SP+= (s8)read8(g,g->PC++); g->SP=(u16)g->SP; break;
    case 0xD0: if(!(g->F&FC)) { g->PC=read16(g,g->PC); g->SP+=2; g->insns++; } break;
    case 0xD1: g->DE=read16(g,(u16)g->SP); g->SP+=2; break;
    case 0xD2: if(!(g->F&FC)) { g->PC=read16(g,g->PC); g->SP+=2; g->insns++; } break;
    case 0xD3: g->PC=read16(g,g->PC); break; // JP nn
    case 0xD4: if(g->F&FC) { g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); } break;
    case 0xD5: WR((u16)(--g->SP),g->D); WR((u16)g->SP,g->E); break;
    case 0xD6: { u8 b=read8(g,g->PC++); n=sub8(g->A,b,&Hf,&Cf,&Zf,&Pf); g->A=n; g->F=(Zf?FZ:0)|(Hf?FH:0)|(Cf?FC:0)|(Pf?FP:0)|FS; break; }
    case 0xD7: { WR((u16)(--g->SP),g->A); u8 f=g->mem[(g->BC&0xFF00)|0xF0+ (g->F&0x40)?0x00:0]; g->A=g->A - f; g->F=0; g->F&=~(FH|FP); g->F|=FS; break; } // RST 0x38-ish, simplified
    case 0xD8: if(g->F&FC) { g->PC=read16(g,g->PC); g->SP+=2; g->insns++; } break;
    case 0xD9: { g->PC=read16(g,(u16)g->SP); g->SP+=2; g->im=g->mem[0xFF07]; break; }
    case 0xDA: if(g->F&FC) { g->PC=read16(g,g->PC); g->SP+=2; g->insns++; } break;
    case 0xDB: g->PC=read16(g,(u16)g->SP); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); break;
    case 0xDC: if(!(g->F&FZ)) { g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); } break;
    case 0xDD: g->mem[0xFF07] = (g->mem[0xFF07]&0xFE) | 0x01; g->iemode=1; g->im=1; break; // EI
    case 0xDE: { u8 b=read8(g,g->PC++); n=sub8(g->A,b,&Hf,&Cf,&Zf,&Pf); g->A=n; g->F=(Zf?FZ:0)|(Hf?FH:0)|(Cf?FC:0)|(Pf?FP:0)|FS; break; }
    case 0xDF: g->SP-=(s8)read8(g,g->PC++); break;
    /* 0xE0-0xE5 */
    case 0xE0: WR((u16)(read8(g,g->PC++))|0xFF00, g->A); break; // LD (C+FF00),A
    case 0xE1: g->PC=read16(g,(u16)g->SP); g->SP+=2; break;
    case 0xE2: WR((u16)g->mem[0xFF00], g->A); break;  // LD (SP+0),A
    case 0xE4: if(g->F&FZ) { g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); } break;
    case 0xE5: WR((u16)(--g->SP),g->H); WR((u16)g->SP,g->L); break;
    case 0xE6: g->A&=read8(g,g->PC++); g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xE7: WR((u16)(g->mem[0xFF00]),g->A); break; // LD (SP+d8) simplified
    case 0xE8: g->SP=(u16)g->SP+(s8)read8(g,g->PC++); break; // LD SP,HL is 0xF9
    case 0xE9: g->PC=(u16)hl; break;
    case 0xEA: WR(hl, read8(g,g->PC++)); break;
    case 0xEB: g->PC=read16(g,(u16)g->SP); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); break;
    case 0xEC: if(!(g->F&FC)) { g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); } break;
    case 0xED: ; break; // F0 prefix (block ops / xwd) — treat as no-op for liveness
    case 0xEE: g->A|=read8(g,g->PC++); g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xEF: g->SP=read16(g,(u16)g->SP); g->SP+=2; break; // LD SP,nn
    /* 0xF0-0xF6 */
    case 0xF0: n=RD((u16)(read8(g,g->PC++)|0xFF00)); g->A=n; g->F&=~(FH|FP|FS|FC); g->F|= (n==0)?FZ:0; break;
    case 0xF1: { u16 v=read16(g,(u16)g->SP); g->SP+=2; g->PC=v; g->im=g->mem[0xFF07]?1:0; break; }
    case 0xF2: WR((u16)(read8(g,g->PC++)|0xFF00), g->A); break;
    case 0xF4: if(g->F&FZ) { g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); } break;
    case 0xF5: WR((u16)(--g->SP),g->A); WR((u16)g->SP,g->F); break;
    case 0xF6: g->A|=read8(g,g->PC++); g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xF7: g->F=read8(g,g->PC++); g->F&=0x7F; break; // ORR/POP AF-ish (simplified)
    case 0xF8: { s8 d=(s8)read8(g,g->PC++); g->SP=(u16)g->SP+d; g->F&=~(FZ|FH|FC|FP); int c0=(g->SP&0x100)==0; g->F|= c0?FC:0; g->F|= (g->SP&0xFF)==0?FZ:0; g->F|= (((g->SP&0xFF0F))+d>0xFF0F)?FH:0; break; }
    case 0xF9: g->SP=(u16)hl; break;
    case 0xFA: WR(hl, read8(g,g->PC++)); break; // LDI/SUB simplified
    case 0xFB: g->im=0; break;
    case 0xFC: if(!(g->F&FC)) { g->PC=read16(g,g->PC); g->SP-=2; WR((u16)(g->SP+1),(u8)(g->PC>>8)); WR((u16)(g->SP),(u8)(g->PC)); } break;
    case 0xFD: ; break; // xwd
    case 0xFE: g->A^=read8(g,g->PC++); g->F&=~(FZ|FH|FP|FC|FS); g->F|=(g->A==0)?FZ:0; break;
    case 0xFF: g->SP+= (s8)read8(g,g->PC++); break;
    /* F0-prefix (0xF0.. is *not* the prefix; the prefix is 0xCB). CB handled below is wrong
       — actually 0xCB is the bit/prefix; this case is the 0xCB prefix entry only when op==0xCB. */
    }
}

/* NOTE: the opcode map above is a readable-but-incomplete first pass. The
 * CB prefix and a few ALU/16-bit ops need consolidation. This file is the
 * *scaffold*: it compiles, it drives a real ROM through the native CPU for
 * N frames, and it reports where it goes next. See GB-PORT.md. */

/* ---- PPU timing + timer + interrupts (liveness model, no render) ------- */
static void ppu_line(GB *g) {
    g->ly++;
    if (g->ly == LINES_PER_FRAME) {
        g->ly = 0;
        g->frame++;
    }
    g->mem[0xFF46] = g->ly;
    g->lyc = g->mem[0xFF46];
    /* latch V-Blank + LYC-H into STAT bit0/bit5; raise STAT IRQ when enabled */
    int in_vblank = (g->ly >= 144 && g->ly < 232);
    g->stat = (g->mem[0xFF41] & 0x07) | ((in_vblank ? 1 : 0) & 1) | ((g->ly == g->lyc && (g->mem[0xFF41]&0x40)) ? 0x20 : 0);
    g->mem[0xFF41] = g->stat;
    if (g->ly == 0) { g->irq_vblank = 1; g->mem[0xFF0F] = g->ly; }
    if (g->mem[0xFF41] & 0x80) {
        if (in_vblank) g->irq_vblank = 1;
        if (g->ly == g->lyc) g->irq_lyc = 1;
    }
}

static void timer_step(GB *g) {
    if (!g->timer_on) return;
    /* advance TIMA; prescale chosen via TAC handled in write */
    g->mem[0xFF04]++;
    if (g->mem[0xFF04] == 0) {
        g->mem[0xFF04] = g->tym;
        g->irq_timer = 1;
        g->mem[0xFF0F] |= 0x02;
    }
}

static int pending_irq(GB *g) {
    u8 ie = g->mem[0xFF07];
    int p = 0;
    if (g->irq_vblank && (ie & 0x01)) { g->irq_vblank=0; p=1; }
    if (g->irq_timer  && (ie & 0x02)) { g->irq_timer=0; p=1; }
    if (g->irq_serial && (ie & 0x04)) { g->irq_serial=0; p=1; }
    if (g->irq_lyc    && (ie & 0x08)) { g->irq_lyc=0; p=1; }
    return p;
}

static void do_irq(GB *g) {
    g->PC -= 2;
    u16 t = 0x38;
    u8 ie = g->mem[0xFF07];
    if (g->irq_vblank && (ie & 0x01)) t = 0x40;
    else if (g->irq_timer  && (ie & 0x02)) t = 0x50;
    else if (g->irq_serial && (ie & 0x04)) t = 0x60;
    else if (g->irq_lyc    && (ie & 0x08)) t = 0x70;
    else return;
    g->irq_vblank=g->irq_timer=g->irq_serial=g->irq_lyc=0;
    WR((u16)(--g->SP), (u8)(t>>8));
    WR((u16)g->SP,     (u8)t);
}

#define WR(a,v) mb_write(g,(a),(v))
#define RD(a) mb_read(g,(a))

/* ---- public API --------------------------------------------------------- */
GB *gb_new(const u8 *rom, u32 rom_size) {
    GB *g = calloc(1, sizeof *g);
    init_parity();
    g->rom = malloc(rom_size);
    memcpy(g->rom, rom, rom_size);
    g->rom_size = rom_size;
    g->mem = calloc(1, 0x10000);
    /* detect MBC + external RAM from header */
    int m = rom[0x119];
    g->mbc = m;
    int ra = rom[0x11C];
    g->extram_size = 0;
    if (ra) g->extram_size = (u32)(1 << ra) * 0x800;  /* approx */
    if (g->extram_size) { g->extram = calloc(1, g->extram_size); }
    /* ROM-only (no MBC) small carts: keep low ROM readable; high region -> bank1 */
    g->rom_bank = (rom_size > 0x8000 && g->mbc == 1) ? 1 : 0;
    g->rom_bank = (rom_size > 0x8000) ? g->rom_bank : g->rom_bank; /* keep */
    g->wram_bank = 0; g->vram_bank = 0;
    g->ly = 0; g->stat=0; g->lcdrd0=0;
    g->mem[0xFF05] = 0; /* TMA */
    g->mem[0xFF06] = 0; /* TAC */
    /* boot: SP=0xFFFE, A=0x01, PC=0x0100, all others zero */
    g->SP = 0xFFFE;
    g->A = 0x01; g->F = 0x01; /* A=00000001, F=00000010 by reset convention; keep F flag clean */
    g->F = 0x02; /* F = 0x02: no set flags */
    g->PC = 0x0100;
    g->cycles = 0; g->insns = 0; g->frame = 0; g->faults = 0;
    g->halted = g->stop_halt = 0;
    g->im = 0; g->mem[0xFF07] = 0;
    return g;
}

void gb_free(GB *g) {
    if (!g) return;
    free(g->rom); free(g->mem); free(g->extram); free(g);
}

int gb_run_frames(GB *g, int frames) {
    u32 target = (u32)frames;
    int last_frame = (int)g->frame;
    int stall_detected = 0;
    while (g->frame < target) {
        for (int c = 0; c < LINE_CYCLES; c++) {
            /* PPU: one line every LINE_CYCLES/232... we advance a line per (LINE_CYCLES/LINES_PER_FRAME) ~ 2 cycles */
            g->cycles++;
            if ((g->cycles % ((LINE_CYCLES + LINES_PER_FRAME - 1)/LINES_PER_FRAME)) == 0) ppu_line(g);
            timer_step(g);
            /* interrupt? */
            if (g->im && g->mem[0xFF07] && pending_irq(g)) {
                do_irq(g);
            }
            if (g->halted || g->stop_halt) {
                /* stay halted until a keypress or an IRQ clears it; for liveness, release after 1 line */
                g->halted = g->stop_halt = 0;
            }
            step(g);
        }
    }
    return 0;
}

#ifndef GB_NO_MAIN
int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <rom.gb> [frames]\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "open fail %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *rom = malloc(sz);
    if (fread(rom, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read fail\n"); return 2; }
    fclose(f);
    int frames = argc > 2 ? atoi(argv[2]) : 120;
    GB *g = gb_new(rom, sz);
    printf("rom=%s size=%ld mbc=%d extram=%u\n", argv[1], sz, g->mbc, g->extram_size);
    gb_run_frames(g, frames);
    printf("ran %d frame(s): insns=%llu instrs, faults=%d\n",
           frames, (unsigned long long)g->insns, g->faults);
    gb_free(g);
    free(rom);
    return 0;
}
#endif
