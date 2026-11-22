// MSR-nx: the fallback interpreter's view of the CPU (interp_arm.c, from msr2ds). Field names are
// msr2ds's (its native/runtime/cpu.h), so interp_arm.c builds unchanged; cpu/dispatch.c copies the
// translated code's Cpu into one of these before interpreting and back after.
#pragma once
#include <stdint.h>
#include <string.h>

// Game memory, as msr_cpu.h has it (not included here: interp_arm.c has helpers of its own with
// the same names as some of msr_cpu.h's). cpu/dispatch.c, which includes both, defines
// MSR_INTERP_NO_MEM and uses msr_cpu.h's.
#ifndef MSR_INTERP_NO_MEM
extern uint8_t* g_mem;
static inline uint32_t rd8(uint32_t a) { return g_mem[a]; }
static inline uint32_t rd16(uint32_t a) { uint16_t v; memcpy(&v, g_mem + a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, g_mem + a, 4); return v; }
static inline void wr8(uint32_t a, uint32_t v) { g_mem[a] = (uint8_t)v; }
static inline void wr16(uint32_t a, uint32_t v) { uint16_t x = (uint16_t)v; memcpy(g_mem + a, &x, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy(g_mem + a, &v, 4); }
#endif

typedef struct CPU {
    uint32_t r[16];
    uint32_t n, z, cf, v;     // flags, 0 or 1 (Cpu's c is cf here)
    uint32_t q;
    uint32_t ge;
    uint32_t cpsr_rest;       // mode and mask bits (0x10: user)
    uint32_t thumb;
    uint32_t s[32];           // VFP S0-S31 (D0-D15 as pairs, low word first, as Cpu's union)
    uint32_t fpscr;
    uint32_t tls;             // CP15 c13 c0 3, read only to the game
    uint32_t tls_rw;          // CP15 c13 c0 2
    uint32_t exit;
    uint32_t svc;             // the SVC number when a step returns STEP_SVC
    uint32_t excl_valid, excl_addr;
    uint64_t excl_value;
} CPU;

enum { EXIT_BRANCH = 0, EXIT_SVC = 1 };

// One ARM or Thumb instruction at r[15]: 0 ran, 1 an SVC (r[15] already past it), -1 not handled
// (r[15] left on it).
int rt_interpret_step(CPU* c);
