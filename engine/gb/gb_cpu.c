#include "gb_cpu.h"

/* Reading and writing macros */
#define READ(addr)  read_mem(user, addr)
#define WRITE(addr, val) write_mem(user, addr, val)

void gb_cpu_init(gb_cpu_t *cpu) {
    cpu->af = 0x01B0;
    cpu->bc = 0x0013;
    cpu->de = 0x00D8;
    cpu->hl = 0x014D;
    cpu->sp = 0xFFFE;
    cpu->pc = 0x0100;
    cpu->ime = false;
    cpu->halted = false;
    cpu->stopped = false;
    cpu->int_flags = 0;
    cpu->int_enable = 0;
    cpu->cycles = 0;
}

/* 8-bit Arithmetic/Logic Helper Functions */

static inline uint8_t op_inc(gb_cpu_t *cpu, uint8_t val) {
    uint8_t res = val + 1;
    cpu->f &= ~(GB_FLAG_Z | GB_FLAG_N | GB_FLAG_H);
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if ((val & 0x0F) == 0x0F) cpu->f |= GB_FLAG_H;
    return res;
}

static inline uint8_t op_dec(gb_cpu_t *cpu, uint8_t val) {
    uint8_t res = val - 1;
    cpu->f &= ~(GB_FLAG_Z | GB_FLAG_N | GB_FLAG_H);
    cpu->f |= GB_FLAG_N;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if ((val & 0x0F) == 0) cpu->f |= GB_FLAG_H;
    return res;
}

static inline void op_add(gb_cpu_t *cpu, uint8_t val) {
    uint32_t res = cpu->a + val;
    cpu->f = 0;
    if ((res & 0xFF) == 0) cpu->f |= GB_FLAG_Z;
    if (((cpu->a & 0x0F) + (val & 0x0F)) > 0x0F) cpu->f |= GB_FLAG_H;
    if (res > 0xFF) cpu->f |= GB_FLAG_C;
    cpu->a = res & 0xFF;
}

static inline void op_adc(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = (cpu->f & GB_FLAG_C) ? 1 : 0;
    uint32_t res = cpu->a + val + carry;
    cpu->f = 0;
    if ((res & 0xFF) == 0) cpu->f |= GB_FLAG_Z;
    if (((cpu->a & 0x0F) + (val & 0x0F) + carry) > 0x0F) cpu->f |= GB_FLAG_H;
    if (res > 0xFF) cpu->f |= GB_FLAG_C;
    cpu->a = res & 0xFF;
}

static inline void op_sub(gb_cpu_t *cpu, uint8_t val) {
    uint32_t res = cpu->a - val;
    cpu->f = GB_FLAG_N;
    if ((res & 0xFF) == 0) cpu->f |= GB_FLAG_Z;
    if ((cpu->a & 0x0F) < (val & 0x0F)) cpu->f |= GB_FLAG_H;
    if (cpu->a < val) cpu->f |= GB_FLAG_C;
    cpu->a = res & 0xFF;
}

static inline void op_sbc(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = (cpu->f & GB_FLAG_C) ? 1 : 0;
    int32_t res = (int32_t)cpu->a - val - carry;
    cpu->f = GB_FLAG_N;
    if ((res & 0xFF) == 0) cpu->f |= GB_FLAG_Z;
    if (((int32_t)(cpu->a & 0x0F) - (val & 0x0F) - carry) < 0) cpu->f |= GB_FLAG_H;
    if (res < 0) cpu->f |= GB_FLAG_C;
    cpu->a = res & 0xFF;
}

static inline void op_and(gb_cpu_t *cpu, uint8_t val) {
    cpu->a &= val;
    cpu->f = GB_FLAG_H;
    if (cpu->a == 0) cpu->f |= GB_FLAG_Z;
}

static inline void op_xor(gb_cpu_t *cpu, uint8_t val) {
    cpu->a ^= val;
    cpu->f = 0;
    if (cpu->a == 0) cpu->f |= GB_FLAG_Z;
}

static inline void op_or(gb_cpu_t *cpu, uint8_t val) {
    cpu->a |= val;
    cpu->f = 0;
    if (cpu->a == 0) cpu->f |= GB_FLAG_Z;
}

static inline void op_cp(gb_cpu_t *cpu, uint8_t val) {
    uint32_t res = cpu->a - val;
    cpu->f = GB_FLAG_N;
    if ((res & 0xFF) == 0) cpu->f |= GB_FLAG_Z;
    if ((cpu->a & 0x0F) < (val & 0x0F)) cpu->f |= GB_FLAG_H;
    if (cpu->a < val) cpu->f |= GB_FLAG_C;
}

static inline void op_add_hl(gb_cpu_t *cpu, uint16_t val) {
    uint32_t res = cpu->hl + val;
    cpu->f &= ~(GB_FLAG_N | GB_FLAG_H | GB_FLAG_C);
    if (((cpu->hl & 0x0FFF) + (val & 0x0FFF)) > 0x0FFF) cpu->f |= GB_FLAG_H;
    if (res > 0xFFFF) cpu->f |= GB_FLAG_C;
    cpu->hl = res & 0xFFFF;
}

/* CB-prefix Instructions helper functions */

static inline uint8_t op_rlc(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = (val & 0x80) >> 7;
    uint8_t res = (val << 1) | carry;
    cpu->f = 0;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if (carry) cpu->f |= GB_FLAG_C;
    return res;
}

