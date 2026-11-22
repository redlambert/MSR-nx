"""Generates C for every function of the game.

Each guest function becomes `void F_xxxxxxxx(Cpu* c)`: branches inside it are gotos, direct calls
are real C calls (followed by a check that the callee returned where the guest expects; if not,
control unwinds to the runtime, which looks up c->pc), returns are real C returns, jump tables are
switches. Output is split into units of about 5 MB for the compiler.

Usage: python3 generate.py <extract dir> <functions.csv> <out dir> [extra_entries.txt]
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode  # noqa: E402
import emit  # noqa: E402
import regs  # noqa: E402

UNIT_BYTES = 5 * 1024 * 1024


def fname(addr):
    return 'F_%08X' % addr


class FnHooks:
    def __init__(self, prog, fn, stats):
        self.prog = prog
        self.fn = fn
        self.stats = stats

    def leave(self, e, target_expr):
        e.line(regs.sync_out())
        e.line('c->pc = %s; return;' % target_expr)

    def tail(self, e, target):
        e.line(regs.sync_out())
        e.line('%s(c); return;' % fname(target & ~1))
        self.stats['tailcalls'] += 1

    def branch(self, e, target):
        if target in self.fn.insns and (target != self.fn.addr or True):
            e.line('goto L_%08X;' % target)
        elif target in self.prog.entries:
            self.tail(e, target)
        else:
            self.unknown(e, 'branch to undecoded %08X' % target)

    def call(self, e, target, ret):
        if e.ins.addr in self.fn.tables:   # blx __ARM_common_switch8: a byte jump table
            targets = self.fn.tables[e.ins.addr]
            n = len(targets) - 1
            e.line('{ u32 sw_i = r3 < %du ? r3 : %du;' % (n, n))
            e.line('  r3 = (u32)rd8(%s + sw_i); r12 = %s + 2u * r3;'
                   % (emit.h((ret & ~1) + 1), emit.h(ret)))
            e.line('  switch (sw_i) {')
            for k, (x, _) in enumerate(targets):
                if x in self.fn.insns:
                    e.line('    case %d: goto L_%08X;' % (k, x))
            e.line('    default: break;')
            e.line('  } }')
            self.unknown(e, 'switch8 table %08X out of range' % e.ins.addr)
            self.stats['tables'] += 1
            return
        a = target & ~1
        e.line(regs.sync_out())
        if a in self.prog.entries:
            e.line('%s(c);' % fname(a))
        else:
            e.line('msr_call_indirect(c, %s);' % emit.h(target))
            self.stats['calls_to_unknown'] += 1
        e.line('if (__builtin_expect(c->pc != %s, 0)) { msr_unwind(c, %s, %s); return; }'
               % (emit.h(ret), emit.h(e.ins.addr), emit.h(ret)))
        e.line(regs.sync_in())

    def call_indirect(self, e, t, ret):
        e.line(regs.sync_out())
        e.line('msr_call_indirect(c, %s);' % t)
        e.line('if (__builtin_expect(c->pc != %s, 0)) { msr_unwind(c, %s, %s); return; }'
               % (emit.h(ret), emit.h(e.ins.addr), emit.h(ret)))
        e.line(regs.sync_in())
        self.stats['icalls'] += 1

    def bx(self, e, t, is_lr):
        if is_lr:
            self.leave(e, t)
        else:
            e.line(regs.sync_out())
            e.line('msr_jump_indirect(c, %s); return;' % t)
            self.stats['ijumps'] += 1

    def write_pc(self, e, t, is_load):
        addr = e.ins.addr
        if addr in self.fn.tables:
            targets = sorted({x for x, _ in self.fn.tables[addr]})
            e.line('switch (%s) {' % t)
            for x in targets:
                if x in self.fn.insns:
                    e.line('  case %s: goto L_%08X;' % (emit.h(x), x))
            e.line('  default: break;')
            e.line('}')
            self.unknown(e, 'jump table %08X out of range' % addr)
            self.stats['tables'] += 1
            return
        if not is_load and not e.ins.thumb:
            t = '(%s & ~3u)' % t
        ins = e.ins
        from_stack = ins.id in (emit.A.ARM_INS_POP, emit.A.ARM_INS_LDM) or (
            ins.id == emit.A.ARM_INS_LDR and ins.operands[1].type == emit.A.ARM_OP_MEM and
            ins.operands[1].mem.base == emit.A.ARM_REG_SP)
        if is_load and from_stack:
            self.leave(e, t)   # pop {pc} / ldr pc, [sp], #4: a return
        elif is_load:
            # A load into pc that is not a pop and not a detected table: try this function's
            # block starts first (an undetected jump table), otherwise leave through the runtime.
            self.switch_leaders(e, t)
            e.line(regs.sync_out())
            e.line('msr_jump_indirect(c, %s); return;' % t)
            self.stats['ijumps'] += 1
        else:
            # mov pc, lr is a return; anything else goes through the runtime.
            if e.ins.id == emit.A.ARM_INS_MOV and e.ins.operands[1].type == emit.A.ARM_OP_REG and \
                    e.ins.operands[1].reg == emit.A.ARM_REG_LR:
                self.leave(e, t)
            else:
                e.line(regs.sync_out())
                e.line('msr_jump_indirect(c, %s); return;' % t)
                self.stats['ijumps'] += 1

    def switch_leaders(self, e, t):
        targets = sorted(self.fn.leaders & set(self.fn.insns))
        e.line('switch (%s) {' % t)
        for x in targets:
            e.line('  case %s: goto L_%08X;' % (emit.h(x | (1 if self.fn.thumb else 0)), x))
        e.line('  default: break;')
        e.line('}')
        self.stats['leader_switches'] += 1

    def svc(self, e, imm):
        nxt = (e.ins.addr + e.ins.size) | (1 if e.ins.thumb else 0)
        e.line(regs.sync_out())
        e.line('c->pc = %s; msr_svc(c, 0x%X);' % (emit.h(nxt), imm))
        e.line(regs.sync_in())
        self.stats['svc'] += 1

    def unknown(self, e, what):
        e.line(regs.sync_out())
        e.line('msr_unknown(c, %s, "%s"); return;' % (emit.h(e.ins.addr), what))


# Instructions given their own C instead of the translation: the frame rate (port menu, 30 or 60).
# The game advances by the measured time between frames, so 60 needs only a shorter frame limit
# and three interface updates that step by a constant 1/30 s to take the measured time instead.
# Addresses and the facts behind them: gminustwo/msr2ds src/msr/frame_rate.h (read as reference);
# checked against our own translation (unit_001.c: cmp r0, #0x21 at 0x156484).
PATCHES = {
    # main loop (0x156450): sleep until msr_frame_limit_ms (33 at 30 fps, 16 at 60) since the last frame
    0x156484: ['{', '  extern volatile u32 msr_frame_limit_ms;', '  (void)subs_(r0, msr_frame_limit_ms, 1, &N, &Z, &C, &V);', '}'],
    0x15648C: ['{', '  extern volatile u32 msr_frame_limit_ms;', '  r6 = msr_frame_limit_ms - r0;', '}'],
    # the HUD's update (0x49DFDC): vldr s16, =1/30; at 60 fps the frame time it is given (s0)
    0x49DFF8: ['{', '  extern volatile int msr_fps60;', '  if (msr_fps60) c->vfp.s[16] = c->vfp.s[0];',
               '  else c->vfp.s[16] = rd32(0x49E000u + 0x298u);', '}'],
    # another interface update (0x4E3B40): vldr s16, =1/30 over the frame time; at 60 fps kept
    0x4E3BA4: ['{', '  extern volatile int msr_fps60;', '  if (!msr_fps60) c->vfp.s[16] = rd32(0x4E3BACu + 0x354u);', '}'],
    # the interface's fixed-step mode (0x10C688): vldrne s0, [r4, #0x340]; at 60 fps the measured time stays
    0x10C6B4: ['if (!Z) {', '  extern volatile int msr_fps60;', '  if (!msr_fps60) c->vfp.s[0] = rd32(r4 + 0x340u);', '}'],
}
# what each patched instruction must be, so a different game version is not patched blindly
PATCH_EXPECT = {0x156484: 'cmp', 0x15648C: 'rsb', 0x49DFF8: 'vldr', 0x4E3BA4: 'vldr', 0x10C6B4: 'vldrne'}

# Instructions that follow a port menu switch: with the bit of msr_extras (runtime/os/extras.h) set,
# the replacement runs; without it, the game's own instruction. Each entry: the bit, the game's word
# (checked, so another game version stops the build), and the replacement's C (a 'b <addr>' entry is
# a branch, emitted as the translator emits one).
# Addresses and the facts behind them: gminustwo/msr2ds src/msr/cheats.h (read as reference).
MOV_R0_1 = ['r0 = 1u;']
MOV_R1_1 = ['r1 = 1u;']
STRB_R0_R3 = ['wr8(r3, r0);']   # the flag a replaced settings read (bl 0x1EDC10) would have stored
GALLERIES, DIFFICULTY, FUSION = 'MSR_EXTRA_GALLERIES', 'MSR_EXTRA_DIFFICULTY', 'MSR_EXTRA_FUSION'
GATED = {
    # The amiibo unlocks a save's profile summary reads (0x3F8xxx): ldr r0, [sp, #0x88]; bl 0x1EDC10
    # for FusionMode, then GalleryMetroidIIUnlocked, GalleryConceptArtUnlocked, GalleryMusicUnlocked.
    0x3F8D40: (DIFFICULTY, 0xE59D0088, MOV_R0_1),
    0x3F8D44: (DIFFICULTY, 0xEBF7D3B1, STRB_R0_R3),
    0x3F8E10: (GALLERIES, 0xE59D0088, MOV_R0_1),
    0x3F8E14: (GALLERIES, 0xEBF7D37D, STRB_R0_R3),
    0x3F8E38: (GALLERIES, 0xE59D0088, MOV_R0_1),
    0x3F8E3C: (GALLERIES, 0xEBF7D373, STRB_R0_R3),
    0x3F8E60: (GALLERIES, 0xE59D0088, MOV_R0_1),
    0x3F8E64: (GALLERIES, 0xEBF7D369, STRB_R0_R3),
    # the menus' checks of the save's Fusion Mode flag (+0x6C of the slot's summary)
    0x4D828C: (DIFFICULTY, 0xE5D0006C, MOV_R0_1),
    0x4DD3F4: (DIFFICULTY, 0xE5D0006C, MOV_R0_1),
    0x4E2134: (DIFFICULTY, 0xE5D1106C, MOV_R1_1),
    # New Game goes to the difficulty list as if the game were beaten (+0x68)
    0x4E3430: (DIFFICULTY, 0xE5D00068, MOV_R0_1),
    # a new file's Play: push {r4-r8, lr} -> b 0x4E33E0, the handler that opens the list
    # (runtime/os/extras.c adapts the list to an empty slot)
    0x4E30C8: (DIFFICULTY, 0xE92D41F0, 'b 0x4E33E0'),
    # the galleries' own screens: str r4, [sp] (the default); bl 0x1EDC10
    0x492504: (GALLERIES, 0xE58D4000, MOV_R0_1),
    0x492508: (GALLERIES, 0xEBF56DC0, STRB_R0_R3),
    0x495BC8: (GALLERIES, 0xE58D4000, MOV_R0_1),
    0x495BCC: (GALLERIES, 0xEBF5600F, STRB_R0_R3),
    0x495D54: (GALLERIES, 0xE58D4000, MOV_R0_1),
    0x495D58: (GALLERIES, 0xEBF55FAC, STRB_R0_R3),
    # the game object's Fusion flag (+0x1859) where it picks Samus's packages, her models, the scripts'
    # LoadModelByFusion and the interface's suit sprites; left alone: the damage multiplier (0x384CBC)
    # and the save's difficulty (0x130454, 0x263460)
    0x10D438: (FUSION, 0xE5D01059, MOV_R1_1),
    0x2707A0: (FUSION, 0xE5D01059, MOV_R1_1),
    0x143EF8: (FUSION, 0xE7D00006, MOV_R0_1),
    0x404080: (FUSION, 0xE5D01059, MOV_R1_1),
    0x3A83D4: (FUSION, 0xE5D00059, MOV_R0_1),
    0x250448: (FUSION, 0xE5D00059, MOV_R0_1),
    0x453468: (FUSION, 0xE5D00059, MOV_R0_1),
    0x473544: (FUSION, 0xE5D00059, MOV_R0_1),
}

# Game functions that call a port function on entry (runtime/os/extras.c), with the calls into them
# that the hooks recognise by their return address: checked, as GATED's words are.
ENTRY_HOOKS = {
    0x4E40A0: ('msr_hook_state_change', [0x4E3470, 0x4DD4D4]),
    0x1913C0: ('msr_hook_text_copy', [0x4DF380, 0x4DDDF0, 0x4DF2DC]),
    0x4C1570: ('msr_hook_music_fade', [0x4E36FC, 0x4E3554, 0x4E3898]),
    0x51BB14: ('msr_hook_heap_setup', [0x51BC14, 0x51BC34]),
}


def check_versions(prog):
    """Stops the build when the game's code is not the version GATED and ENTRY_HOOKS were made for."""
    for a, (_, word, _) in GATED.items():
        if prog.word(a) != word:
            raise SystemExit('extras: the word at %08X is %08X, expected %08X: another game version?' % (a, prog.word(a), word))
    for fn, (_, rets) in ENTRY_HOOKS.items():
        if fn not in prog.functions:
            raise SystemExit('extras: %08X is not a function entry: another game version?' % fn)
        for r in rets:
            bl = prog.word(r - 4)
            off = ((bl & 0xFFFFFF) ^ 0x800000) - 0x800000
            if bl >> 24 != 0xEB or r + 4 + off * 4 != fn:
                raise SystemExit('extras: no call to %08X returning to %08X: another game version?' % (fn, r))


