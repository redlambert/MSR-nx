// MSR-nx: a differential test of the fallback interpreter against the translated code (Linux).
// For each sampled game function, from the same random registers and memory:
//   A: the translated function (native)
//   B: the interpreter alone (every instruction interpreted, calls too)
//   C: dispatch.c's interpret() as msr_call_indirect uses it (interpreter, native for known code)
// then registers, flags, VFP and all memory are compared. Runs where the native one faults, runs long
// or jumps into data (random inputs do that) are skipped; the interpreter gets a longer time limit.
//
// Build (generated code in build/gen, compiled to build/xobj with gcc -O1 -w -fno-strict-aliasing
// -ffp-contract=off -Iruntime/cpu -Iruntime/os -Ibuild/gen):
//   gcc -O1 -w -fno-strict-aliasing -ffp-contract=off -Iruntime/cpu -Iruntime/os -Ibuild/gen \
//       -c tools/tests/interp_diff_test.c -o build/interp_diff_test.o
//   gcc -no-pie -o build/interp_diff_test build/interp_diff_test.o runtime/cpu/interp_arm.c \
//       runtime/os/mem.c runtime/os/extras.c build/xobj/*.o -Iruntime/cpu -Iruntime/os -lm
//   build/interp_diff_test extract 8000
// 0.7.0 on Linux x86-64, 8000 functions sampled: 2752 tested, 2747 the same fully interpreted and 2748 the
// same mixed. The rest: 3 interpreter runs past the time limit, one jump into data from random input, and
// one function whose flags differ after it returns (the translation's byte jump table stand-in leaves
// the flags the helper would set; flags are dead after a call).
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include "dispatch.c"
#include "msr_mem.h"

volatile u32 msr_frame_limit_ms = 33; volatile int msr_fps60;
static sigjmp_buf g_jb;
static volatile int g_in_run;
static unsigned g_svc_calls;
void msr_log(const char* fmt, ...) { (void)fmt; }
void msr_receipt(const char* fmt, ...) { (void)fmt; }
void msr_fatal(Cpu* c, const char* why) { (void)c; (void)why; siglongjmp(g_jb, 3); }
void msr_svc(Cpu* c, u32 imm) { g_svc_calls++; c->r[0] = imm * 7u; (void)c; }
static void on_sig(int s) { if (g_in_run) siglongjmp(g_jb, s == SIGALRM ? 2 : 1); _exit(99); }

static u32 rng = 1;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

#define HEAP 0x08000000u
#define HEAP_SIZE 0x400000u
#define STACK_TOP 0x10000000u
#define STACK_SIZE 0x40000u
#define MAGIC 0xFFFFFFF0u
static u8* snap_heap; static u8* snap_stack; static u8* snap_data;
static u32 data_base, data_size;

static u8 snap_tls[0x8000];
static void save_mem(u8* h, u8* s, u8* d) { memcpy(h, g_mem + HEAP, HEAP_SIZE); memcpy(s, g_mem + STACK_TOP - STACK_SIZE, STACK_SIZE); memcpy(d, g_mem + data_base, data_size); }
static void load_mem(const u8* h, const u8* s, const u8* d) { memcpy(g_mem + 0x1FF80000, snap_tls, sizeof snap_tls); memcpy(g_mem + HEAP, h, HEAP_SIZE); memcpy(g_mem + STACK_TOP - STACK_SIZE, s, STACK_SIZE); memcpy(g_mem + data_base, d, data_size); }

typedef struct { Cpu c; u8 *h, *s, *d; int status; } Result;

static void setup_cpu(Cpu* c, u32 seed) {
    rng = seed * 2654435761u + 1;
    memset(c, 0, sizeof *c);
    for (int k = 0; k < 13; k++) {
        u32 v = rnd();
        c->r[k] = (v & 3) ? HEAP + 0x1000 + (rnd() % (HEAP_SIZE - 0x2000)) / 4 * 4 : (v >> 2) % 256;
    }
    c->r[13] = STACK_TOP - 0x10000;
    c->r[14] = MAGIC;
    for (int k = 0; k < 32; k++) { float f = (float)((int)(rnd() % 2000) - 1000) / 7.0f; memcpy(&c->vfp.s[k], &f, 4); }
    c->n = rnd() & 1; c->z = rnd() & 1; c->c = rnd() & 1; c->v = rnd() & 1;
    c->fpscr = 0;
    c->tls = 0x1FF82000u;
}

static int pure_interp(Cpu* c, u32 entry) {
    CPU ic; to_interp(c, &ic, entry);
    for (unsigned long n = 0;; n++) {
        if ((ic.r[15] | ic.thumb) == MAGIC) break;
        int r = rt_interpret_step(&ic);
        if (r == 1) { from_interp(&ic, c); msr_svc(c, ic.svc); to_interp(c, &ic, ic.r[15] | ic.thumb); }
        else if (r < 0) { static int shown; if (shown++ < 40) printf("  unsupported at %08X: %s %08X\n", ic.r[15], ic.thumb ? "T" : "A", ic.thumb ? rd16(ic.r[15]) : rd32(ic.r[15])); return 4; }
    }
    from_interp(&ic, c);
    return 0;
}