static inline uint8_t op_rrc(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = val & 0x01;
    uint8_t res = (val >> 1) | (carry << 7);
    cpu->f = 0;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if (carry) cpu->f |= GB_FLAG_C;
    return res;
}

static inline uint8_t op_rl(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = (cpu->f & GB_FLAG_C) ? 1 : 0;
    uint8_t next_carry = (val & 0x80) >> 7;
    uint8_t res = (val << 1) | carry;
    cpu->f = 0;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if (next_carry) cpu->f |= GB_FLAG_C;
    return res;
}

static inline uint8_t op_rr(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = (cpu->f & GB_FLAG_C) ? 1 : 0;
    uint8_t next_carry = val & 0x01;
    uint8_t res = (val >> 1) | (carry << 7);
    cpu->f = 0;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if (next_carry) cpu->f |= GB_FLAG_C;
    return res;
}

static inline uint8_t op_sla(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = (val & 0x80) >> 7;
    uint8_t res = val << 1;
    cpu->f = 0;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if (carry) cpu->f |= GB_FLAG_C;
    return res;
}

static inline uint8_t op_sra(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = val & 0x01;
    uint8_t res = (val >> 1) | (val & 0x80);
    cpu->f = 0;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if (carry) cpu->f |= GB_FLAG_C;
    return res;
}

static inline uint8_t op_swap(gb_cpu_t *cpu, uint8_t val) {
    uint8_t res = ((val & 0x0F) << 4) | ((val & 0xF0) >> 4);
    cpu->f = 0;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    return res;
}

static inline uint8_t op_srl(gb_cpu_t *cpu, uint8_t val) {
    uint8_t carry = val & 0x01;
    uint8_t res = val >> 1;
    cpu->f = 0;
    if (res == 0) cpu->f |= GB_FLAG_Z;
    if (carry) cpu->f |= GB_FLAG_C;
    return res;
}

static inline void op_bit(gb_cpu_t *cpu, uint8_t bit, uint8_t val) {
    cpu->f &= ~(GB_FLAG_Z | GB_FLAG_N);
    cpu->f |= GB_FLAG_H;
    if (!(val & (1 << bit))) cpu->f |= GB_FLAG_Z;
}