def gen_function(prog, fn, stats, problems):
    out = ['void %s(Cpu* __restrict c) {' % fname(fn.addr)]
    if fn.addr in ENTRY_HOOKS:
        # the registers are in Cpu at a function's entry; the hook may change them before they load
        out.append('  { extern void %s(Cpu* c); %s(c); }' % (ENTRY_HOOKS[fn.addr][0], ENTRY_HOOKS[fn.addr][0]))
        stats['entry_hooks'] += 1
    out += ['  ' + l for l in regs.declare_in()]
    hooks = FnHooks(prog, fn, stats)
    addrs = sorted(fn.insns)
    if addrs and addrs[0] != fn.addr:
        # Code reached by a branch lies below the entry: start at the entry, not the lowest address.
        out.append('  goto L_%08X;' % fn.addr)
        stats['entry_not_first'] += 1
    for k, a in enumerate(addrs):
        ins = fn.insns[a]
        out.append('L_%08X: // %s %s' % (a, ins.mnem, ins.op_str))
        if a in PATCHES:
            if ins.mnem != PATCH_EXPECT[a]:
                raise SystemExit('patch at %08X expects %s, found %s %s' % (a, PATCH_EXPECT[a], ins.mnem, ins.op_str))
            out += ['  ' + l for l in PATCHES[a]]
            stats['patched'] += 1
            nxt = a + ins.size
            following = addrs[k + 1] if k + 1 < len(addrs) else None
            if following != nxt and nxt in fn.insns:
                out.append('  goto L_%08X;' % nxt)
            continue
        try:
            body = emit.emit(ins, hooks)
        except emit.Unsupported as ex:
            problems[str(ex)] += 1
            e = emit.Emit(ins, hooks)
            hooks.unknown(e, 'untranslated %s' % ins.mnem)
            body = ['{'] + ['  ' + l for l in e.out] + ['}']
        if a in GATED:
            bit, word, repl = GATED[a]
            if isinstance(repl, str):   # a branch
                e = emit.Emit(ins, hooks)
                hooks.branch(e, int(repl.split()[1], 16))
                repl = e.out
            out.append('  if (msr_extras & %s) {' % bit)
            out += ['    ' + l for l in repl]
            out.append('  } else {')
            out += ['    ' + l for l in body]
            out.append('  }')
            stats['gated'] += 1
        else:
            out += ['  ' + l for l in body]
        nxt = a + ins.size
        following = addrs[k + 1] if k + 1 < len(addrs) else None
        if following != nxt and can_fall_through(ins):
            if nxt in fn.insns:
                out.append('  goto L_%08X;' % nxt)
            elif nxt in prog.entries:
                e = emit.Emit(ins, hooks)
                hooks.tail(e, nxt)
                out += ['  ' + l for l in e.out]
            else:
                e = emit.Emit(ins, hooks)
                hooks.unknown(e, 'fell off the end')
                out += ['  ' + l for l in e.out]
                stats['fall_off'] += 1
    out.append('}')
    # Labels nobody jumps to are fine; silence the warning in the build flags.
    return out


