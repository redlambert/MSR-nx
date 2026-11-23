// MSR-nx: what Samus Returns shows on the bottom screen, worked out from its pixels, for the
// single-screen layout. A C port of gminustwo/msr2ds src/msr/bottom_classifier.h
// (GPL-2.0-or-later), its thresholds and positions unchanged (msr2ds measured them on the USA game).
#pragma once
#include <stdint.h>

enum {
    BC_UNKNOWN,   // nothing classified yet
    BC_HUD,       // the gameplay HUD (its readouts go over the top screen)
    BC_LOGO,      // the idle emblem of cutscenes and loading, or the HUD dimmed for a cutscene
    BC_MENU,      // a framed menu: pause pages, file select, gallery, options, amiibo...
    BC_PROMPT,    // text on the bare backdrop: the title's prompt, the prologue's lines
};

enum {
    BC_PAGE_OTHER,
    BC_PAGE_PAUSE_MAP,
    BC_PAGE_PAUSE_SAMUS,
    BC_PAGE_MAP_PINS,
    BC_PAGE_FILE_LIST,
    BC_PAGE_FILE_OPEN,
    BC_PAGE_GALLERY,   // the gallery viewer: the art on the top screen, its counter on the bottom
};

typedef struct {
    int bottom;            // BC_*
    int page;              // BC_PAGE_*, for BC_MENU
    int title_menu;        // BC_MENU on black (the title's menus), not the pause menu
    int x0, y0, x1, y1;    // BC_PROMPT: the box around its text (x1, y1 exclusive), rows from the top
    int peak;              // BC_PROMPT: the text's brightest pixel, 0 to 255 (the title's prompt pulses)
} BcResult;

// pic: the bottom screen as main_nx.c keeps it, 320x240 RGBA words (red in the low byte), rows
// stored bottom to top.
void bc_classify(const uint32_t* pic, BcResult* out);
