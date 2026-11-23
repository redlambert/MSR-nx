// MSR-nx: the bottom screen's classifier (bottom_class.h). Ported from gminustwo/msr2ds
// src/msr/bottom_classifier.h (GPL-2.0-or-later): the same tests, thresholds and positions, in C,
// reading this port's picture layout. msr2ds's notes on what it measured are kept with each test.
// Left out: its beam slot and map marker readers (this port has its own beam reader, and no minimap).
#include "bottom_class.h"

#define W 320
#define H 240

static const uint32_t* g_pic;

static inline void rgb(int x, int y, int* r, int* g, int* b) {
    uint32_t c = g_pic[(H - 1 - y) * W + x];   // rows stored bottom to top
    *r = (int)(c & 0xFF);
    *g = (int)((c >> 8) & 0xFF);
    *b = (int)((c >> 16) & 0xFF);
}

static inline int brightness(int x, int y) {
    int r, g, b;
    rgb(x, y, &r, &g, &b);
    int m = r > g ? r : g;
    return m > b ? m : b;
}

static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// A HUD rule at row y: the fraction of x where blue in rows y+-2 stands out from the rows 7 above
// and below, and the rule's mean blue level. Contrast finds the rules even when the game dims the
// bottom screen for a cutscene.
typedef struct {
    float coverage;
    float level;
} Rule;

static Rule find_rule(int y, int x0, int x1) {
    int hits = 0;
    float level = 0.f;
    for (int x = x0; x < x1; x++) {
        int line = 0;
        for (int dy = -2; dy <= 2; dy++) {
            int r, g, b;
            rgb(x, clampi(y + dy, 0, H - 1), &r, &g, &b);
            if (b > line) line = b;
        }
        int r, g, above, below;
        rgb(x, clampi(y - 7, 0, H - 1), &r, &g, &above);
        rgb(x, clampi(y + 7, 0, H - 1), &r, &g, &below);
        int low = above < below ? above : below;
        hits += line - low > 35;
        level += (float)line;
    }
    float count = (float)(x1 - x0 > 1 ? x1 - x0 : 1);
    Rule rule = {(float)hits / count, level / count};
    return rule;
}

// A cyan pixel: blue at least min_blue and min_tint above red, green above red.
static inline int cyan(int x, int y, int min_blue, int min_tint) {
    int r, g, b;
    rgb(x, y, &r, &g, &b);
    return b >= min_blue && b > r + min_tint && g > r;
}

// Longest horizontal run of bright cyan pixels in rows [y0, y1). Framed menus have long rules;
// the title, prologue and emblem have none. A lower min_blue also finds unlit frames.
typedef struct {
    int length, y, x0, x1;
} Run;

static Run longest_rule(int y0, int y1, int min_blue, int min_tint) {
    Run best = {0, 0, 0, 0};
    for (int y = y0; y < y1; y++) {
        int run = 0;
        for (int x = 0; x < W; x++) {
            run = cyan(x, y, min_blue, min_tint) ? run + 1 : 0;
            if (run > best.length) {
                best.length = run;
                best.y = y;
                best.x0 = x + 1 - run;
                best.x1 = x + 1;
            }
        }
    }
    return best;
}

// Longest vertical run of bright cyan pixels in columns [x0, x1), rows [y0, y1).
static int longest_column(int x0, int x1, int y0, int y1) {
    int best = 0;
    for (int x = x0; x < x1; x++) {
        int run = 0;
        for (int y = y0; y < y1; y++) {
            run = cyan(x, y, 150, 30) ? run + 1 : 0;
            if (run > best) best = run;
        }
    }
    return best;
}

// Fraction of pixels brighter (or darker) than threshold, sampled every other pixel.
static float bright_fraction(int threshold) {
    int hits = 0, total = 0;
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 2) {
            hits += brightness(x, y) > threshold;
            total++;
        }
    return (float)hits / (float)total;
}

static float dark_fraction(int threshold) {
    int hits = 0, total = 0;
    for (int y = 0; y < H; y += 2)
        for (int x = 0; x < W; x += 2) {
            hits += brightness(x, y) < threshold;
            total++;
        }
    return (float)hits / (float)total;
}

