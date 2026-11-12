#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "gb_cpu.h"

/* Simulated GB 64KB RAM */
static uint8_t mock_mem[65536];

static uint8_t mock_read(void *user, uint16_t addr) {
    (void)user;
    return mock_mem[addr];
}

static void mock_write(void *user, uint16_t addr, uint8_t val) {
    (void)user;
    mock_mem[addr] = val;
}

static void reset_test_env(gb_cpu_t *cpu) {
    memset(mock_mem, 0, sizeof(mock_mem));
    gb_cpu_init(cpu);
}

void test_load_instructions(void) {
    gb_cpu_t cpu;
    reset_test_env(&cpu);

    /* 0x06: LD B, 0x42 */
    mock_mem[0x0100] = 0x06;
    mock_mem[0x0101] = 0x42;
    /* 0x0E: LD C, 0x24 */
    mock_mem[0x0102] = 0x0E;
    mock_mem[0x0103] = 0x24;

    gb_cpu_step(&cpu, NULL, mock_read, mock_write);
    assert(cpu.b == 0x42);
    assert(cpu.pc == 0x0102);

    gb_cpu_step(&cpu, NULL, mock_read, mock_write);
    assert(cpu.c == 0x24);
    assert(cpu.pc == 0x0104);

    printf("PASS: Load instructions test\n");
}

void test_add_flags(void) {
    gb_cpu_t cpu;
    reset_test_env(&cpu);

    /* LD A, 0x0F */
    cpu.a = 0x0F;
    /* ADD A, 0x01 */
    mock_mem[0x0100] = 0xC6;
    mock_mem[0x0101] = 0x01;

    gb_cpu_step(&cpu, NULL, mock_read, mock_write);
    assert(cpu.a == 0x10);
    /* Should trigger Half-Carry (H) and clear Z, N, C */
    assert(cpu.f & GB_FLAG_H);
    assert(!(cpu.f & GB_FLAG_Z));
    assert(!(cpu.f & GB_FLAG_N));
    assert(!(cpu.f & GB_FLAG_C));

    /* Test Carry flag: LD A, 0xF0, ADD A, 0x10 */
    reset_test_env(&cpu);
    cpu.a = 0xF0;
    mock_mem[0x0100] = 0xC6;
    mock_mem[0x0101] = 0x10;
    gb_cpu_step(&cpu, NULL, mock_read, mock_write);
    assert(cpu.a == 0x00);
    assert(cpu.f & GB_FLAG_Z);
    assert(cpu.f & GB_FLAG_C);
    assert(!(cpu.f & GB_FLAG_N));

    printf("PASS: Addition flags test\n");
}

void test_cb_instructions(void) {
    gb_cpu_t cpu;
    reset_test_env(&cpu);

    /* Test BIT instruction: BIT 7, B */
    cpu.b = 0x80;
    /* CB 78: BIT 7, B */
    mock_mem[0x0100] = 0xCB;
    mock_mem[0x0101] = 0x78;

    gb_cpu_step(&cpu, NULL, mock_read, mock_write);
    /* BIT 7 of 0x80 is 1, so Zero flag should be cleared */
    assert(!(cpu.f & GB_FLAG_Z));
    assert(cpu.f & GB_FLAG_H);
    assert(!(cpu.f & GB_FLAG_N));

    /* Test SWAP instruction: SWAP A */
    reset_test_env(&cpu);
    cpu.a = 0x35;
    /* CB 37: SWAP A */
    mock_mem[0x0100] = 0xCB;
    mock_mem[0x0101] = 0x37;

    gb_cpu_step(&cpu, NULL, mock_read, mock_write);
    assert(cpu.a == 0x53);
    assert(!(cpu.f & GB_FLAG_Z));

    printf("PASS: CB-prefix instructions test\n");
}

void test_push_pop_af(void) {
    gb_cpu_t cpu;
    reset_test_env(&cpu);

    cpu.af = 0x1234;
    /* F5: PUSH AF */
    mock_mem[0x0100] = 0xF5;
    /* F1: POP AF */
    mock_mem[0x0101] = 0xF1;

    gb_cpu_step(&cpu, NULL, mock_read, mock_write);
    assert(cpu.sp == 0xFFFC);
    assert(mock_mem[0xFFFC] == 0x34); /* Low byte (F) */
    assert(mock_mem[0xFFFD] == 0x12); /* High byte (A) */

    gb_cpu_step(&cpu, NULL, mock_read, mock_write);
    assert(cpu.sp == 0xFFFE);
    assert(cpu.af == 0x1230);

    printf("PASS: PUSH/POP AF lower 4 bits zeroed test\n");
}

int main(void) {
    printf("Starting NativeRa CPU Core Known Answer Tests (KATs)...\n");
    test_load_instructions();
    test_add_flags();
    test_cb_instructions();
    test_push_pop_af();
    printf("ALL NATIVERA CPU CORE KATs PASSED SUCCESSFULLY!\n");
    return 0;
}
