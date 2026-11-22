// MSR-nx: extras the game normally gives through amiibo or after beating it, available from the
// start (port menu rows), and the game's package heap made 8 MB larger.
#pragma once
#include "msr_cpu.h"

// Bits of msr_extras. The instruction patches compiled into the game code (tools/gen/generate.py
// GATED) and the hooks below test these at run time, so the port menu can turn each one on or off.
#define MSR_EXTRA_GALLERIES  1u   // concept art, Metroid II art and the sound test in Options > Gallery
#define MSR_EXTRA_DIFFICULTY 2u   // Normal, Hard and Fusion offered for every file, a new one too
#define MSR_EXTRA_FUSION     4u   // Samus in the Fusion suit at any difficulty (damage stays the difficulty's)

extern volatile u32 msr_extras;

// Called by msr_boot once code.bin is in memory, before the game runs: the package heap 8 MB larger.
void msr_extras_boot(void);

// Entry hooks: called by the generated code on entry to these game functions, with the arguments
// in c->r as the caller left them (tools/gen/generate.py ENTRY_HOOKS).
void msr_hook_state_change(Cpu* c);   // 0x4E40A0, the file select's state change
void msr_hook_text_copy(Cpu* c);      // 0x1913C0, a text copy (the questions after a difficulty)
void msr_hook_music_fade(Cpu* c);     // 0x4C1570, the title music's fade (a Yes that starts the game)
void msr_hook_heap_setup(Cpu* c);     // 0x51BB14, a heap set-up (this, start, end)