static int run(int mode, u32 entry, GuestFn f, Cpu* c) {
    struct itimerval t = {{0, 0}, {mode ? 2 : 0, mode ? 0 : 200000}};
    int st = sigsetjmp(g_jb, 1);
    if (st) { g_in_run = 0; struct itimerval z = {{0,0},{0,0}}; setitimer(ITIMER_REAL, &z, 0); return st; }
    g_in_run = 1;
    setitimer(ITIMER_REAL, &t, 0);
    if (mode == 0) { c->r[15] = entry & ~1u; f(c); }
    else if (mode == 1) { int r = pure_interp(c, entry); if (r) { g_in_run = 0; return r; } }
    else interpret(c, entry, MAGIC, 0);
    struct itimerval z = {{0,0},{0,0}}; setitimer(ITIMER_REAL, &z, 0);
    g_in_run = 0;
    return 0;
}

static int same(const Cpu* a, const Cpu* b, const char** what) {
    for (int k = 0; k < 15; k++) if (a->r[k] != b->r[k]) { static char w[32]; snprintf(w, sizeof w, "r%d", k); *what = w; return 0; }
    if (a->pc != b->pc) { *what = "pc"; return 0; }
    if (a->n != b->n || a->z != b->z || a->c != b->c || a->v != b->v) { *what = "flags"; return 0; }
    if (memcmp(a->vfp.s, b->vfp.s, sizeof a->vfp.s)) { *what = "vfp"; return 0; }
    return 1;
}

int main(int argc, char** argv) {
    const char* dir = argv[1];
    unsigned count = argc > 2 ? (unsigned)atoi(argv[2]) : 2000;
    signal(SIGSEGV, on_sig); signal(SIGBUS, on_sig); signal(SIGALRM, on_sig); signal(SIGFPE, on_sig);
    msr_mem_init();
    char path[600]; snprintf(path, sizeof path, "%s/code.bin", dir);
    FILE* fp = fopen(path, "rb"); fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    msr_mem_map(0x100000, (u32)sz + 0x80000, MSR_PERM_RW, MSR_STATE_CODE, "code");
    fread(g_mem + 0x100000, 1, sz, fp); fclose(fp);
    data_base = 0x100000; data_size = (u32)(sz + 0x80000);   // the whole image: a stray store into code would make the interpreter differ
    msr_mem_map(HEAP, HEAP_SIZE, MSR_PERM_RW, MSR_STATE_PRIVATE, "heap");
    msr_mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE, MSR_PERM_RW, MSR_STATE_PRIVATE, "stack");
    msr_mem_map(0x1FF80000, 0x8000, MSR_PERM_RW, MSR_STATE_PRIVATE, "tls");
    // heap: pointers to itself and small numbers, so walks of game structures stay in mapped memory
    rng = 7;
    for (u32 a = HEAP; a < HEAP + HEAP_SIZE; a += 4) { u32 v = rnd(); wr32(a, (v & 1) ? HEAP + 0x1000 + (rnd() % (HEAP_SIZE - 0x2000)) / 4 * 4 : (v >> 1) % 64); }
    u8 *h0 = malloc(HEAP_SIZE), *s0 = malloc(STACK_SIZE), *d0 = malloc(data_size);
    save_mem(h0, s0, d0);
    memcpy(snap_tls, g_mem + 0x1FF80000, sizeof snap_tls);
    Result R[3]; for (int k = 0; k < 3; k++) { R[k].h = malloc(HEAP_SIZE); R[k].s = malloc(STACK_SIZE); R[k].d = malloc(data_size); }
    unsigned tested = 0, skipped = 0, bad[3] = {0}, ok_pure = 0, ok_mixed = 0, interp_only_fail = 0;
    unsigned long long pure_steps = 0;
    u32 seed = 1;
    for (unsigned t = 0; t < count; t++) {
        unsigned idx = (unsigned)(((unsigned long long)rnd() * 7919u + t * 104729u) % msr_fn_count);
        u32 entry = msr_fn_addr[idx];
        GuestFn f = msr_fn_ptr[idx];
        int st[3];
        for (int m = 0; m < 3; m++) {
            load_mem(h0, s0, d0);
            setup_cpu(&R[m].c, seed + t);
            st[m] = run(m, entry, f, &R[m].c);
            save_mem(R[m].h, R[m].s, R[m].d);
        }
        if (st[0] || R[0].c.pc != MAGIC) { skipped++; continue; }   // the native run faulted, ran long or jumped into data on this input
        tested++;
        for (int m = 1; m < 3; m++) {
            const char* what = "status";
            int good = st[m] == 0 && same(&R[0].c, &R[m].c, &what);
            if (good && memcmp(R[0].h, R[m].h, HEAP_SIZE)) { good = 0; what = "heap"; }
            if (good && memcmp(R[0].s, R[m].s, STACK_SIZE)) { good = 0; what = "stack"; }
            if (good && memcmp(R[0].d, R[m].d, data_size)) { good = 0; what = "data"; }
            if (good) { if (m == 1) ok_pure++; else ok_mixed++; }
            else {
                bad[m]++;
                if (bad[m] <= 12) printf("%s differs at %08X: %s (status %d)\n", m == 1 ? "pure" : "mixed", entry, what, st[m]);
            }
        }
    }
    printf("functions tried %u, tested %u (native ran cleanly), skipped %u\n", count, tested, skipped);
    printf("pure interpreter: %u same, %u differ; mixed (as the port runs it): %u same, %u differ\n", ok_pure, bad[1], ok_mixed, bad[2]);
    printf("interpreter instructions run in mixed mode: %llu\n", g_interp_steps);
    return 0;
}
