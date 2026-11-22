// MSR-nx: lookups for calls and jumps through registers, and the top-level run loop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "msr_cpu.h"
#include "msr_log.h"
#define MSR_INTERP_NO_MEM
#include "interp_cpu.h"

extern const unsigned msr_fn_count;
extern const u32 msr_fn_addr[];
extern void (*const msr_fn_ptr[])(Cpu* __restrict);

typedef void (*GuestFn)(Cpu* __restrict);

// Binary search over the sorted entry list (bit 0 of each entry marks Thumb). quiet: no log line
// for a mode mismatch (the interpreter asks at every instruction it runs).
static GuestFn lookup_at(u32 target, int quiet) {
    u32 a = target & ~1u;
    unsigned lo = 0, hi = msr_fn_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        u32 m = msr_fn_addr[mid] & ~1u;
        if (m == a) {
            if ((msr_fn_addr[mid] & 1u) != (target & 1u) && (target & 1u)) {
                // A Thumb target (bit 0 set) that our list has as ARM, or the reverse through a
                // pop/ldr pc. Running it in the wrong mode is never right.
                if (!quiet)
                    msr_log("target %08X is %s code, but the function list has it as %s: it runs on the interpreter",
                            target, (target & 1u) ? "Thumb" : "ARM", (msr_fn_addr[mid] & 1u) ? "Thumb" : "ARM");
                return 0;
            }
            return msr_fn_ptr[mid];
        }
        if (m < a) lo = mid + 1; else hi = mid;
    }
    return 0;
}
static GuestFn lookup(u32 target) { return lookup_at(target, 0); }

// ---- the fallback interpreter ---------------------------------------------------------------------
// Code the function list lacks (reached only through a pointer made at run time, as both of issue 1's
// stops were) runs on the interpreter (interp_arm.c, from msr2ds) instead of stopping the game. It runs
// until the code returns where it was called from, calling translated functions natively along the
// way. Each address it starts at is written to the receipt once, so the next build can translate it
// (an extra_entries.txt line, tools/switch/local_build.sh).

static u32 g_tls_rw;   // CP15 c13 c0 2, which only the interpreter keeps (the translation has no use for it)

static void to_interp(const Cpu* c, CPU* i, u32 pc) {
    memset(i, 0, sizeof *i);
    memcpy(i->r, c->r, sizeof i->r);
    i->n = c->n;
    i->z = c->z;
    i->cf = c->c;
    i->v = c->v;
    i->q = c->q;
    i->ge = c->ge;
    i->cpsr_rest = 0x10;   // user mode
    memcpy(i->s, c->vfp.s, sizeof i->s);
    i->fpscr = c->fpscr;
    i->tls = c->tls;
    i->tls_rw = g_tls_rw;
    i->excl_valid = c->excl_valid;
    i->excl_addr = c->excl_addr;
    if (c->excl_valid) i->excl_value = rd32(c->excl_addr);
    i->thumb = pc & 1u;
    i->r[15] = pc & ~1u;
}

static void from_interp(const CPU* i, Cpu* c) {
    memcpy(c->r, i->r, sizeof c->r);
    c->n = i->n;
    c->z = i->z;
    c->c = i->cf;
    c->v = i->v;
    c->q = i->q;
    c->ge = i->ge;
    memcpy(c->vfp.s, i->s, sizeof c->vfp.s);
    c->fpscr = i->fpscr;
    g_tls_rw = i->tls_rw;
    c->excl_valid = i->excl_valid;
    c->excl_addr = i->excl_addr;
    c->pc = i->r[15] | i->thumb;
}

static unsigned long long g_interp_steps;
unsigned long long msr_interp_steps(void) { return g_interp_steps; }   // the speed report

static void note_interpreted(u32 pc, u32 lr) {
    static u32 seen[64];
    static unsigned n;
    for (unsigned k = 0; k < n; k++)
        if (seen[k] == pc) return;
    if (n < 64) {
        seen[n++] = pc;
        msr_receipt("cpu: %08X (%s) runs on the interpreter: not in the function list (lr %08X). "
                    "For the next build: extra_entries.txt line 0x%08X", pc & ~1u, (pc & 1u) ? "Thumb" : "ARM", lr, pc);
    }
}