def can_fall_through(ins):
    """False for instructions that always leave (unconditional branch, return, table)."""
    from capstone import arm as A
    if ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
        return True
    if ins.id in (A.ARM_INS_B, A.ARM_INS_BX):
        return False
    if decode.writes_pc(ins):
        return False
    if ins.id == A.ARM_INS_UDF:
        return False
    return True


def main():
    extract, functions_csv, out_dir = sys.argv[1:4]
    extra = sys.argv[4] if len(sys.argv) > 4 else None
    prog = decode.load(extract, functions_csv, extra).run()
    check_versions(prog)
    os.makedirs(out_dir, exist_ok=True)
    stats = collections.Counter()
    problems = collections.Counter()
    header = ['#include "msr_cpu.h"', '#define DECL(a) void a(Cpu* __restrict c);']
    decls = ['void %s(Cpu* __restrict c);' % fname(a) for a in sorted(prog.functions)]
    open(os.path.join(out_dir, 'functions.h'), 'w').write('#pragma once\n#include "msr_cpu.h"\n#include "extras.h"\n' + '\n'.join(decls) + '\n')
    unit, size, n_units = [], 0, 0

    def flush():
        nonlocal unit, size, n_units
        if not unit:
            return
        path = os.path.join(out_dir, 'unit_%03d.c' % n_units)
        open(path, 'w').write('#include "functions.h"\n' + '\n'.join(unit) + '\n')
        n_units += 1
        unit, size = [], 0

    for a in sorted(prog.functions):
        lines = gen_function(prog, prog.functions[a], stats, problems)
        text = '\n'.join(lines)
        unit.append(text)
        size += len(text)
        if size >= UNIT_BYTES:
            flush()
    flush()
    # Function table for the runtime: sorted addresses (bit 0 = Thumb) and pointers.
    tab = ['#include "functions.h"', 'const unsigned msr_fn_count = %d;' % len(prog.functions),
           'const u32 msr_fn_addr[] = {']
    tab += ['  0x%08Xu,' % (a | (1 if prog.functions[a].thumb else 0)) for a in sorted(prog.functions)]
    tab += ['};', 'void (*const msr_fn_ptr[])(Cpu* __restrict) = {']
    tab += ['  %s,' % fname(a) for a in sorted(prog.functions)]
    tab += ['};']
    open(os.path.join(out_dir, 'table.c'), 'w').write('\n'.join(tab) + '\n')
    print('functions %d (%d found as pc-relative code pointers), units %d' % (len(prog.functions), getattr(prog, 'found_pc_relative', 0), n_units))
    print('found by the generator beyond the function list:', dict(prog.found_unreached))
    with open(os.path.join(out_dir, 'found_functions.txt'), 'w') as f:
        for t, thumb, why in prog.found_list:
            f.write('%08X %s %s\n' % (t, 'thumb' if thumb else 'arm', why))
    print('stats', dict(stats))
    print('untranslated', sum(problems.values()))
    for k, v in problems.most_common(30):
        print('  ', v, k)


if __name__ == '__main__':
    main()