// The box around the pixels brighter than threshold (a prompt's text); x1 <= x0 when there are none.
static void bright_box(int threshold, int* x0, int* y0, int* x1, int* y1) {
    int bx0 = W, by0 = H, bx1 = 0, by1 = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (brightness(x, y) > threshold) {
                if (x < bx0) bx0 = x;
                if (y < by0) by0 = y;
                if (x + 1 > bx1) bx1 = x + 1;
                if (y + 1 > by1) by1 = y + 1;
            }
    if (bx1 <= bx0) bx0 = by0 = bx1 = by1 = 0;
    *x0 = bx0;
    *y0 = by0;
    *x1 = bx1;
    *y1 = by1;
}

// Title menus. The gallery viewer frames its page counter with a cyan edge down each side (x 35
// and 284). The file select's first file box starts at y ~33; in the list the box closes at y ~85,
// an opened file's box runs on around its buttons. Unlit boxes are dimmer.
static int title_menu_page(void) {
    if (longest_column(30, 42, 50, 190) >= 100 && longest_column(278, 290, 50, 190) >= 100) return BC_PAGE_GALLERY;
    Run box_top = longest_rule(28, 39, 70, 15);
    if (box_top.length >= 240 && box_top.x0 <= 55) {
        Run box_bottom = longest_rule(80, 90, 70, 15);
        return box_bottom.length >= 240 && box_bottom.x0 <= 45 ? BC_PAGE_FILE_LIST : BC_PAGE_FILE_OPEN;
    }
    return BC_PAGE_OTHER;
}

void bc_classify(const uint32_t* pic, BcResult* out) {
    g_pic = pic;
    out->bottom = BC_LOGO;
    out->page = BC_PAGE_OTHER;
    out->title_menu = 0;
    out->x0 = out->y0 = out->x1 = out->y1 = 0;
    // Gameplay HUD: rules under the energy bar and above the counters. Bright when playing, dimmed
    // by the game during cutscenes, when it stays hidden. A menu's title rule (y ~24, 100 px or
    // more; the HUD has 23 at most there) rules it out: a well explored pause map draws its outlines
    // along the HUD's rule.
    Rule top = find_rule(40, 85, 220);
    Rule bottom = find_rule(215, 65, 230);
    Run title = longest_rule(18, 31, 150, 30);
    if (top.coverage > .8f && bottom.coverage > .8f && title.length < 100) {
        out->bottom = top.level > 135.f && bottom.level > 135.f ? BC_HUD : BC_LOGO;
        return;
    }
    // Framed menus have a long rule. The title menus stand on black, the pause menu on its
    // backdrop; its pages are told apart by the rule under their title (y ~24): map page x 61-259,
    // Samus page x 61-320, map pins x 101-219.
    if (longest_rule(0, H, 150, 30).length >= 100) {
        if (dark_fraction(6) > .3f) {
            out->bottom = BC_MENU;
            out->title_menu = 1;
            out->page = title_menu_page();
            return;
        }
        // The pause menu's pages and boxes stand over its bottom bar, a rule in the last rows
        // (unlit 132-273); a picture with a cyan edge does not, such as the ship's cockpit the
        // bottom screen shows after a file's Continue (the windscreen's edge, 123-135 px; 88 at
        // most there). Hidden, as the emblem.
        if (longest_rule(195, H, 70, 15).length < 110) return;   // BC_LOGO
        out->bottom = BC_MENU;
        if (title.length >= 60) {
            if (title.x1 >= 310 && title.x0 < 90) out->page = BC_PAGE_PAUSE_SAMUS;
            else if (title.x0 < 80 && title.x1 < 280 && bottom.coverage > .8f) out->page = BC_PAGE_PAUSE_MAP;
            else if (title.x0 >= 90 && title.x1 <= 235 && bottom.coverage > .8f) out->page = BC_PAGE_MAP_PINS;
        }
        return;
    }
    if (bright_fraction(140) > 0.001f) {
        out->bottom = BC_PROMPT;
        // msr2ds compositor.cpp: the text's box, made even about the screen's middle
        int x0, y0, x1, y1;
        bright_box(140, &x0, &y0, &x1, &y1);
        if (x1 > x0) {
            int m = x0 < W - x1 ? x0 : W - x1;
            out->x0 = m;
            out->y0 = y0;
            out->x1 = W - m;
            out->y1 = y1;
        }
        return;
    }
    // BC_LOGO
}
