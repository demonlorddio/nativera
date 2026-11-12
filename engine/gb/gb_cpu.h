#ifndef NATIVERA_GB_CPU_H
#define NATIVERA_GB_CPU_H

#include <stdint.h>
#include <stdbool.h>

/* LR35902 (Game Boy CPU) State */
typedef struct {
    union {
        struct { uint8_t f, a; };
        uint16_t af;
    };
    union {
        struct { uint8_t c, b; };
        uint16_t bc;
    };
    union {
        struct { uint8_t e, d; };
        uint16_t de;
    };
    union {
        struct { uint8_t l, h; };
        uint16_t hl;
    };
    uint16_t sp;
    uint16_t pc;

    bool ime;         /* Interrupt Master Enable */
    bool halted;
    bool stopped;

    uint8_t int_flags; /* IF register */
    uint8_t int_enable;/* IE register */

    uint64_t cycles;  /* Total elapsed CPU cycles */
} gb_cpu_t;

/* GB Flag masks */
#define GB_FLAG_Z 0x80
#define GB_FLAG_N 0x40
#define GB_FLAG_H 0x20
#define GB_FLAG_C 0x10

/* Memory read/write callbacks for the CPU */
typedef uint8_t (*gb_read_cb_t)(void *user, uint16_t addr);
typedef void    (*gb_write_cb_t)(void *user, uint16_t addr, uint8_t val);

void gb_cpu_init(gb_cpu_t *cpu);
int  gb_cpu_step(gb_cpu_t *cpu, void *user, gb_read_cb_t read_mem, gb_write_cb_t write_mem);

#endif /* NATIVERA_GB_CPU_H */