// Runs game code on the interpreter from pc until control reaches `until` (a return address, or the
// top level's stop address), or with stop_at_known, until it reaches code the translation has
// (left to the caller). On the way, translated functions it reaches are called natively.
static void interpret(Cpu* c, u32 pc, u32 until, int stop_at_known) {
    note_interpreted(pc, c->r[14]);
    CPU ic;
    to_interp(c, &ic, pc);
    for (int first = 1;; first = 0) {
        u32 cur = ic.r[15] | ic.thumb;
        if (cur == until) break;
        if (!first) {
            GuestFn f = lookup_at(cur, 1);
            if (f) {
                if (stop_at_known) break;
                from_interp(&ic, c);
                c->r[15] = cur & ~1u;
                f(c);                      // returns with c->pc where it went back to
                to_interp(c, &ic, c->pc);
                continue;
            }
        }
        int r = rt_interpret_step(&ic);
        g_interp_steps++;
        if (r == 1) {                      // SVC: ic.r[15] is already past it
            from_interp(&ic, c);
            msr_svc(c, ic.svc);
            to_interp(c, &ic, ic.r[15] | ic.thumb);
        } else if (r < 0) {
            from_interp(&ic, c);
            msr_receipt("fatal: the interpreter does not handle the instruction at %08X (%s, %08X)", ic.r[15],
                        ic.thumb ? "Thumb" : "ARM", ic.thumb ? rd16(ic.r[15]) : rd32(ic.r[15]));
            msr_fatal(c, "an instruction the interpreter does not handle");
        }
    }
    from_interp(&ic, c);
}

// The mode of the function holding addr (for an address inside a function, not at its entry).
static u32 mode_of(u32 addr) {
    unsigned lo = 0, hi = msr_fn_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if ((msr_fn_addr[mid] & ~1u) <= addr) lo = mid + 1; else hi = mid;
    }
    return lo ? (msr_fn_addr[lo - 1] & 1u) : 0;
}

void msr_call_indirect(Cpu* c, u32 target) {
    GuestFn f = lookup(target);
    if (!f) {
        interpret(c, target, c->r[14], 0);   // until it returns to the caller (c->pc is then lr)
        return;
    }
    f(c);
}

void msr_jump_indirect(Cpu* c, u32 target) {
    GuestFn f = lookup(target);
    if (f) { f(c); return; }        // a tail call through a register
    c->pc = target;                  // otherwise a return through a register (bx r3 and the like)
}

static unsigned g_unwinds;

void msr_unwind(Cpu* c, u32 call_site, u32 expected) {
    if (g_unwinds++ < 200)
        msr_log("unwind: call at %08X expected a return to %08X, got %08X (lr %08X sp %08X)",
                call_site, expected, c->pc, c->r[14], c->r[13]);
    if (g_unwinds == 1)
        msr_receipt("first unexpected return: call at %08X expected %08X, got %08X",
                    call_site, expected, c->pc);
}

void msr_unknown(Cpu* c, u32 addr, const char* what) {
    // The instruction at addr has not run (the registers are as it found them), except at the end
    // of a function that falls into code the list lacks, where it has.
    if (strcmp(what, "fell off the end") == 0) {
        msr_log("fatal: %s at %08X", what, addr);
        msr_fatal(c, what);
    }
    msr_log("%s at %08X: it runs on the interpreter", what, addr);
    // Interpreted until code the translation has; the translated callers then unwind to msr_run,
    // which goes on from there (as for any unexpected return).
    interpret(c, addr | mode_of(addr), 0xFFFFFFFFu, 1);
}

// Runs guest code from c->pc until the guest returns to `stop`. A function that returns to an
// address other than its call's expected one unwinds to here; we look the address up and go on.
void msr_run(Cpu* c, u32 entry, u32 stop) {
    c->pc = entry;
    for (;;) {
        u32 pc = c->pc;
        if (pc == stop) return;
        GuestFn f = lookup(pc);
        if (!f) {
            // code the list lacks, or a return into the middle of a function after an unwind
            msr_log("run: %08X is not a function entry (lr %08X sp %08X): the interpreter runs it",
                    pc, c->r[14], c->r[13]);
            interpret(c, pc, stop, 1);
            continue;
        }
        c->r[15] = pc;
        msr_log("run: entering %08X from the top level", pc);
        f(c);
    }
}

void msr_nan_seen(const char* fn, int reg, int dbl) {
    static const char* seen[256];
    static int n;
    for (int k = 0; k < n; k++) if (seen[k] == fn) return;
    if (n < 256) seen[n++] = fn;
    msr_log("nan: %s wrote NaN to %c%d", fn, dbl ? 'd' : 's', reg);
}