int gb_cpu_step(gb_cpu_t *cpu, void *user, gb_read_cb_t read_mem, gb_write_cb_t write_mem) {
    /* Handle interrupts */
    uint8_t ints = cpu->int_enable & cpu->int_flags & 0x1F;
    if (ints != 0) {
        cpu->halted = false;
        if (cpu->ime) {
            cpu->ime = false;
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);

            if (ints & 0x01) { cpu->int_flags &= ~0x01; cpu->pc = 0x0040; }
            else if (ints & 0x02) { cpu->int_flags &= ~0x02; cpu->pc = 0x0048; }
            else if (ints & 0x04) { cpu->int_flags &= ~0x04; cpu->pc = 0x0050; }
            else if (ints & 0x08) { cpu->int_flags &= ~0x08; cpu->pc = 0x0058; }
            else if (ints & 0x10) { cpu->int_flags &= ~0x10; cpu->pc = 0x0060; }

            cpu->cycles += 20;
            return 20;
        }
    }

    if (cpu->halted) {
        cpu->cycles += 4;
        return 4;
    }

    uint8_t opcode = READ(cpu->pc++);
    int cycles = 4;

    switch (opcode) {
        /* NOP */
        case 0x00: cycles = 4; break;

        /* LD BC, d16 */
        case 0x01: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            cpu->bc = (high << 8) | low;
            cycles = 12;
            break;
        }

        /* LD (BC), A */
        case 0x02: WRITE(cpu->bc, cpu->a); cycles = 8; break;

        /* INC BC */
        case 0x03: cpu->bc++; cycles = 8; break;

        /* INC B */
        case 0x04: cpu->b = op_inc(cpu, cpu->b); cycles = 4; break;

        /* DEC B */
        case 0x05: cpu->b = op_dec(cpu, cpu->b); cycles = 4; break;

        /* LD B, d8 */
        case 0x06: cpu->b = READ(cpu->pc++); cycles = 8; break;

        /* RLCA */
        case 0x07: {
            uint8_t carry = (cpu->a & 0x80) >> 7;
            cpu->a = (cpu->a << 1) | carry;
            cpu->f = carry ? GB_FLAG_C : 0;
            cycles = 4;
            break;
        }

        /* LD (a16), SP */
        case 0x08: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            uint16_t addr = (high << 8) | low;
            WRITE(addr, cpu->sp & 0xFF);
            WRITE(addr + 1, (cpu->sp >> 8) & 0xFF);
            cycles = 20;
            break;
        }

        /* ADD HL, BC */
        case 0x09: op_add_hl(cpu, cpu->bc); cycles = 8; break;

        /* LD A, (BC) */
        case 0x0A: cpu->a = READ(cpu->bc); cycles = 8; break;

        /* DEC BC */
        case 0x0B: cpu->bc--; cycles = 8; break;

        /* INC C */
        case 0x0C: cpu->c = op_inc(cpu, cpu->c); cycles = 4; break;

        /* DEC C */
        case 0x0D: cpu->c = op_dec(cpu, cpu->c); cycles = 4; break;

        /* LD C, d8 */
        case 0x0E: cpu->c = READ(cpu->pc++); cycles = 8; break;

        /* RRCA */
        case 0x0F: {
            uint8_t carry = cpu->a & 0x01;
            cpu->a = (cpu->a >> 1) | (carry << 7);
            cpu->f = carry ? GB_FLAG_C : 0;
            cycles = 4;
            break;
        }

        /* STOP */
        case 0x10: {
            cpu->stopped = true;
            cpu->pc++; /* STOP is technically a 2-byte instruction (0x10 0x00) */
            cycles = 4;
            break;
        }

        /* LD DE, d16 */
        case 0x11: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            cpu->de = (high << 8) | low;
            cycles = 12;
            break;
        }

        /* LD (DE), A */
        case 0x12: WRITE(cpu->de, cpu->a); cycles = 8; break;

        /* INC DE */
        case 0x13: cpu->de++; cycles = 8; break;

        /* INC D */
        case 0x14: cpu->d = op_inc(cpu, cpu->d); cycles = 4; break;

        /* DEC D */
        case 0x15: cpu->d = op_dec(cpu, cpu->d); cycles = 4; break;

        /* LD D, d8 */
        case 0x16: cpu->d = READ(cpu->pc++); cycles = 8; break;

        /* RLA */
        case 0x17: {
            uint8_t carry = (cpu->f & GB_FLAG_C) ? 1 : 0;
            uint8_t next_carry = (cpu->a & 0x80) >> 7;
            cpu->a = (cpu->a << 1) | carry;
            cpu->f = next_carry ? GB_FLAG_C : 0;
            cycles = 4;
            break;
        }

        /* JR r8 */
        case 0x18: {
            int8_t offset = (int8_t)READ(cpu->pc++);
            cpu->pc += offset;
            cycles = 12;
            break;
        }

        /* ADD HL, DE */
        case 0x19: op_add_hl(cpu, cpu->de); cycles = 8; break;

        /* LD A, (DE) */
        case 0x1A: cpu->a = READ(cpu->de); cycles = 8; break;

        /* DEC DE */
        case 0x1B: cpu->de--; cycles = 8; break;

        /* INC E */
        case 0x1C: cpu->e = op_inc(cpu, cpu->e); cycles = 4; break;

        /* DEC E */
        case 0x1D: cpu->e = op_dec(cpu, cpu->e); cycles = 4; break;

        /* LD E, d8 */
        case 0x1E: cpu->e = READ(cpu->pc++); cycles = 8; break;

        /* RRA */
        case 0x1F: {
            uint8_t carry = (cpu->f & GB_FLAG_C) ? 1 : 0;
            uint8_t next_carry = cpu->a & 0x01;
            cpu->a = (cpu->a >> 1) | (carry << 7);
            cpu->f = next_carry ? GB_FLAG_C : 0;
            cycles = 4;
            break;
        }

        /* JR NZ, r8 */
        case 0x20: {
            int8_t offset = (int8_t)READ(cpu->pc++);
            if (!(cpu->f & GB_FLAG_Z)) {
                cpu->pc += offset;
                cycles = 12;
            } else {
                cycles = 8;
            }
            break;
        }

        /* LD HL, d16 */
        case 0x21: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            cpu->hl = (high << 8) | low;
            cycles = 12;
            break;
        }

        /* LD (HL+), A */
        case 0x22: WRITE(cpu->hl++, cpu->a); cycles = 8; break;

        /* INC HL */
        case 0x23: cpu->hl++; cycles = 8; break;

        /* INC H */
        case 0x24: cpu->h = op_inc(cpu, cpu->h); cycles = 4; break;

        /* DEC H */
        case 0x25: cpu->h = op_dec(cpu, cpu->h); cycles = 4; break;

        /* LD H, d8 */
        case 0x26: cpu->h = READ(cpu->pc++); cycles = 8; break;

        /* DAA */
        case 0x27: {
            uint16_t correction = 0;
            if (cpu->f & GB_FLAG_H) correction |= 0x06;
            if (cpu->f & GB_FLAG_C) correction |= 0x60;
            if (cpu->f & GB_FLAG_N) {
                cpu->a -= correction;
            } else {
                if ((cpu->a & 0x0F) > 9) correction |= 0x06;
                if (cpu->a > 0x99) correction |= 0x60;
                cpu->a += correction;
            }
            cpu->f &= ~(GB_FLAG_Z | GB_FLAG_H);
            if (correction & 0x60) cpu->f |= GB_FLAG_C;
            if (cpu->a == 0) cpu->f |= GB_FLAG_Z;
            cycles = 4;
            break;
        }

        /* JR Z, r8 */
        case 0x28: {
            int8_t offset = (int8_t)READ(cpu->pc++);
            if (cpu->f & GB_FLAG_Z) {
                cpu->pc += offset;
                cycles = 12;
            } else {
                cycles = 8;
            }
            break;
        }

        /* ADD HL, HL */
        case 0x29: op_add_hl(cpu, cpu->hl); cycles = 8; break;

        /* LD A, (HL+) */
        case 0x2A: cpu->a = READ(cpu->hl++); cycles = 8; break;

        /* DEC HL */
        case 0x2B: cpu->hl--; cycles = 8; break;

        /* INC L */
        case 0x2C: cpu->l = op_inc(cpu, cpu->l); cycles = 4; break;

        /* DEC L */
        case 0x2D: cpu->l = op_dec(cpu, cpu->l); cycles = 4; break;

        /* LD L, d8 */
        case 0x2E: cpu->l = READ(cpu->pc++); cycles = 8; break;

        /* CPL */
        case 0x2F: cpu->a = ~cpu->a; cpu->f |= (GB_FLAG_N | GB_FLAG_H); cycles = 4; break;

        /* JR NC, r8 */
        case 0x30: {
            int8_t offset = (int8_t)READ(cpu->pc++);
            if (!(cpu->f & GB_FLAG_C)) {
                cpu->pc += offset;
                cycles = 12;
            } else {
                cycles = 8;
            }
            break;
        }

        /* LD SP, d16 */
        case 0x31: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            cpu->sp = (high << 8) | low;
            cycles = 12;
            break;
        }

        /* LD (HL-), A */
        case 0x32: WRITE(cpu->hl--, cpu->a); cycles = 8; break;

        /* INC SP */
        case 0x33: cpu->sp++; cycles = 8; break;

        /* INC (HL) */
        case 0x34: {
            uint8_t val = READ(cpu->hl);
            WRITE(cpu->hl, op_inc(cpu, val));
            cycles = 12;
            break;
        }

        /* DEC (HL) */
        case 0x35: {
            uint8_t val = READ(cpu->hl);
            WRITE(cpu->hl, op_dec(cpu, val));
            cycles = 12;
            break;
        }

        /* LD (HL), d8 */
        case 0x36: WRITE(cpu->hl, READ(cpu->pc++)); cycles = 12; break;

        /* SCF */
        case 0x37: cpu->f &= ~(GB_FLAG_N | GB_FLAG_H); cpu->f |= GB_FLAG_C; cycles = 4; break;

        /* JR C, r8 */
        case 0x38: {
            int8_t offset = (int8_t)READ(cpu->pc++);
            if (cpu->f & GB_FLAG_C) {
                cpu->pc += offset;
                cycles = 12;
            } else {
                cycles = 8;
            }
            break;
        }

        /* ADD HL, SP */
        case 0x39: op_add_hl(cpu, cpu->sp); cycles = 8; break;

        /* LD A, (HL-) */
        case 0x3A: cpu->a = READ(cpu->hl--); cycles = 8; break;

        /* DEC SP */
        case 0x3B: cpu->sp--; cycles = 8; break;

        /* INC A */
        case 0x3C: cpu->a = op_inc(cpu, cpu->a); cycles = 4; break;

        /* DEC A */
        case 0x3D: cpu->a = op_dec(cpu, cpu->a); cycles = 4; break;

        /* LD A, d8 */
        case 0x3E: cpu->a = READ(cpu->pc++); cycles = 8; break;

        /* CCF */
        case 0x3F: {
            cpu->f &= ~(GB_FLAG_N | GB_FLAG_H);
            cpu->f ^= GB_FLAG_C;
            cycles = 4;
            break;
        }

        /* LD B, B / C / D / E / H / L / (HL) / A */
        case 0x40: cpu->b = cpu->b; cycles = 4; break;
        case 0x41: cpu->b = cpu->c; cycles = 4; break;
        case 0x42: cpu->b = cpu->d; cycles = 4; break;
        case 0x43: cpu->b = cpu->e; cycles = 4; break;
        case 0x44: cpu->b = cpu->h; cycles = 4; break;
        case 0x45: cpu->b = cpu->l; cycles = 4; break;
        case 0x46: cpu->b = READ(cpu->hl); cycles = 8; break;
        case 0x47: cpu->b = cpu->a; cycles = 4; break;

        /* LD C, B / C / D / E / H / L / (HL) / A */
        case 0x48: cpu->c = cpu->b; cycles = 4; break;
        case 0x49: cpu->c = cpu->c; cycles = 4; break;
        case 0x4A: cpu->c = cpu->d; cycles = 4; break;
        case 0x4B: cpu->c = cpu->e; cycles = 4; break;
        case 0x4C: cpu->c = cpu->h; cycles = 4; break;
        case 0x4D: cpu->c = cpu->l; cycles = 4; break;
        case 0x4E: cpu->c = READ(cpu->hl); cycles = 8; break;
        case 0x4F: cpu->c = cpu->a; cycles = 4; break;

        /* LD D, B / C / D / E / H / L / (HL) / A */
        case 0x50: cpu->d = cpu->b; cycles = 4; break;
        case 0x51: cpu->d = cpu->c; cycles = 4; break;
        case 0x52: cpu->d = cpu->d; cycles = 4; break;
        case 0x53: cpu->d = cpu->e; cycles = 4; break;
        case 0x54: cpu->d = cpu->h; cycles = 4; break;
        case 0x55: cpu->d = cpu->l; cycles = 4; break;
        case 0x56: cpu->d = READ(cpu->hl); cycles = 8; break;
        case 0x57: cpu->d = cpu->a; cycles = 4; break;

        /* LD E, B / C / D / E / H / L / (HL) / A */
        case 0x58: cpu->e = cpu->b; cycles = 4; break;
        case 0x59: cpu->e = cpu->c; cycles = 4; break;
        case 0x5A: cpu->e = cpu->d; cycles = 4; break;
        case 0x5B: cpu->e = cpu->e; cycles = 4; break;
        case 0x5C: cpu->e = cpu->h; cycles = 4; break;
        case 0x5D: cpu->e = cpu->l; cycles = 4; break;
        case 0x5E: cpu->e = READ(cpu->hl); cycles = 8; break;
        case 0x5F: cpu->e = cpu->a; cycles = 4; break;

        /* LD H, B / C / D / E / H / L / (HL) / A */
        case 0x60: cpu->h = cpu->b; cycles = 4; break;
        case 0x61: cpu->h = cpu->c; cycles = 4; break;
        case 0x62: cpu->h = cpu->d; cycles = 4; break;
        case 0x63: cpu->h = cpu->e; cycles = 4; break;
        case 0x64: cpu->h = cpu->h; cycles = 4; break;
        case 0x65: cpu->h = cpu->l; cycles = 4; break;
        case 0x66: cpu->h = READ(cpu->hl); cycles = 8; break;
        case 0x67: cpu->h = cpu->a; cycles = 4; break;

        /* LD L, B / C / D / E / H / L / (HL) / A */
        case 0x68: cpu->l = cpu->b; cycles = 4; break;
        case 0x69: cpu->l = cpu->c; cycles = 4; break;
        case 0x6A: cpu->l = cpu->d; cycles = 4; break;
        case 0x6B: cpu->l = cpu->e; cycles = 4; break;
        case 0x6C: cpu->l = cpu->h; cycles = 4; break;
        case 0x6D: cpu->l = cpu->l; cycles = 4; break;
        case 0x6E: cpu->l = READ(cpu->hl); cycles = 8; break;
        case 0x6F: cpu->l = cpu->a; cycles = 4; break;

        /* LD (HL), B / C / D / E / H / L / HALT / A */
        case 0x70: WRITE(cpu->hl, cpu->b); cycles = 8; break;
        case 0x71: WRITE(cpu->hl, cpu->c); cycles = 8; break;
        case 0x72: WRITE(cpu->hl, cpu->d); cycles = 8; break;
        case 0x73: WRITE(cpu->hl, cpu->e); cycles = 8; break;
        case 0x74: WRITE(cpu->hl, cpu->h); cycles = 8; break;
        case 0x75: WRITE(cpu->hl, cpu->l); cycles = 8; break;
        case 0x76: cpu->halted = true; cycles = 4; break;
        case 0x77: WRITE(cpu->hl, cpu->a); cycles = 8; break;

        /* LD A, B / C / D / E / H / L / (HL) / A */
        case 0x78: cpu->a = cpu->b; cycles = 4; break;
        case 0x79: cpu->a = cpu->c; cycles = 4; break;
        case 0x7A: cpu->a = cpu->d; cycles = 4; break;
        case 0x7B: cpu->a = cpu->e; cycles = 4; break;
        case 0x7C: cpu->a = cpu->h; cycles = 4; break;
        case 0x7D: cpu->a = cpu->l; cycles = 4; break;
        case 0x7E: cpu->a = READ(cpu->hl); cycles = 8; break;
        case 0x7F: cpu->a = cpu->a; cycles = 4; break;

        /* ADD A, B / C / D / E / H / L / (HL) / A */
        case 0x80: op_add(cpu, cpu->b); cycles = 4; break;
        case 0x81: op_add(cpu, cpu->c); cycles = 4; break;
        case 0x82: op_add(cpu, cpu->d); cycles = 4; break;
        case 0x83: op_add(cpu, cpu->e); cycles = 4; break;
        case 0x84: op_add(cpu, cpu->h); cycles = 4; break;
        case 0x85: op_add(cpu, cpu->l); cycles = 4; break;
        case 0x86: op_add(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0x87: op_add(cpu, cpu->a); cycles = 4; break;

        /* ADC A, B / C / D / E / H / L / (HL) / A */
        case 0x88: op_adc(cpu, cpu->b); cycles = 4; break;
        case 0x89: op_adc(cpu, cpu->c); cycles = 4; break;
        case 0x8A: op_adc(cpu, cpu->d); cycles = 4; break;
        case 0x8B: op_adc(cpu, cpu->e); cycles = 4; break;
        case 0x8C: op_adc(cpu, cpu->h); cycles = 4; break;
        case 0x8D: op_adc(cpu, cpu->l); cycles = 4; break;
        case 0x8E: op_adc(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0x8F: op_adc(cpu, cpu->a); cycles = 4; break;

        /* SUB B / C / D / E / H / L / (HL) / A */
        case 0x90: op_sub(cpu, cpu->b); cycles = 4; break;
        case 0x91: op_sub(cpu, cpu->c); cycles = 4; break;
        case 0x92: op_sub(cpu, cpu->d); cycles = 4; break;
        case 0x93: op_sub(cpu, cpu->e); cycles = 4; break;
        case 0x94: op_sub(cpu, cpu->h); cycles = 4; break;
        case 0x95: op_sub(cpu, cpu->l); cycles = 4; break;
        case 0x96: op_sub(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0x97: op_sub(cpu, cpu->a); cycles = 4; break;

        /* SBC A, B / C / D / E / H / L / (HL) / A */
        case 0x98: op_sbc(cpu, cpu->b); cycles = 4; break;
        case 0x99: op_sbc(cpu, cpu->c); cycles = 4; break;
        case 0x9A: op_sbc(cpu, cpu->d); cycles = 4; break;
        case 0x9B: op_sbc(cpu, cpu->e); cycles = 4; break;
        case 0x9C: op_sbc(cpu, cpu->h); cycles = 4; break;
        case 0x9D: op_sbc(cpu, cpu->l); cycles = 4; break;
        case 0x9E: op_sbc(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0x9F: op_sbc(cpu, cpu->a); cycles = 4; break;

        /* AND B / C / D / E / H / L / (HL) / A */
        case 0xA0: op_and(cpu, cpu->b); cycles = 4; break;
        case 0xA1: op_and(cpu, cpu->c); cycles = 4; break;
        case 0xA2: op_and(cpu, cpu->d); cycles = 4; break;
        case 0xA3: op_and(cpu, cpu->e); cycles = 4; break;
        case 0xA4: op_and(cpu, cpu->h); cycles = 4; break;
        case 0xA5: op_and(cpu, cpu->l); cycles = 4; break;
        case 0xA6: op_and(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0xA7: op_and(cpu, cpu->a); cycles = 4; break;

        /* XOR B / C / D / E / H / L / (HL) / A */
        case 0xA8: op_xor(cpu, cpu->b); cycles = 4; break;
        case 0xA9: op_xor(cpu, cpu->c); cycles = 4; break;
        case 0xAA: op_xor(cpu, cpu->d); cycles = 4; break;
        case 0xAB: op_xor(cpu, cpu->e); cycles = 4; break;
        case 0xAC: op_xor(cpu, cpu->h); cycles = 4; break;
        case 0xAD: op_xor(cpu, cpu->l); cycles = 4; break;
        case 0xAE: op_xor(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0xAF: op_xor(cpu, cpu->a); cycles = 4; break;

        /* OR B / C / D / E / H / L / (HL) / A */
        case 0xB0: op_or(cpu, cpu->b); cycles = 4; break;
        case 0xB1: op_or(cpu, cpu->c); cycles = 4; break;
        case 0xB2: op_or(cpu, cpu->d); cycles = 4; break;
        case 0xB3: op_or(cpu, cpu->e); cycles = 4; break;
        case 0xB4: op_or(cpu, cpu->h); cycles = 4; break;
        case 0xB5: op_or(cpu, cpu->l); cycles = 4; break;
        case 0xB6: op_or(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0xB7: op_or(cpu, cpu->a); cycles = 4; break;

        /* CP B / C / D / E / H / L / (HL) / A */
        case 0xB8: op_cp(cpu, cpu->b); cycles = 4; break;
        case 0xB9: op_cp(cpu, cpu->c); cycles = 4; break;
        case 0xBA: op_cp(cpu, cpu->d); cycles = 4; break;
        case 0xBB: op_cp(cpu, cpu->e); cycles = 4; break;
        case 0xBC: op_cp(cpu, cpu->h); cycles = 4; break;
        case 0xBD: op_cp(cpu, cpu->l); cycles = 4; break;
        case 0xBE: op_cp(cpu, READ(cpu->hl)); cycles = 8; break;
        case 0xBF: op_cp(cpu, cpu->a); cycles = 4; break;

        /* RET NZ */
        case 0xC0: {
            if (!(cpu->f & GB_FLAG_Z)) {
                uint8_t low = READ(cpu->sp++);
                uint8_t high = READ(cpu->sp++);
                cpu->pc = (high << 8) | low;
                cycles = 20;
            } else {
                cycles = 8;
            }
            break;
        }

        /* POP BC */
        case 0xC1: {
            uint8_t low = READ(cpu->sp++);
            uint8_t high = READ(cpu->sp++);
            cpu->bc = (high << 8) | low;
            cycles = 12;
            break;
        }

        /* JP NZ, a16 */
        case 0xC2: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            if (!(cpu->f & GB_FLAG_Z)) {
                cpu->pc = (high << 8) | low;
                cycles = 16;
            } else {
                cycles = 12;
            }
            break;
        }

        /* JP a16 */
        case 0xC3: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            cpu->pc = (high << 8) | low;
            cycles = 16;
            break;
        }

        /* CALL NZ, a16 */
        case 0xC4: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            if (!(cpu->f & GB_FLAG_Z)) {
                cpu->sp -= 2;
                WRITE(cpu->sp, cpu->pc & 0xFF);
                WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
                cpu->pc = (high << 8) | low;
                cycles = 24;
            } else {
                cycles = 12;
            }
            break;
        }

        /* PUSH BC */
        case 0xC5: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->bc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->bc >> 8) & 0xFF);
            cycles = 16;
            break;
        }

        /* ADD A, d8 */
        case 0xC6: op_add(cpu, READ(cpu->pc++)); cycles = 8; break;

        /* RST 00H */
        case 0xC7: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = 0x0000;
            cycles = 16;
            break;
        }

        /* RET Z */
        case 0xC8: {
            if (cpu->f & GB_FLAG_Z) {
                uint8_t low = READ(cpu->sp++);
                uint8_t high = READ(cpu->sp++);
                cpu->pc = (high << 8) | low;
                cycles = 20;
            } else {
                cycles = 8;
            }
            break;
        }

        /* RET */
        case 0xC9: {
            uint8_t low = READ(cpu->sp++);
            uint8_t high = READ(cpu->sp++);
            cpu->pc = (high << 8) | low;
            cycles = 16;
            break;
        }

        /* JP Z, a16 */
        case 0xCA: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            if (cpu->f & GB_FLAG_Z) {
                cpu->pc = (high << 8) | low;
                cycles = 16;
            } else {
                cycles = 12;
            }
            break;
        }

        /* CB Prefix Opcodes */
        case 0xCB: {
            uint8_t cb_opcode = READ(cpu->pc++);
            cycles = 8; /* Default CB register instruction is 8 cycles, or 16 for (HL) */
            uint8_t bit = (cb_opcode >> 3) & 0x07;
            uint8_t reg = cb_opcode & 0x07;

            /* Helper function to get the value of register selected bycb_opcode lower 3 bits */
            uint8_t val = 0;
            if (reg == 0) val = cpu->b;
            else if (reg == 1) val = cpu->c;
            else if (reg == 2) val = cpu->d;
            else if (reg == 3) val = cpu->e;
            else if (reg == 4) val = cpu->h;
            else if (reg == 5) val = cpu->l;
            else if (reg == 6) { val = READ(cpu->hl); cycles = 16; }
            else if (reg == 7) val = cpu->a;

            /* Operation selection based on upper bits of cb_opcode */
            uint8_t op = cb_opcode >> 6;
            if (op == 0) {
                /* Shifts / Rotates */
                uint8_t sub_op = (cb_opcode >> 3) & 0x07;
                if (sub_op == 0) val = op_rlc(cpu, val);
                else if (sub_op == 1) val = op_rrc(cpu, val);
                else if (sub_op == 2) val = op_rl(cpu, val);
                else if (sub_op == 3) val = op_rr(cpu, val);
                else if (sub_op == 4) val = op_sla(cpu, val);
                else if (sub_op == 5) val = op_sra(cpu, val);
                else if (sub_op == 6) val = op_swap(cpu, val);
                else if (sub_op == 7) val = op_srl(cpu, val);
            } else if (op == 1) {
                /* BIT b, r */
                op_bit(cpu, bit, val);
                if (reg == 6) cycles = 12;
            } else if (op == 2) {
                /* RES b, r */
                val &= ~(1 << bit);
            } else if (op == 3) {
                /* SET b, r */
                val |= (1 << bit);
            }

            /* Store result back if it was modify operation */
            if (op != 1) {
                if (reg == 0) cpu->b = val;
                else if (reg == 1) cpu->c = val;
                else if (reg == 2) cpu->d = val;
                else if (reg == 3) cpu->e = val;
                else if (reg == 4) cpu->h = val;
                else if (reg == 5) cpu->l = val;
                else if (reg == 6) { WRITE(cpu->hl, val); }
                else if (reg == 7) cpu->a = val;
            }
            break;
        }

        /* CALL Z, a16 */
        case 0xCC: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            if (cpu->f & GB_FLAG_Z) {
                cpu->sp -= 2;
                WRITE(cpu->sp, cpu->pc & 0xFF);
                WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
                cpu->pc = (high << 8) | low;
                cycles = 24;
            } else {
                cycles = 12;
            }
            break;
        }

        /* CALL a16 */
        case 0xCD: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = (high << 8) | low;
            cycles = 24;
            break;
        }

        /* ADC A, d8 */
        case 0xCE: op_adc(cpu, READ(cpu->pc++)); cycles = 8; break;

        /* RST 08H */
        case 0xCF: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = 0x0008;
            cycles = 16;
            break;
        }

        /* RET NC */
        case 0xD0: {
            if (!(cpu->f & GB_FLAG_C)) {
                uint8_t low = READ(cpu->sp++);
                uint8_t high = READ(cpu->sp++);
                cpu->pc = (high << 8) | low;
                cycles = 20;
            } else {
                cycles = 8;
            }
            break;
        }

        /* POP DE */
        case 0xD1: {
            uint8_t low = READ(cpu->sp++);
            uint8_t high = READ(cpu->sp++);
            cpu->de = (high << 8) | low;
            cycles = 12;
            break;
        }

        /* JP NC, a16 */
        case 0xD2: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            if (!(cpu->f & GB_FLAG_C)) {
                cpu->pc = (high << 8) | low;
                cycles = 16;
            } else {
                cycles = 12;
            }
            break;
        }

        /* Invalid Opcode 0xD3 */
        case 0xD3: cycles = 4; break;

        /* CALL NC, a16 */
        case 0xD4: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            if (!(cpu->f & GB_FLAG_C)) {
                cpu->sp -= 2;
                WRITE(cpu->sp, cpu->pc & 0xFF);
                WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
                cpu->pc = (high << 8) | low;
                cycles = 24;
            } else {
                cycles = 12;
            }
            break;
        }

        /* PUSH DE */
        case 0xD5: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->de & 0xFF);
            WRITE(cpu->sp + 1, (cpu->de >> 8) & 0xFF);
            cycles = 16;
            break;
        }

        /* SUB d8 */
        case 0xD6: op_sub(cpu, READ(cpu->pc++)); cycles = 8; break;

        /* RST 10H */
        case 0xD7: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = 0x0010;
            cycles = 16;
            break;
        }

        /* RET C */
        case 0xD8: {
            if (cpu->f & GB_FLAG_C) {
                uint8_t low = READ(cpu->sp++);
                uint8_t high = READ(cpu->sp++);
                cpu->pc = (high << 8) | low;
                cycles = 20;
            } else {
                cycles = 8;
            }
            break;
        }

        /* RETI */
        case 0xD9: {
            uint8_t low = READ(cpu->sp++);
            uint8_t high = READ(cpu->sp++);
            cpu->pc = (high << 8) | low;
            cpu->ime = true;
            cycles = 16;
            break;
        }

        /* JP C, a16 */
        case 0xDA: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            if (cpu->f & GB_FLAG_C) {
                cpu->pc = (high << 8) | low;
                cycles = 16;
            } else {
                cycles = 12;
            }
            break;
        }

        /* Invalid Opcode 0xDB */
        case 0xDB: cycles = 4; break;

        /* CALL C, a16 */
        case 0xDC: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            if (cpu->f & GB_FLAG_C) {
                cpu->sp -= 2;
                WRITE(cpu->sp, cpu->pc & 0xFF);
                WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
                cpu->pc = (high << 8) | low;
                cycles = 24;
            } else {
                cycles = 12;
            }
            break;
        }

        /* Invalid Opcode 0xDD */
        case 0xDD: cycles = 4; break;

        /* SBC A, d8 */
        case 0xDE: op_sbc(cpu, READ(cpu->pc++)); cycles = 8; break;

        /* RST 18H */
        case 0xDF: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = 0x0018;
            cycles = 16;
            break;
        }

        /* LDH (a8), A */
        case 0xE0: {
            uint16_t addr = 0xFF00 + READ(cpu->pc++);
            WRITE(addr, cpu->a);
            cycles = 12;
            break;
        }

        /* POP HL */
        case 0xE1: {
            uint8_t low = READ(cpu->sp++);
            uint8_t high = READ(cpu->sp++);
            cpu->hl = (high << 8) | low;
            cycles = 12;
            break;
        }

        /* LD (C), A */
        case 0xE2: WRITE(0xFF00 + cpu->c, cpu->a); cycles = 8; break;

        /* Invalid Opcodes 0xE3, 0xE4 */
        case 0xE3: case 0xE4: cycles = 4; break;

        /* PUSH HL */
        case 0xE5: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->hl & 0xFF);
            WRITE(cpu->sp + 1, (cpu->hl >> 8) & 0xFF);
            cycles = 16;
            break;
        }

        /* AND d8 */
        case 0xE6: op_and(cpu, READ(cpu->pc++)); cycles = 8; break;

        /* RST 20H */
        case 0xE7: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = 0x0020;
            cycles = 16;
            break;
        }

        /* ADD SP, r8 */
        case 0xE8: {
            int8_t offset = (int8_t)READ(cpu->pc++);
            uint32_t res = cpu->sp + offset;
            cpu->f = 0;
            if (((cpu->sp & 0x0F) + (offset & 0x0F)) > 0x0F) cpu->f |= GB_FLAG_H;
            if (((cpu->sp & 0xFF) + (offset & 0xFF)) > 0xFF) cpu->f |= GB_FLAG_C;
            cpu->sp = res & 0xFFFF;
            cycles = 16;
            break;
        }

        /* JP (HL) */
        case 0xE9: cpu->pc = cpu->hl; cycles = 4; break;

        /* LD (a16), A */
        case 0xEA: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            uint16_t addr = (high << 8) | low;
            WRITE(addr, cpu->a);
            cycles = 16;
            break;
        }

        /* Invalid Opcodes 0xEB, 0xEC, 0xED */
        case 0xEB: case 0xEC: case 0xED: cycles = 4; break;

        /* XOR d8 */
        case 0xEE: op_xor(cpu, READ(cpu->pc++)); cycles = 8; break;

        /* RST 28H */
        case 0xEF: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = 0x0028;
            cycles = 16;
            break;
        }

        /* LDH A, (a8) */
        case 0xF0: {
            uint16_t addr = 0xFF00 + READ(cpu->pc++);
            cpu->a = READ(addr);
            cycles = 12;
            break;
        }

        /* POP AF */
        case 0xF1: {
            uint8_t low = READ(cpu->sp++);
            uint8_t high = READ(cpu->sp++);
            cpu->af = ((high << 8) | low) & 0xFFF0; /* Lower 4 bits of F must always be 0 */
            cycles = 12;
            break;
        }

        /* LD A, (C) */
        case 0xF2: cpu->a = READ(0xFF00 + cpu->c); cycles = 8; break;

        /* DI */
        case 0xF3: cpu->ime = false; cycles = 4; break;

        /* Invalid Opcode 0xF4 */
        case 0xF4: cycles = 4; break;

        /* PUSH AF */
        case 0xF5: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->af & 0xFF);
            WRITE(cpu->sp + 1, (cpu->af >> 8) & 0xFF);
            cycles = 16;
            break;
        }

        /* OR d8 */
        case 0xF6: op_or(cpu, READ(cpu->pc++)); cycles = 8; break;

        /* RST 30H */
        case 0xF7: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = 0x0030;
            cycles = 16;
            break;
        }

        /* LD HL, SP+r8 */
        case 0xF8: {
            int8_t offset = (int8_t)READ(cpu->pc++);
            uint32_t res = cpu->sp + offset;
            cpu->f = 0;
            if (((cpu->sp & 0x0F) + (offset & 0x0F)) > 0x0F) cpu->f |= GB_FLAG_H;
            if (((cpu->sp & 0xFF) + (offset & 0xFF)) > 0xFF) cpu->f |= GB_FLAG_C;
            cpu->hl = res & 0xFFFF;
            cycles = 12;
            break;
        }

        /* LD SP, HL */
        case 0xF9: cpu->sp = cpu->hl; cycles = 8; break;

        /* LD A, (a16) */
        case 0xFA: {
            uint8_t low = READ(cpu->pc++);
            uint8_t high = READ(cpu->pc++);
            uint16_t addr = (high << 8) | low;
            cpu->a = READ(addr);
            cycles = 16;
            break;
        }

        /* EI */
        case 0xFB: cpu->ime = true; cycles = 4; break;

        /* Invalid Opcodes 0xFC, 0xFD */
        case 0xFC: case 0xFD: cycles = 4; break;

        /* CP d8 */
        case 0xFE: op_cp(cpu, READ(cpu->pc++)); cycles = 8; break;

        /* RST 38H */
        case 0xFF: {
            cpu->sp -= 2;
            WRITE(cpu->sp, cpu->pc & 0xFF);
            WRITE(cpu->sp + 1, (cpu->pc >> 8) & 0xFF);
            cpu->pc = 0x0038;
            cycles = 16;
            break;
        }
    }

    cpu->cycles += cycles;
    return cycles;
}
