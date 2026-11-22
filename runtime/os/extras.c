// MSR-nx: the extras' run-time half (extras.h). The facts behind every address here, and the hook
// logic, come from gminustwo/msr2ds (src/msr/cheats.h, native/runtime/msr/patches.cpp,
// GPL-2.0-or-later), read as reference and written again for this port. tools/gen/generate.py
// checks the code words around each hooked function when it translates the game, so a different
// game version stops the build instead of being patched blindly.
#include "extras.h"
#include "msr_log.h"
#include "msr_mem.h"

volatile u32 msr_extras;

static int mapped(u32 a) {
    if (a < 0x00100000u) return 0;
    return msr_mem_query(a).state != MSR_STATE_FREE;
}

// ---- every difficulty for every file --------------------------------------------------------------
// The game offers its difficulty list (Normal, Hard, Fusion) only for a used slot's New Game, once
// that save has beaten the game. The patches (generate.py GATED, MSR_EXTRA_DIFFICULTY) offer it for
// every save and send a new file's Play to the list too. The list was made for a used slot, so three
// hooks on the file select adapt it to an empty one (its summary's +0x54, "has a game", is 0):
// - The state change (this, the new state, _, at once): the new-file panel (0xE) fades the whole
//   menu out on its way out, so the change to the list (0x2A) leaves from the state a used slot's
//   New Game leaves from ("Start a new game?", 0x28). The list's Back goes to a used slot's menu
//   (0x12): for an empty one, back to the files (0xD).
// - The questions after a difficulty say "Current data will be erased...": for an empty slot the
//   text they copy becomes the used slot's "Start a new game?".
// - Yes then starts the game as a new file's Play does: the new-file flag (+0xF9C) set and the
//   title music's fade called without Play's last argument.
#define MENU_STATE 0x138u
#define MENU_SLOT 0xF50u
#define SLOT_SUMMARY 0x154u
#define SUMMARY_HAS_GAME 0x54u
#define MENU_NEW_FILE 0xF9Cu
#define ST_FILES 0xDu
#define ST_NEW_FILE_PANEL 0xEu
#define ST_SLOT_MENU 0x12u
#define ST_NEW_GAME_QUESTION 0x28u
#define ST_DIFFICULTY_LIST 0x2Au
#define START_NEW_GAME_TEXT 0x707AD4u   // "Start a new game?" (state 0x28's text)

static const u32 k_question_returns[3] = {0x4DF380u, 0x4DDDF0u, 0x4DF2DCu};   // Normal, Hard, Fusion
static const u32 k_yes_fade_returns[3] = {0x4E36FCu, 0x4E3554u, 0x4E3898u};   // their Yes handlers

static int empty_slot(u32 menu) {
    if (!mapped(menu + MENU_SLOT)) return 0;
    u32 slot = rd32(menu + MENU_SLOT);
    if (!mapped(slot + SLOT_SUMMARY)) return 0;
    u32 summary = rd32(slot + SLOT_SUMMARY);
    return mapped(summary + SUMMARY_HAS_GAME) && rd8(summary + SUMMARY_HAS_GAME) == 0;
}

static int returns_to(const u32* list, u32 lr) {
    for (int k = 0; k < 3; k++)
        if (list[k] == lr) return 1;
    return 0;
}

void msr_hook_state_change(Cpu* c) {
    if (!(msr_extras & MSR_EXTRA_DIFFICULTY)) return;
    u32 menu = c->r[0];
    if (c->r[3] != 0 || !mapped(menu + MENU_STATE)) return;
    u32 now = rd32(menu + MENU_STATE);
    if (c->r[1] == ST_DIFFICULTY_LIST && now == ST_NEW_FILE_PANEL) {
        wr32(menu + MENU_STATE, ST_NEW_GAME_QUESTION);
    } else if (c->r[1] == ST_SLOT_MENU && now == ST_DIFFICULTY_LIST && empty_slot(menu)) {
        c->r[1] = ST_FILES;
    }
}

void msr_hook_text_copy(Cpu* c) {
    if (!(msr_extras & MSR_EXTRA_DIFFICULTY)) return;
    if (returns_to(k_question_returns, c->r[14]) && empty_slot(c->r[4])) c->r[1] = rd32(START_NEW_GAME_TEXT);
}

void msr_hook_music_fade(Cpu* c) {
    if (!(msr_extras & MSR_EXTRA_DIFFICULTY)) return;
    if (!returns_to(k_yes_fade_returns, c->r[14]) || !empty_slot(c->r[0])) return;
    wr8(c->r[0] + MENU_NEW_FILE, 1);
    c->r[3] = 1;
}

// ---- the package heap 8 MB larger -------------------------------------------------------------------
// For the Fusion suit changed in play. The game keeps its resource packages in a 20 MB heap: the
// system's, Samus's and her suit family's packages first, then the area's, then its sub-areas, which
// take turns in the space left. A suit change in play loads the other suit family while hers is still
// there, so it lands in the sub-areas' space, and a sub-area loaded again later no longer fits: its
// models never come (msr2ds saw an Alpha Metroid fought unseen). 8 MB more leaves room.
// The game asks for 56 MB of linear memory at boot (the word at 0x704B18, read at 0x108EC8) and
// divides it with the set-up at 0x51BB14 (this, start, end). It asks for 8 MB more here; the hook ends
// the package heap 8 MB later and starts the main heap 8 MB later, so every other heap keeps its size.
#define LINEAR_SIZE 0x704B18u
#define LINEAR_SIZE_GAME 0x03800000u
#define HEAP_EXTRA 0x800000u
#define PACKAGE_HEAP_RETURN 0x51BC14u
#define MAIN_HEAP_RETURN 0x51BC34u
static int g_heap_wide;

void msr_extras_boot(void) {
    if (rd32(LINEAR_SIZE) == LINEAR_SIZE_GAME && rd32(0x108EC8u) == 0xE5940018u && rd32(0x51BC00u) == 0xE2855505u &&
        rd32(0x51BBD0u) == 0xE2407507u) {
        wr32(LINEAR_SIZE, LINEAR_SIZE_GAME + HEAP_EXTRA);
        g_heap_wide = 1;
        msr_receipt("extras: the game's linear memory 8 MB larger (package heap 28 MB, for the Fusion suit)");
    } else {
        msr_receipt("extras: the package heap is left at 20 MB (the game's code is not as expected)");
    }
}

void msr_hook_heap_setup(Cpu* c) {
    if (!g_heap_wide) return;
    if (c->r[14] == PACKAGE_HEAP_RETURN) {
        c->r[2] += HEAP_EXTRA;
        msr_receipt("extras: package heap %08X to %08X (%u MB)", c->r[1], c->r[2], (c->r[2] - c->r[1]) >> 20);
    } else if (c->r[14] == MAIN_HEAP_RETURN) {
        c->r[1] += HEAP_EXTRA;
    }
}
