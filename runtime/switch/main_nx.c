// MSR-nx: the Switch platform layer. Boots the game's translated code on its own threads, shows
// the two 3DS screens with deko3d, plays the DSP mix through audout and feeds the Joy-Cons to hid.
//
// Bring-up version: the 3DS GPU work is drawn by the reference renderer on the CPU (correct but
// slow); the deko3d PICA renderer replaces it next. Files live in sdmc:/switch/msr/.
#include <deko3d.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "boot.h"
#define MSR_VERSION "0.8.4 test (shared descriptors, trace, A/B)"
#include "frame_plan.h"
#include "audio_stretch.h"
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"
#include "sched.h"
#include "port_menu.h"
#include "bottom_class.h"
#include "dk_shaders.h"   // generated (tools/switch/mkshaders.py): hud_vsh, hud_fsh

#include <dirent.h>
#include <unistd.h>
typedef DIR DirHandle;   // DIR is the folder below from here on
#define DIR "sdmc:/switch/msr"

void msr_run(Cpu* c, u32 entry, u32 stop);
const char* msr_trace_ring[64];
unsigned msr_trace_pos;
void msr_dump_trace(void) {}

extern int msr_full_log;
extern int sched_realtime;
extern u64 msr_time_limit_ns;
extern unsigned msr_svc_limit;
extern char msr_save_dir[512];
extern u32 msr_buttons;
extern s16 msr_circle_x, msr_circle_y;
extern int msr_touch_down;
extern u16 msr_touch_x, msr_touch_y;
extern s16 msr_audio_ring[];
extern volatile u32 msr_audio_write;
typedef struct { u32 left, right, stride, format; unsigned swaps; } MsrFramebuffer;
extern MsrFramebuffer msr_fb[2];

// ---- the game ------------------------------------------------------------------------------------

static Cpu* g_main_cpu;
// Moves the calling thread to one CPU core: the game's threads stay on core 0, the graphics
// thread (os/hle_hid_gsp.c) goes to core 1, the screens, input and sound to core 2.
void msr_thread_to_core(int core) {
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (R_FAILED(rc)) msr_receipt("core: could not move a thread to core %d (%08X)", core, rc);
}

static void* game_main(void* arg) {
    (void)arg;
    void sched_enter_main(void);
    sched_enter_main();
    msr_run(g_main_cpu, MSR_CODE_BASE, 0xFFFFFFF0u);
    msr_receipt("the game's entry function returned");
    msr_log_close();
    exit(0);
    return 0;
}

// ---- display ---------------------------------------------------------------------------------------

#define FB_W 1280
#define FB_H 720
static DkDevice g_dev;
static DkQueue g_queue;
static DkSwapchain g_swap;
static DkMemBlock g_fb_mem, g_cmd_mem, g_tex_mem, g_up_mem, g_panel_mem, g_fps_mem;
static DkImage g_fb[2], g_scr[2], g_panel_img, g_fps_img;
// single-screen HUD: its shaders, a uniform block, and the bottom screen's descriptors
static DkMemBlock g_hud_code, g_hud_data;
static DkShader g_hud_vsh, g_hud_fsh;
static DkCmdBuf g_cmd;
static u32* g_upload;   // CPU-visible staging: top 400x240 then bottom 320x240, RGBA8
static Mutex g_qlock;   // the game's renderer and the presenter share the queue
void dkr_init(DkDevice dev, DkQueue q, Mutex* qlock);

static void display_init(void) {
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginLowerLeft;   // the PICA's conventions
    g_dev = dkDeviceCreate(&dm);

    DkImageLayoutMaker lm;
    DkImageLayout fbl;
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = FB_W;
    lm.dimensions[1] = FB_H;
    dkImageLayoutInitialize(&fbl, &lm);
    u32 fsz = (u32)dkImageLayoutGetSize(&fbl), falign = dkImageLayoutGetAlignment(&fbl);
    fsz = (fsz + falign - 1) & ~(falign - 1);
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, g_dev, (2 * fsz + 0xFFFF) & ~0xFFFFu);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    g_fb_mem = dkMemBlockCreate(&mm);
    DkImage const* list[2];
    for (int k = 0; k < 2; k++) { dkImageInitialize(&g_fb[k], &fbl, g_fb_mem, k * fsz); list[k] = &g_fb[k]; }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, g_dev, nwindowGetDefault(), list, 2);
    g_swap = dkSwapchainCreate(&sm);

    // the two screen images, sources for the scaled blits
    DkImageLayout sl[2];
    u32 off = 0, offs[2];
    for (int k = 0; k < 2; k++) {
        dkImageLayoutMakerDefaults(&lm, g_dev);
        lm.flags = DkImageFlags_Usage2DEngine;
        lm.format = DkImageFormat_RGBA8_Unorm;
        lm.dimensions[0] = k ? 320 : 400;
        lm.dimensions[1] = 240;
        dkImageLayoutInitialize(&sl[k], &lm);
        u32 a = dkImageLayoutGetAlignment(&sl[k]);
        off = (off + a - 1) & ~(a - 1);
        offs[k] = off;
        off += (u32)dkImageLayoutGetSize(&sl[k]);
    }
    dkMemBlockMakerDefaults(&mm, g_dev, (off + 0xFFFF) & ~0xFFFFu);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    g_tex_mem = dkMemBlockCreate(&mm);
    for (int k = 0; k < 2; k++) dkImageInitialize(&g_scr[k], &sl[k], g_tex_mem, offs[k]);

    dkMemBlockMakerDefaults(&mm, g_dev, 0x80000);   // (400 + 320) * 240 * 4 = 691200 > 0x80000? use 1 MiB
    mm.size = 0x100000;
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_up_mem = dkMemBlockCreate(&mm);
    g_upload = (u32*)dkMemBlockGetCpuAddr(g_up_mem);

    // the port menu's panel: CPU-drawn pixels (staging, then an image for the blit)
    {
        DkImageLayout pl;
        dkImageLayoutMakerDefaults(&lm, g_dev);
        lm.flags = DkImageFlags_Usage2DEngine;
        lm.format = DkImageFormat_RGBA8_Unorm;
        lm.dimensions[0] = PANEL_W;
        lm.dimensions[1] = PANEL_H;
        dkImageLayoutInitialize(&pl, &lm);
        u32 psz = (u32)dkImageLayoutGetSize(&pl), pal = dkImageLayoutGetAlignment(&pl);
        u32 img_off = (PANEL_W * PANEL_H * 4 + pal - 1) & ~(pal - 1);
        dkMemBlockMakerDefaults(&mm, g_dev, (img_off + psz + 0xFFFF) & ~0xFFFFu);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
        g_panel_mem = dkMemBlockCreate(&mm);
        dkImageInitialize(&g_panel_img, &pl, g_panel_mem, img_off);
        // and the frame counter's
        dkImageLayoutMakerDefaults(&lm, g_dev);
        lm.flags = DkImageFlags_Usage2DEngine;
        lm.format = DkImageFormat_RGBA8_Unorm;
        lm.dimensions[0] = FPS_W;
        lm.dimensions[1] = FPS_H;
        dkImageLayoutInitialize(&pl, &lm);
        psz = (u32)dkImageLayoutGetSize(&pl);
        pal = dkImageLayoutGetAlignment(&pl);
        img_off = (FPS_W * FPS_H * 4 + pal - 1) & ~(pal - 1);
        dkMemBlockMakerDefaults(&mm, g_dev, (img_off + psz + 0xFFFF) & ~0xFFFFu);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
        g_fps_mem = dkMemBlockCreate(&mm);
        dkImageInitialize(&g_fps_img, &pl, g_fps_mem, img_off);
    }

    {   // the single-screen HUD's shaders and data
        u32 vsz = (sizeof hud_vsh_dksh + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
        dkMemBlockMakerDefaults(&mm, g_dev, (vsz + sizeof hud_fsh_dksh + DK_SHADER_CODE_UNUSABLE_SIZE + 0xFFFF) & ~0xFFFFu);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        g_hud_code = dkMemBlockCreate(&mm);
        u8* code = (u8*)dkMemBlockGetCpuAddr(g_hud_code);
        memcpy(code, hud_vsh_dksh, sizeof hud_vsh_dksh);
        memcpy(code + vsz, hud_fsh_dksh, sizeof hud_fsh_dksh);
        DkShaderMaker sm2;
        dkShaderMakerDefaults(&sm2, g_hud_code, 0);
        dkShaderInitialize(&g_hud_vsh, &sm2);
        dkShaderMakerDefaults(&sm2, g_hud_code, vsz);
        dkShaderInitialize(&g_hud_fsh, &sm2);
        // 0x000 uniforms (48 bytes), 0x100 image descriptor, 0x200 sampler descriptors (nearest, linear)
        dkMemBlockMakerDefaults(&mm, g_dev, 0x1000);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        g_hud_data = dkMemBlockCreate(&mm);
        u8* d = (u8*)dkMemBlockGetCpuAddr(g_hud_data);
        DkImageView iv;
        dkImageViewDefaults(&iv, &g_scr[1]);
        dkImageDescriptorInitialize((DkImageDescriptor*)(d + 0x100), &iv, false, false);
        DkSampler smp;
        dkSamplerDefaults(&smp);   // nearest: the readouts stay as sharp as the game drew them
        dkSamplerDescriptorInitialize((DkSamplerDescriptor*)(d + 0x200), &smp);
        dkSamplerDefaults(&smp);   // linear: the subtitles at 1.5x
        smp.minFilter = DkFilter_Linear;
        smp.magFilter = DkFilter_Linear;
        dkSamplerDescriptorInitialize((DkSamplerDescriptor*)(d + 0x200 + sizeof(DkSamplerDescriptor)), &smp);
    }

    dkMemBlockMakerDefaults(&mm, g_dev, 0x10000);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_cmd_mem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    g_cmd = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(g_cmd, g_cmd_mem, 0, 0x10000);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute;   // the VRAM kernels and the screen conversion are compute work
    // scratch memory for the interpreter vertex shader's spills (43520 bytes per warp); the translated
    // shaders need none, but a configuration missing from the shader pack falls back to it
    qm.perWarpScratchMemorySize = 0xE000;   // the interpreter vertex shader spills 57344 bytes per warp (check the .dksh when it changes)
    g_queue = dkQueueCreate(&qm);
}

// ---- single-screen mode: what goes on the Switch's screen ----------------------------------------------
// The bottom screen is classified from its pixels each new picture (bottom_class.c, msr2ds's
// classifier), and what it shows decides the view. Until 0.7.0 only the file select and the pause
// menu frame were recognized (two pixel checks), so the gallery viewer, the amiibo screen and the
// prologue's lines were never shown.
enum { SS_TOP, SS_HUD, SS_MENU, SS_GALLERY, SS_TEXT };
static int g_ss_view = SS_TOP;
// SS_TEXT, the subtitles, after msr2ds's compositor: the text keyed out of the bottom screen (its
// backdrop fully see-through) over a dark card. The card follows the text's box, but grows at once
// and shrinks only after a second without growing, and what is drawn eases to it, so lines coming
// and the cursor blinking after them do not move or resize anything. The placement is fixed for a
// size: a short prompt (the title's) at 2x with the bottom screen's middle at 80% of the screen's
// height, below the logo; taller text (the prologue's) at 1.5x and lower, held for 5 s after the
// text was last tall.
static float g_ss_card[4];                     // the card's text area, bottom-screen pixels (x0, y0, x1, y1)
static float g_ss_shown[4];                    // ...as drawn, easing to it
static int g_ss_card_set;
static unsigned long long g_ss_card_ns, g_ss_tall_ns;
static float g_ss_gain = 1.0f;                 // the text's brightness gain (the title's prompt pulses)
static int g_ss_text_dst[4];                   // where the text is drawn on the Switch's screen (x, y, w, h)
static float g_ss_text_src[4];                 // ...and the bottom-screen area it shows (x, y, w, h)
static unsigned long long g_start_pressed_ns;  // the 3DS Start button's last press (input, below)
static const char* const k_ss_names[] = {
    "top screen only",
    "top screen with the HUD readouts",
    "the bottom screen's menu",
    "gallery: the picture with its counter beside it",
    "top screen with the bottom screen's text as subtitles",
};
static const char* const k_bc_names[] = {"nothing yet", "HUD", "emblem or dimmed HUD", "menu", "text"};
static const char* const k_page_names[] = {"", ", pause map", ", pause Samus page", ", map pins", ", file list", ", file opened", ", gallery viewer"};

// ---- the minimap (single-screen play) ----------------------------------------------------------------
// After msr2ds native/runtime/msr/minimap.cpp: Samus's marker where the game's own minimap draws it, read
// from the game's interface objects (the colour of the picture alone mistook a Missile Reload Station's
// icon for her, in msr2ds). The game object (0x76F314) holds a handle at +0x17FC to the HUD, which holds
// one at +0x550 to the minimap (vtable 0x6E6120), which holds one at +0x280 to Samus's icon (vtable
// 0x6E5FF8); a handle's object is at its +8. The icon keeps where it is drawn: top left at +0x10, size at
// +0x1C, fractions of the 320x240 screen. The minimap's +0x22A or +0x22B set: it shows a place of the
// game's choosing (the DNA scanner's way on), not Samus's.
static int map_valid(u32 a) {
    return a >= 0x00100000u && msr_mem_query(a).state != MSR_STATE_FREE;
}
static u32 map_held(u32 owner, u32 offset) {
    if (!owner || !map_valid(owner + offset + 4)) return 0;
    u32 h = rd32(owner + offset);
    return h && map_valid(h + 12) ? rd32(h + 8) : 0;
}
static float map_rf(u32 a) {
    u32 w = rd32(a);
    float f;
    memcpy(&f, &w, 4);
    return f;
}
// 1 and Samus's marker (bottom-screen pixels) when the game's minimap has one; *focus set while it shows
// a place of its own.
static int map_marker(float* x, float* y, int* focus) {
    static int supported = -1;
    if (supported < 0) {
        supported = rd32(0x1A4D30) == 0xE2800B05u && rd32(0x1A4D34) == 0xE2800FFFu && rd32(0x1AE62C) == 0xE5900550u &&
                    rd32(0x507E24) == 0xE5940280u;
        if (!supported) msr_receipt("minimap: the game's code is not as expected; the minimap stays on the map's middle");
    }
    *focus = 0;
    if (!supported) return 0;
    u32 minimap = map_held(map_held(rd32(0x76F314u), 0x17FC), 0x550);
    if (!minimap || !map_valid(minimap + 0x290) || rd32(minimap) != 0x6E6120u) return 0;
    *focus = rd8(minimap + 0x22A) || rd8(minimap + 0x22B);
    u32 icon = map_held(minimap, 0x280);
    if (!icon || !map_valid(icon + 0x60) || rd32(icon) != 0x6E5FF8u) return 0;
    float l = map_rf(icon + 0x10), t = map_rf(icon + 0x14), w = map_rf(icon + 0x1C), h = map_rf(icon + 0x20);
    if (!(l > -4.0f && l < 4.0f) || !(t > -4.0f && t < 4.0f) || !(w > 0.0f && w < 1.0f) || !(h > 0.0f && h < 1.0f)) return 0;
    *x = (l + w * 0.5f) * 320.0f;
    *y = (t + h * 0.5f) * 240.0f;
    return 1;
}

static void single_screen_update(const u32* pic) {
    BcResult r;
    bc_classify(pic, &r);
    int want = SS_TOP;
    if (r.bottom == BC_HUD) want = SS_HUD;
    else if (r.bottom == BC_PROMPT) want = SS_TEXT;
    else if (r.bottom == BC_MENU) want = r.page == BC_PAGE_GALLERY ? SS_GALLERY : SS_MENU;
    unsigned long long now = msr_now_ns();
    if (want == SS_TEXT && r.x1 > r.x0) {
        float t[4] = {(float)r.x0, (float)r.y0, (float)r.x1, (float)r.y1};
        float* c = g_ss_card;
        int beyond_card = t[0] < c[0] - 1 || t[1] < c[1] - 1 || t[2] > c[2] + 1 || t[3] > c[3] + 1;
        int card_beyond = c[0] < t[0] - 1 || c[1] < t[1] - 1 || c[2] > t[2] + 1 || c[3] > t[3] + 1;
        if (!g_ss_card_set || t[1] < c[1] - 4 || t[1] > c[1] + 4) {   // new text
            memcpy(c, t, sizeof t);
            if (!g_ss_card_set) memcpy(g_ss_shown, t, sizeof t);
            g_ss_card_set = 1;
            g_ss_card_ns = now;
        } else if (beyond_card) {   // grows at once
            if (t[0] < c[0]) c[0] = t[0];
            if (t[1] < c[1]) c[1] = t[1];
            if (t[2] > c[2]) c[2] = t[2];
            if (t[3] > c[3]) c[3] = t[3];
            g_ss_card_ns = now;
        } else if (!card_beyond || now - g_ss_card_ns > 1000000000ull) {   // shrinks after a second
            memcpy(c, t, sizeof t);
            g_ss_card_ns = now;
        }
        if (c[3] - c[1] > 26.0f) g_ss_tall_ns = now;
        float peak = r.peak > 40 ? r.peak / 255.0f : 40 / 255.0f;
        g_ss_gain = 0.88f / peak;
        if (g_ss_gain < 1.0f) g_ss_gain = 1.0f;
        if (g_ss_gain > 12.0f) g_ss_gain = 12.0f;
    }
    static int cand = -1, cand_n, in_play, menu_asked;
    static unsigned long long cand_since, hud_seen;
    if (want == SS_GALLERY && in_play) want = SS_MENU;   // the gallery is a title screen: in play, a misread
    if (want == SS_HUD) {
        hud_seen = now;
        in_play = 1;
        menu_asked = 0;
    }
    if (r.bottom == BC_MENU && r.title_menu) in_play = 0;
    if (want == cand) {
        cand_n++;
    } else {
        cand = want;
        cand_n = 1;
        cand_since = now;
    }
    if (want == g_ss_view) return;
    // a few pictures in a row decide (menus sliding in or out read as other things for a moment);
    // leaving a menu takes a few more
    // (on the title screens, where the menus sliding about read as the emblem more often, 15)
    int need = (g_ss_view == SS_MENU || g_ss_view == SS_GALLERY) ? (in_play ? 6 : 15) : 3;
    int held = cand_n >= need && now - cand_since >= 80000000ull;
    // A menu over play that no Start press asked for (msr2ds compositor.cpp, its #12 and #32): the
    // HUD misread while picking things up, or one of the game's own screens over play (an elevator's
    // or a Teleport Station's map, a new ability's). Shown once it has held 1.5 s, or 0.3 s when the
    // HUD has been gone for a second (after the game's own messages).
    if (held && want == SS_MENU && in_play && !menu_asked) {
        if (g_start_pressed_ns && now - g_start_pressed_ns < 2000000000ull) menu_asked = 1;
        else if (now - cand_since < (now - hud_seen >= 1000000000ull ? 300000000ull : 1500000000ull)) held = 0;
    }
    if (!held) return;
    if (want != SS_TEXT) g_ss_card_set = 0;
    g_ss_view = want;
    g_map_view = want == SS_MENU;
    g_gallery_view = want == SS_GALLERY;
    static unsigned logged;
    if (logged++ < 400)
        msr_receipt("single screen: %s (bottom screen: %s%s)", k_ss_names[want], k_bc_names[r.bottom],
                    r.bottom == BC_MENU ? k_page_names[r.page] : "");
}

// The beam column on the right of the bottom screen: three slots (power beam, then the beams the
// game adds). The selected one is lit (bright pixels, every second one counted); the others are dim. -1: none lit.
static const int k_beam_y[3] = {8, 84, 160};   // slot tops, 70 rows each, x 250 to 319
static int selected_beam_slot(void) {
    const u32* b = g_upload + 400 * 240;
    int best = -1, best_n = 150;   // lit: 218 to 381 in runs 10 to 18, menus 47 to 123
    for (int k = 0; k < 3; k++) {
        int n = 0;
        for (int y = k_beam_y[k]; y < k_beam_y[k] + 68; y += 2)
            for (int x = 252; x < 318; x += 2) {
                u32 c = b[(239 - y) * 320 + x];
                int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
                if (r > 180 || g > 180 || bl > 180) n++;
            }
        if (n > best_n) { best_n = n; best = k; }
    }
    return best;
}

// An LCD framebuffer is 240 pixels per column, columns left to right, each column bottom to top.
static void convert_screen(int screen, u32* out, int flip) {
    MsrFramebuffer* f = &msr_fb[screen];
    u32 W = screen ? 320 : 400, H = 240, fmt = f->format & 7;
    if (!f->left) { memset(out, 0, W * H * 4); return; }
    u32 bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    u32 stride = f->stride ? f->stride : 240 * bpp;
    const u8* base = g_mem + f->left;
    for (u32 x = 0; x < W; x++) {
        const u8* col = base + x * stride;
        for (u32 y = 0; y < H; y++) {
            const u8* p = col + (H - 1 - y) * bpp;
            u32 r, g, b;
            u16 h = (u16)(p[0] | (bpp > 1 ? p[1] << 8 : 0));
            switch (fmt) {
            case 0: r = p[3]; g = p[2]; b = p[1]; break;
            case 1: r = p[2]; g = p[1]; b = p[0]; break;
            case 3: r = ((h >> 11) & 31) << 3; g = ((h >> 5) & 63) << 2; b = (h & 31) << 3; break;
            case 2: r = ((h >> 11) & 31) << 3; g = ((h >> 6) & 31) << 3; b = ((h >> 1) & 31) << 3; break;
            default: r = (h >> 12) << 4; g = ((h >> 8) & 15) << 4; b = ((h >> 4) & 15) << 4; break;
            }
            // Rows go in bottom to top: the device uses OriginLowerLeft (the PICA's convention), so
            // deko3d tells the compositor to flip the whole window vertically (HAL_TRANSFORM_FLIP_V,
            // deko3d source/dk_swapchain.cpp). Our 2D-engine blits copy rows in memory order, so
            // the picture has to be stored upside down to come out the right way up.
            out[(flip ? H - 1 - y : y) * W + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }
}

void dkr_frame_end(void);
static int g_gpu_render;
static int g_debug;   // sdmc:/switch/msr/debug exists: a test run
static int g_shot_request, g_shots_left = 6;   // pictures of both screens when the pause view first comes up
static unsigned g_hires_shown, g_native_shown;
// A picture of the whole Switch output (scaler, HUD and all) as tNNN_screen.png: with each automatic
// pair of pictures, and on both sticks pressed in together.
static int g_out_shot;
static char g_out_shot_tag[48];
static char g_run_dir[128];
static DkMemBlock g_out_mem;   // top-screen pictures shown from the scaler's picture, and not
static unsigned g_shown_total;   // new top-screen pictures put on the Switch's screen (the speed report)
static void present(void) {
    if (g_gpu_render) dkr_frame_end();
    // only convert the screens when the game showed a new picture on one of them; the menu (open,
    // or just closed) and a layout change redraw the output from the last pictures
    static unsigned seen[2] = {~0u, ~0u};
    static int last_layout = -1, menu_was_open;
    int fresh = !(msr_fb[0].swaps == seen[0] && msr_fb[1].swaps == seen[1]);
    int redraw = g_menu_open || menu_was_open || last_layout != g_set.layout;
    if (!fresh && !redraw) return;
    menu_was_open = g_menu_open;
    last_layout = g_set.layout;
    // the fps meter counts new top screen pictures put on the Switch's screen here, not the
    // game's own pictures: when the GPU falls behind, the game can make 32 a second while far
    // fewer reach the screen (receipt: 162 game pictures and 70 shown in 5 s)
    static unsigned shown_n;
    static unsigned long long shown_ns;
    if (msr_fb[0].swaps != seen[0]) { shown_n++; shown_ns = msr_now_ns(); g_shown_total++; }
    seen[0] = msr_fb[0].swaps; seen[1] = msr_fb[1].swaps;
    MSR_PERF_BEGIN(PERF_PRESENT);
    // the GPU renderer converts the screens on the GPU from its copy of VRAM; otherwise (or for a
    // framebuffer outside VRAM) the CPU does it
    DkGpuAddr up = dkMemBlockGetGpuAddr(g_up_mem);
    int done0 = 0, done1 = 0;
    if (fresh && g_gpu_render) {
        int dkr_record_screen(DkCmdBuf cmd, int screen, u32 addr, u32 stride, u32 format, DkGpuAddr out, u32 out_word);
        done0 = dkr_record_screen(g_cmd, 0, msr_fb[0].left, msr_fb[0].stride, msr_fb[0].format, up, 0);
        done1 = dkr_record_screen(g_cmd, 1, msr_fb[1].left, msr_fb[1].stride, msr_fb[1].format, up, 400 * 240);
    }
    if (fresh && !done0) convert_screen(0, g_upload, 1);
    if (fresh && !done1) convert_screen(1, g_upload + 400 * 240, 1);
    mutexLock(&g_qlock);
    int slot = dkQueueAcquireImage(g_queue, g_swap);
    mutexUnlock(&g_qlock);
    DkImageView fbv, sv[2];
    dkImageViewDefaults(&fbv, &g_fb[slot]);
    dkCmdBufBindRenderTarget(g_cmd, &fbv, NULL);
    // the whole output: the scissor is GPU state the game's draws leave behind (test 11's clear
    // missed the corners, and the closed menu stayed there)
    DkScissor fsc = {0, 0, FB_W, FB_H};
    dkCmdBufSetScissors(g_cmd, 0, &fsc, 1);
    dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    for (int k = 0; k < 2; k++) {
        dkImageViewDefaults(&sv[k], &g_scr[k]);
        if (!fresh) continue;   // the images still hold the last pictures
        u32 W = k ? 320 : 400;
        DkCopyBuf src = {up + (k ? 400 * 240 * 4 : 0), 0, 0};
        DkImageRect r = {0, 0, 0, W, 240, 1};
        dkCmdBufCopyBufferToImage(g_cmd, &src, &sv[k], &r, 0);
    }
    // single-screen mode: the view follows what the bottom screen shows (single_screen_update)
    if (fresh && g_set.layout == LAYOUT_TOP_ONLY) {
        int before = g_ss_view;
        single_screen_update(g_upload + 400 * 240);
        if (g_ss_view != before && g_ss_view == SS_MENU && g_debug && g_shots_left > 0) {   // pictures of both screens
            g_shots_left--;
            g_shot_request = 1;
        }
    }
    if (g_set.layout != LAYOUT_TOP_ONLY && g_ss_view != SS_TOP) {
        g_ss_view = SS_TOP;
        g_map_view = 0;
        g_gallery_view = 0;
    }
    // the screens where the port menu's layout puts them
    Rect tr, br;
    port_menu_layout(&tr, &br);
    // (the output image is shown upside down under OriginLowerLeft: its row 0 is the bottom of the TV)
    DkImageRect ts = {0, 0, 0, 400, 240, 1}, td = {(u32)tr.x, (u32)(FB_H - tr.y - tr.h), 0, (u32)tr.w, (u32)tr.h, 1};
    DkImageRect bs = {0, 0, 0, 320, 240, 1}, bd = {(u32)br.x, (u32)(FB_H - br.y - br.h), 0, (u32)br.w, (u32)br.h, 1};
    // the resolution scaler: the top screen from the large picture made from the game's render
    // target (dk_pica.c k_hires), while its framebuffer still holds what that picture was made for
    DkImage* hi = 0;
    u32 hw = 0, hh = 0;
    if (tr.w && g_gpu_render) {
        int dkr_hires_top(u32 addr, DkImage** img, u32* w, u32* h);
        if (dkr_hires_top(msr_fb[0].left, &hi, &hw, &hh)) g_hires_shown++; else g_native_shown++;
    }
    if (tr.w && hi) {
        DkImageView hv;
        dkImageViewDefaults(&hv, hi);
        DkImageRect hs = {0, 0, 0, hw, hh, 1};
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, DkInvalidateFlags_Image);   // the renderer's kernel wrote it
        dkCmdBufBlitImage(g_cmd, &hv, &hs, &fbv, &td, DkBlitFlag_FilterLinear, 0);
    } else if (tr.w) dkCmdBufBlitImage(g_cmd, &sv[0], &ts, &fbv, &td, DkBlitFlag_FilterLinear, 0);
    if (br.w) dkCmdBufBlitImage(g_cmd, &sv[1], &bs, &fbv, &bd, DkBlitFlag_FilterLinear, 0);
    if (g_set.layout == LAYOUT_TOP_ONLY && (g_ss_view == SS_HUD || (g_ss_view == SS_TEXT && g_ss_card_set))) {
        // single-screen mode: parts of the bottom screen drawn over the top one. With the HUD, its
        // readouts cut out and drawn at 2x (energy, missiles, Aeion gauge and the ability cross; the
        // selected ability; the Metroid DNA and Metroids left); with text on the bare backdrop (the
        // prologue's lines, the title's prompt), the subtitles (g_ss_card).
        // Bottom-screen coordinates, rows counted from the top; sw, sh the size drawn; look how the
        // fragment shader draws it (hud_fsh.glsl).
        typedef struct { float sx, sy, w, h, dx, dy, sw, sh; float look[4]; } Piece;
        static const Piece k_hud[3] = {
            {0, 0, 160, 60, 48, 8, 320, 120, {0, 0, 0, 0}},                       // top left
            {60, 212, 185, 28, 48, 720 - 8 - 56, 370, 56, {0, 0, 0, 0}},          // bottom left
            {250, 8, 70, 70, 48 + 320 + 8, 8, 120, 120, {0, 0, 0, 1}},           // right of the top left one: the selected beam, as tall (filtered)
        };
        Piece pieces[4];
        int npieces = 0;
        int linear = 0;   // the text at 1.5x is filtered; the readouts at 2x stay sharp
        if (g_ss_view == SS_HUD) {
            unsigned long long now = msr_now_ns();
            pieces[0] = k_hud[0];
            pieces[1] = k_hud[1];
            npieces = 2;
            // The selected beam, always shown, as tall as the energy and Aeion readout and right of it
            // (Alec: the top right is the minimap's place)
            Piece b = k_hud[2];
            int sl = selected_beam_slot();
            b.sy = (float)k_beam_y[sl < 0 ? 0 : sl];
            pieces[npieces++] = b;
            if (g_set.minimap) {
                // The map around Samus, top right: 120x80 of the bottom screen's map (x 0-252, y 58-213,
                // under the energy bar and above the counters) at 1.5x, as tall as the readouts (120), eased
                // after her marker (msr2ds).
                static float cx = 125.0f, cy = 135.0f;   // the map's middle, where the game keeps her marker
                static unsigned long long last_ns, log_ns;
                float dt = last_ns && now > last_ns ? (float)((now - last_ns) / 1e9) : 0.0f;
                last_ns = now;
                float mx = 125.0f, my = 135.0f;
                int focus = 0;
                int found = map_marker(&mx, &my, &focus);
                if (!found || focus) {
                    mx = 125.0f;
                    my = 135.0f;
                }
                float k = dt * 12.0f > 1.0f ? 1.0f : dt * 12.0f;
                cx += (mx - cx) * k;
                cy += (my - cy) * k;
                float sx = cx - 60.0f, sy = cy - 40.0f;
                if (sx < 0.0f) sx = 0.0f;
                if (sx > 252.0f - 120.0f) sx = 252.0f - 120.0f;
                if (sy < 58.0f) sy = 58.0f;
                if (sy > 213.0f - 80.0f) sy = 213.0f - 80.0f;
                Piece m = {sx, sy, 120, 80, 1240 - 8 - 180, 8, 180, 120, {3.0f, 0.85f, 1.15f, 1.0f}};   // 1.5x, as tall as the readouts, filtered
                pieces[npieces++] = m;
                // what the minimap saw, every 5 s (0.7.4 drew an empty box): the marker, the window, and how
                // bright the bottom screen is in it and in the whole map area
                if (!log_ns || now - log_ns > 5000000000ull) {
                    log_ns = now;
                    const u32* b = g_upload + 400 * 240;
                    int peak_win = 0, peak_map = 0;
                    for (int y = 58; y < 213; y++)
                        for (int x = 0; x < 252; x++) {
                            u32 c = b[(239 - y) * 320 + x];
                            int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
                            int v = r > g ? r : g;
                            if (bl > v) v = bl;
                            if (v > peak_map) peak_map = v;
                            if (x >= (int)sx && x < (int)sx + 120 && y >= (int)sy && y < (int)sy + 80 && v > peak_win) peak_win = v;
                        }
                    static unsigned logged;
                    if (logged++ < 60)
                        msr_receipt("minimap: marker %s (%.0f, %.0f)%s, window from (%.0f, %.0f); brightest in it %d, in the map area %d",
                                    found ? "from the game" : "not found", mx, my, focus ? ", the game's map elsewhere" : "", sx, sy,
                                    peak_win, peak_map);
                }
            }
        } else {
            static unsigned long long last_ns;
            unsigned long long now = msr_now_ns();
            float dt = last_ns && now > last_ns ? (float)((now - last_ns) / 1e9) : 0.0f;
            last_ns = now;
            float e = dt * 12.0f > 1.0f ? 1.0f : dt * 12.0f;
            for (int k = 0; k < 4; k++) g_ss_shown[k] += (g_ss_card[k] - g_ss_shown[k]) * e;
            int tall = g_ss_tall_ns && now - g_ss_tall_ns < 5000000000ull;
            float sc = tall ? 1.5f : 2.0f;
            float row = tall ? 126.0f : 120.0f;                                                // the row placed at y_at
            float y_at = tall ? (FB_H * 0.95f - 90.0f * sc < FB_H * 0.8f ? FB_H * 0.95f - 90.0f * sc : FB_H * 0.8f) : FB_H * 0.8f;
            const float pad_x = 12.0f, pad_y = 7.0f;
            float a0 = g_ss_shown[0] - pad_x, b0 = g_ss_shown[1] - pad_y, a1 = g_ss_shown[2] + pad_x, b1 = g_ss_shown[3] + pad_y;
            Piece card;
            card.sx = a0;
            card.sy = b0;
            card.w = a1 - a0;
            card.h = b1 - b0;
            card.dx = FB_W / 2 + (a0 - 160.0f) * sc;
            card.dy = y_at + (b0 - row) * sc;
            card.sw = card.w * sc;
            card.sh = card.h * sc;
            card.look[0] = 2.0f;
            card.look[1] = 0.75f;
            card.look[2] = 0.0f;
            card.look[3] = 0.0f;
            Piece text = card;
            if (text.sx < 0) { text.dx -= text.sx * sc; text.w += text.sx; text.sx = 0; }
            if (text.sy < 0) { text.dy -= text.sy * sc; text.h += text.sy; text.sy = 0; }
            if (text.sx + text.w > 320) text.w = 320 - text.sx;
            if (text.sy + text.h > 240) text.h = 240 - text.sy;
            text.sw = text.w * sc;
            text.sh = text.h * sc;
            text.look[0] = 1.0f;
            text.look[1] = 1.0f;
            text.look[2] = g_ss_gain;
            g_ss_text_dst[0] = (int)text.dx;
            g_ss_text_dst[1] = (int)text.dy;
            g_ss_text_dst[2] = (int)text.sw;
            g_ss_text_dst[3] = (int)text.sh;
            g_ss_text_src[0] = text.sx;
            g_ss_text_src[1] = text.sy;
            g_ss_text_src[2] = text.w;
            g_ss_text_src[3] = text.h;
            pieces[0] = card;
            pieces[1] = text;
            npieces = 2;
            linear = tall;
        }
        // Drawn (not blitted) so the dark backgrounds can be see-through: hud_fsh.glsl.
        DkGpuAddr hd = dkMemBlockGetGpuAddr(g_hud_data);
        DkViewport vp = {0, 0, FB_W, FB_H, 0.0f, 1.0f};
        dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
        DkRasterizerState rs;
        dkRasterizerStateDefaults(&rs);
        rs.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(g_cmd, &rs);
        DkColorState cs;
        dkColorStateDefaults(&cs);
        dkColorStateSetBlendEnable(&cs, 0, true);
        dkCmdBufBindColorState(g_cmd, &cs);
        DkBlendState bls;
        dkBlendStateDefaults(&bls);
        dkBlendStateSetFactors(&bls, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_Zero, DkBlendFactor_One);
        dkCmdBufBindBlendStates(g_cmd, 0, &bls, 1);
        DkColorWriteState cw;
        dkColorWriteStateDefaults(&cw);
        dkCmdBufBindColorWriteState(g_cmd, &cw);
        DkDepthStencilState dss;
        dkDepthStencilStateDefaults(&dss);
        dss.depthTestEnable = false;
        dss.depthWriteEnable = false;
        dkCmdBufBindDepthStencilState(g_cmd, &dss);
        dkCmdBufBindImageDescriptorSet(g_cmd, hd + 0x100, 1);
        dkCmdBufBindSamplerDescriptorSet(g_cmd, hd + 0x200, 2);
        DkResHandle th = dkMakeTextureHandle(0, linear ? 1 : 0);
        dkCmdBufBindTextures(g_cmd, DkStage_Fragment, 0, &th, 1);
        int bound_linear = linear;
        DkShader const* hs[2] = {&g_hud_vsh, &g_hud_fsh};
        dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, hs, 2);
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, DkInvalidateFlags_Image);   // the blits and uploads into the images are done
        for (int k = 0; k < npieces; k++) {
            const Piece* q = &pieces[k];
            int want_linear = linear || q->look[3] > 0.5f;   // look[3]: filtered (a readout at an odd scale)
            if (want_linear != bound_linear) {
                DkResHandle h = dkMakeTextureHandle(0, want_linear ? 1 : 0);
                dkCmdBufBindTextures(g_cmd, DkStage_Fragment, 0, &h, 1);
                bound_linear = want_linear;
            }
            float rect[12] = {q->dx, FB_H - q->dy - q->sh, q->sw, q->sh,
                              q->sx, 240.0f - q->sy - q->h, q->w, q->h,
                              q->look[0], q->look[1], q->look[2], q->look[3]};
            dkCmdBufPushConstants(g_cmd, hd, 48, 0, 48, rect);
            DkBufExtents ub = {hd, 48};
            dkCmdBufBindUniformBuffers(g_cmd, DkStage_Vertex, 0, &ub, 1);
            dkCmdBufBindUniformBuffers(g_cmd, DkStage_Fragment, 0, &ub, 1);
            dkCmdBufDraw(g_cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        }
        dkCmdBufBarrier(g_cmd, DkBarrier_Fragments, 0);   // before the blits that follow
    }
    if (g_set.show_fps) {
        // New top screen pictures put on the Switch's screen, timed from the first of the last
        // second or so to the last one. Counting whole pictures over a fixed half second made a
        // steady 30 read 28 to 32, depending on where the window edges fell.
        static unsigned long long t0;
        static unsigned s0;
        static int fps10;
        unsigned s1 = shown_n;
        unsigned long long t1 = shown_ns;
        if (!t0) { t0 = t1; s0 = s1; }
        if (t1 > t0 && t1 - t0 >= 1000000000ull) {
            fps10 = (int)((unsigned long long)(s1 - s0) * 10000000000ull / (t1 - t0));
            if (fps10 > 999) fps10 = 999;
            t0 = t1;
            s0 = s1;
        } else if (msr_now_ns() - t1 > 2000000000ull) {
            fps10 = 0;   // no picture for 2 s (loading)
            t0 = 0;
        }
        DkImageView fv;
        dkImageViewDefaults(&fv, &g_fps_img);
        const u32* px = port_menu_fps(fps10);
        if (px) {
            u32* dst = (u32*)dkMemBlockGetCpuAddr(g_fps_mem);
            for (u32 y = 0; y < FPS_H; y++) memcpy(dst + y * FPS_W, px + (FPS_H - 1 - y) * FPS_W, FPS_W * 4);
            DkCopyBuf src = {dkMemBlockGetGpuAddr(g_fps_mem), 0, 0};
            DkImageRect r = {0, 0, 0, FPS_W, FPS_H, 1};
            dkCmdBufCopyBufferToImage(g_cmd, &src, &fv, &r, 0);
        }
        DkImageRect fs = {0, 0, 0, FPS_W, FPS_H, 1}, fd = {FB_W - 8 - FPS_W, 8, 0, FPS_W, FPS_H, 1};   // bottom right
        dkCmdBufBlitImage(g_cmd, &fv, &fs, &fbv, &fd, 0, 0);
    }
    if (g_menu_open) {
        DkImageView pv;
        dkImageViewDefaults(&pv, &g_panel_img);
        const u32* px = port_menu_panel();
        if (px) {   // rows bottom to top, like the screens (the output is shown upside down)
            u32* dst = (u32*)dkMemBlockGetCpuAddr(g_panel_mem);
            for (u32 y = 0; y < PANEL_H; y++) memcpy(dst + y * PANEL_W, px + (PANEL_H - 1 - y) * PANEL_W, PANEL_W * 4);
            DkCopyBuf src = {dkMemBlockGetGpuAddr(g_panel_mem), 0, 0};
            DkImageRect r = {0, 0, 0, PANEL_W, PANEL_H, 1};
            dkCmdBufCopyBufferToImage(g_cmd, &src, &pv, &r, 0);
        }
        DkImageRect ps = {0, 0, 0, PANEL_W, PANEL_H, 1}, pd = {(FB_W - PANEL_W) / 2, (FB_H - PANEL_H) / 2, 0, PANEL_W, PANEL_H, 1};
        dkCmdBufBlitImage(g_cmd, &pv, &ps, &fbv, &pd, 0, 0);
    }
    if (g_out_shot == 1) {   // the finished output into memory the CPU reads after the wait below
        if (!g_out_mem) {
            DkMemBlockMaker mm;
            dkMemBlockMakerDefaults(&mm, g_dev, (FB_W * FB_H * 4 + 0xFFF) & ~0xFFFu);
            mm.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
            g_out_mem = dkMemBlockCreate(&mm);
        }
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, 0);
        DkCopyBuf ob = {dkMemBlockGetGpuAddr(g_out_mem), 0, 0};
        DkImageRect orr = {0, 0, 0, FB_W, FB_H, 1};
        dkCmdBufCopyImageToBuffer(g_cmd, &fbv, &orr, &ob, 0);
        g_out_shot = 2;
    }
    // Submit and present, then wait for this list alone, outside the queue lock: waiting for the
    // whole queue under the lock made the game thread stop until the GPU was idle (test 6c).
    DkFence done;
    mutexLock(&g_qlock);
    dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(g_cmd));
    dkQueueSignalFence(g_queue, &done, false);
    dkQueuePresentImage(g_queue, g_swap, slot);
    dkQueueFlush(g_queue);
    mutexUnlock(&g_qlock);
    dkFenceWait(&done, -1);
    dkCmdBufClear(g_cmd);
    if (g_out_shot == 2) {   // rows bottom to top there (the output is shown upside down)
        g_out_shot = 0;
        const u32* src = (const u32*)dkMemBlockGetCpuAddr(g_out_mem);
        armDCacheFlush((void*)src, FB_W * FB_H * 4);
        u8* rgb = malloc(FB_W * FB_H * 3);
        if (rgb) {
            for (u32 y = 0; y < FB_H; y++)
                for (u32 x = 0; x < FB_W; x++) {
                    u32 c = src[(FB_H - 1 - y) * FB_W + x];
                    u8* o = rgb + (y * FB_W + x) * 3;
                    o[0] = c & 0xFF; o[1] = (c >> 8) & 0xFF; o[2] = (c >> 16) & 0xFF;
                }
            char path[300];
            snprintf(path, sizeof path, "%s/%s_screen.png", g_run_dir, g_out_shot_tag);
            int msr_write_png(const char* path, const u8* rgb, u32 w, u32 h);
            msr_write_png(path, rgb, FB_W, FB_H);
            free(rgb);
            msr_receipt("  picture of the Switch output saved: %s_screen.png", g_out_shot_tag);
        }
    }
    MSR_PERF_END(PERF_PRESENT);
}

// ---- audio: the DSP's 32728 Hz mix, resampled to 48 kHz -----------------------------------------

#define AUD_SAMPLES 1440   // 30 ms per buffer
#define RING (32768 * 2)
static AudioOutBuffer g_abuf[3];
static s16* g_adata[3];

static void* audio_thread(void* arg) {
    (void)arg;
    msr_thread_to_core(2);
    u32 size = (AUD_SAMPLES * 4 + 0xFFF) & ~0xFFFu;
    for (int k = 0; k < 3; k++) {
        g_adata[k] = (s16*)memalign(0x1000, size);
        memset(g_adata[k], 0, size);
        g_abuf[k].buffer = g_adata[k];
        g_abuf[k].buffer_size = size;
        g_abuf[k].data_size = AUD_SAMPLES * 4;
        g_abuf[k].data_offset = 0;
        audoutAppendAudioOutBuffer(&g_abuf[k]);
    }
    stretch_reset();
    for (;;) {
        AudioOutBuffer* done = 0;
        u32 n = 0;
        audoutWaitPlayFinish(&done, &n, UINT64_MAX);
        if (!done) continue;
        // stretched to the game's speed (no crackle in slowdowns), then resampled to 48 kHz
        stretch_fill((s16*)done->buffer, AUD_SAMPLES, 48000.0, msr_audio_ring, RING, msr_audio_write);
        audoutAppendAudioOutBuffer(done);
    }
    return 0;
}

// ---- input ------------------------------------------------------------------------------------------

static PadState g_pad;
static char g_run_dir[128];
// The shader pack, built into msr.nro by tools/switch/nxbuild.sh when the build has one.
extern const u8 msr_pack_data[] __attribute__((weak));
extern const u8 msr_pack_end[] __attribute__((weak));
// The fragment configurations the pack was made from (fs.bin), for the start-up self-test.
extern const u8 msr_fsrec_data[] __attribute__((weak));
extern const u8 msr_fsrec_end[] __attribute__((weak));

int msr_write_png(const char* path, const u8* rgb, u32 w, u32 h);
static u32 g_shot_buf[400 * 240];
static u8 g_shot_rgb[400 * 240 * 3];
static void convert_screen(int screen, u32* out, int flip);
// Both screens as the game drew them, straight from its framebuffers.
static void save_shots(const char* tag) {
    for (int sc = 0; sc < 2; sc++) {
        u32 W = sc ? 320 : 400;
        if (g_gpu_render) {   // what present() last put up (rows bottom to top there)
            const u32* src = g_upload + (sc ? 400 * 240 : 0);
            for (u32 y = 0; y < 240; y++) memcpy(g_shot_buf + y * W, src + (239 - y) * W, W * 4);
        } else convert_screen(sc, g_shot_buf, 0);
        for (u32 i = 0; i < W * 240; i++) {
            g_shot_rgb[i * 3] = g_shot_buf[i] & 0xFF;
            g_shot_rgb[i * 3 + 1] = (g_shot_buf[i] >> 8) & 0xFF;
            g_shot_rgb[i * 3 + 2] = (g_shot_buf[i] >> 16) & 0xFF;
        }
        char path[300];
        snprintf(path, sizeof path, "%s/%s_%s.png", g_run_dir, tag, sc ? "bottom" : "top");
        msr_write_png(path, g_shot_rgb, W, 240);
    }
    msr_receipt("  pictures saved: %s_top.png, %s_bottom.png", tag, tag);
    snprintf(g_out_shot_tag, sizeof g_out_shot_tag, "%s", tag);
    g_out_shot = 1;   // and the output, at the next present
}

// Clock speeds as the system has them now (sys-clk changes included).
static void clocks(u32* cpu, u32* gpu, u32* mem) {
    *cpu = *gpu = *mem = 0;
    if (hosversionAtLeast(8, 0, 0)) {
        static int ok = -1;
        if (ok < 0) ok = R_SUCCEEDED(clkrstInitialize());
        if (!ok) return;
        PcvModuleId ids[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
        u32* outs[3] = {cpu, gpu, mem};
        for (int k = 0; k < 3; k++) {
            ClkrstSession s;
            if (R_SUCCEEDED(clkrstOpenSession(&s, ids[k], 3))) {
                clkrstGetClockRate(&s, outs[k]);
                clkrstCloseSession(&s);
            }
        }
    } else {
        static int ok = -1;
        if (ok < 0) ok = R_SUCCEEDED(pcvInitialize());
        if (!ok) return;
        pcvGetClockRate(PcvModule_CpuBus, cpu);
        pcvGetClockRate(PcvModule_GPU, gpu);
        pcvGetClockRate(PcvModule_EMC, mem);
    }
}
static unsigned long long g_start_ns;

static void input_poll(void) {
    padUpdate(&g_pad);
    u64 k = padGetButtons(&g_pad);
    u32 b = port_menu_input(k);   // the port menu's button mapping (0 while the menu is open)
    // ZL + ZR + minus held for a second: capture the next frame for the reference renderer
    {
        static unsigned long long held_since;
        static int fired;
        u64 combo = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus;
        if ((k & combo) == combo) {
            unsigned long long t = msr_now_ns();
            if (!held_since) held_since = t;
            if (!fired && t - held_since >= 1000000000ull) {   // a second by the clock (test 7 counted loops)
                fired = 1;
                static int nth;
                char path[300];
                snprintf(path, sizeof path, "%s/capture%d.bin", g_run_dir, ++nth);
                void msr_capture_request(const char* path);
                msr_capture_request(path);
                msr_receipt("input: frame capture requested (%s)", path);
            }
        } else { held_since = 0; fired = 0; }
    }
    // the first presses go to the receipt, to check that the game is being told about them
    static u32 last_b;
    static unsigned logged;
    if (b != last_b && logged < 60) {
        logged++;
        msr_receipt("input: buttons %03X at %.1f s (game has shown %u top-screen pictures)", b,
                    (msr_now_ns() - g_start_ns) / 1e9, msr_fb[0].swaps);
    }
    if ((b & 0x008u) && !(last_b & 0x008u)) g_start_pressed_ns = msr_now_ns();   // the 3DS Start button
    last_b = b;
    msr_buttons = b;
    HidAnalogStickState s = padGetStickPos(&g_pad, 0);
    msr_circle_x = g_menu_open ? 0 : (s16)(s.x * 156 / 32767);
    msr_circle_y = g_menu_open ? 0 : (s16)(s.y * 156 / 32767);
    // touch screen (handheld): wherever the layout puts the bottom screen
    HidTouchScreenState ts = {0};
    msr_touch_down = 0;
    Rect tr, br;
    port_menu_layout(&tr, &br);
    if (!g_menu_open && br.w && hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        int x = ((int)ts.touches[0].x - br.x) * 320 / br.w, y = ((int)ts.touches[0].y - br.y) * 240 / br.h;
        if ((int)ts.touches[0].x >= br.x && (int)ts.touches[0].y >= br.y && x < 320 && y < 240) {
            msr_touch_down = 1; msr_touch_x = (u16)x; msr_touch_y = (u16)y;
        }
    } else if (!g_menu_open && g_ss_view == SS_TEXT && g_ss_text_dst[2] > 0 && hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        // single-screen subtitles: a touch on them is a touch on that text (the prologue's skip)
        int tx = (int)ts.touches[0].x - g_ss_text_dst[0], ty = (int)ts.touches[0].y - g_ss_text_dst[1];
        if (tx >= 0 && ty >= 0 && tx < g_ss_text_dst[2] && ty < g_ss_text_dst[3]) {
            msr_touch_down = 1;
            msr_touch_x = (u16)(g_ss_text_src[0] + tx * g_ss_text_src[2] / g_ss_text_dst[2]);
            msr_touch_y = (u16)(g_ss_text_src[1] + ty * g_ss_text_src[3] / g_ss_text_dst[3]);
        }
    }
    {   // both sticks pressed in: a picture of the Switch output now
        static int was;
        u64 held = padGetButtons(&g_pad);
        int both = (held & HidNpadButton_StickL) && (held & HidNpadButton_StickR);
        if (both && !was && !g_out_shot) {
            snprintf(g_out_shot_tag, sizeof g_out_shot_tag, "p%03d", (int)((msr_now_ns() - g_start_ns) / 1000000000ull));
            g_out_shot = 1;
            if (g_set.scale && g_debug) {   // and the renderer's own images on the way (dk_pica.c dump_all)
                char pre[200];
                snprintf(pre, sizeof pre, "%s/%s", g_run_dir, g_out_shot_tag);
                void dkr_request_dump(const char* prefix);
                dkr_request_dump(pre);
            }
        }
        was = both;
    }
    // Next beam (port menu, ZR): a tap on the next slot of the beam column, as a finger would. An
    // empty slot does nothing, so if the lit slot has not changed a moment later the one after it is
    // tried (each slot once).
    static int bt_frames, bt_from, bt_target, bt_tries;
    if (port_menu_beam_request() && !g_menu_open && !bt_frames) {
        bt_from = selected_beam_slot();
        if (bt_from >= 0) { bt_target = (bt_from + 1) % 3; bt_tries = 0; bt_frames = 1; }
    }
    if (bt_frames) {
        if (bt_frames <= 4) { msr_touch_down = 1; msr_touch_x = 285; msr_touch_y = (u16)(k_beam_y[bt_target] + 35); }
        if (++bt_frames > 16) {   // about a quarter second: did the lit slot move?
            int now_sel = selected_beam_slot();
            if (now_sel != bt_from || ++bt_tries >= 2) {
                msr_receipt("input: next beam, slot %d to %d", bt_from, now_sel);
                bt_frames = 0;
            } else { bt_target = (bt_target + 1) % 3; bt_frames = 1; }
        }
    }
}

// ---- main -------------------------------------------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    appletLockExit();
    // The game's 512 MiB memory window first, before any thread exists: every thread's stack is
    // placed at a random spot in the same address region, and with the renderer's, the shader
    // compiler's and the sound thread's stacks already there, a free 512 MiB stretch was sometimes
    // not left (a run stopped at "could not reserve the 4 GiB guest window" and the next launch
    // worked). boot.c's own call then finds it reserved.
    msr_mem_init();
    mkdir(DIR, 0777);
    struct stat st;
    msr_full_log = stat(DIR "/fulllog", &st) == 0;   // create this file to get the detailed msr.log
    g_debug = stat(DIR "/debug", &st) == 0;          // create this file for test runs: pictures every 30 s, renderer dumps
    // every launch gets its own folder for the receipt, pictures and anything else it writes
    mkdir(DIR "/switchlogs", 0777);
    // The next number after the highest one there (0.5.0 took the lowest free number, and its own
    // clean-up of old runs freed low numbers, so new runs hid among old ones), then the runs ten or
    // more numbers back are removed.
    int top = 0;
    {
        DirHandle* d = opendir(DIR "/switchlogs");
        if (d) {
            struct dirent* e;
            while ((e = readdir(d))) {
                int k;
                if (sscanf(e->d_name, "run%d", &k) == 1 && k > top) top = k;
            }
            closedir(d);
        }
    }
    int n = top + 1 > 999 ? 1 : top + 1;
    snprintf(g_run_dir, sizeof g_run_dir, DIR "/switchlogs/run%03d", n);
    mkdir(g_run_dir, 0777);
    if (!g_debug) {   // keep the last thirty runs
        DirHandle* d = opendir(DIR "/switchlogs");
        char old[160], f[300];
        int gone[64], ng = 0;
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) && ng < 64) {
                int k;
                if (sscanf(e->d_name, "run%d", &k) == 1 && k <= n - 30) gone[ng++] = k;
            }
            closedir(d);
        }
        for (int g = 0; g < ng; g++) {
            snprintf(old, sizeof old, DIR "/switchlogs/run%03d", gone[g]);
            DirHandle* r = opendir(old);
            if (!r) continue;
            struct dirent* e;
            while ((e = readdir(r))) {
                if (e->d_name[0] == '.') continue;
                snprintf(f, sizeof f, "%s/%s", old, e->d_name);
                remove(f);
            }
            closedir(r);
            rmdir(old);
        }
    }
    msr_log_open(g_run_dir);
    msr_log_buffered = 1;   // the main loop writes the logs out; the game's threads never wait on the SD card
    int cpu_render = stat(DIR "/cpurender", &st) == 0;   // create this file to draw with the CPU reference renderer
    msr_receipt("MSR-nx " MSR_VERSION " (%s)", cpu_render ? "CPU reference renderer" : "deko3d PICA renderer");
    snprintf(msr_save_dir, sizeof msr_save_dir, DIR "/save");
    sched_realtime = 1;
    { extern size_t sched_thread_stack; sched_thread_stack = 4u << 20; }
    msr_time_limit_ns = ~0ull;
    msr_svc_limit = ~0u;

    port_menu_init(DIR "/settings.ini");
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    mutexInit(&g_qlock);
    display_init();
    if (!cpu_render) {
        dkr_init(g_dev, g_queue, &g_qlock);
        void dkr_load_pack(const u8* data, size_t size, const char* new_path, const char* fs_new_path);
        char vs_new[300], fs_new[300];
        snprintf(vs_new, sizeof vs_new, "%s/vs_new.bin", g_run_dir);
        snprintf(fs_new, sizeof fs_new, "%s/fs_new.bin", g_run_dir);
        dkr_load_pack(msr_pack_data, msr_pack_data ? (size_t)(msr_pack_end - msr_pack_data) : 0, vs_new, fs_new);
        void dkr_compiler_init(const char* cache_dir);
        dkr_compiler_init(DIR "/shadercache");
        void dkr_selftest(const u8* rec, size_t size, const char* png_path);
        char st_png[300];
        snprintf(st_png, sizeof st_png, "%s/selftest.png", g_run_dir);
        dkr_selftest(msr_fsrec_data, msr_fsrec_data ? (size_t)(msr_fsrec_end - msr_fsrec_data) : 0, st_png);
        void dkr_scaler_selftest(void);
        dkr_scaler_selftest();
        g_gpu_render = 1;
    }
    if (R_SUCCEEDED(audoutInitialize()) && R_SUCCEEDED(audoutStartAudioOut())) {
        pthread_t at;
        pthread_create(&at, 0, audio_thread, 0);
    } else msr_receipt("audio: audout did not start");

    g_main_cpu = msr_boot(DIR);
    if (!g_main_cpu) {
        msr_log_close();
        appletUnlockExit();
        return 1;
    }
    // msr_boot took the baton for the main guest thread on this host thread; the game runs on its
    // own thread with a large stack, so hand the baton over by running it there.
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);
    pthread_t gt;
    void sched_handoff_main(void);
    sched_handoff_main();
    pthread_create(&gt, &at, game_main, 0);

    g_start_ns = msr_now_ns();
    unsigned long long next_report = g_start_ns + 5000000000ull;
    unsigned last_swaps = 0;
    int reports = 0;
    unsigned long long shot_at = g_debug ? g_start_ns + 5000000000ull : 0;
    void dkr_force_general(int vs, int fs);
    if (g_gpu_render) dkr_force_general(0, 0);   // the shader pack, with the general shaders only for misses
    msr_thread_to_core(2);
    unsigned long long next_pump = 0;
    while (appletMainLoop()) {
        input_poll();
        present();
        unsigned long long now = msr_now_ns();
        if (now >= next_pump) { msr_log_pump(); next_pump = now + 1000000000ull; }
        if (now >= next_report) {
            // the speed meter: the real 3DS shows up to 60 pictures a second on the top screen
            double secs = (now - next_report + 5000000000ull) / 1e9;
            unsigned sw = msr_fb[0].swaps;
            u32 ccpu, cgpu, cmem;
            clocks(&ccpu, &cgpu, &cmem);
            msr_receipt("speed at %.0f s: %.1f top-screen pictures per second; clocks CPU %u MHz, GPU %u MHz, memory %u MHz",
                        (now - g_start_ns) / 1e9, (sw - last_swaps) / secs, ccpu / 1000000, cgpu / 1000000,
                        cmem / 1000000);
            msr_perf_report(secs);
            port_menu_clocks_refresh();
            if (g_gpu_render) {
                void dkr_vs_stats(unsigned* hits, unsigned* misses);
                unsigned h, m;
                dkr_vs_stats(&h, &m);
                msr_receipt("  vertex shaders: %u draws translated, %u on the interpreter", h, m);
                void dkr_fs_stats(unsigned* hits, unsigned* misses, unsigned* waits);
                unsigned wt;
                dkr_fs_stats(&h, &m, &wt);
                msr_receipt("  fragment shaders: %u draws specialized, %u on the general one, %u waited for a compile", h, m, wt);
                void dkr_compile_stats(unsigned* compiled, unsigned* avg_ms, unsigned* max_ms, unsigned* pending);
                unsigned cc, ca, cm, cp;
                dkr_compile_stats(&cc, &ca, &cm, &cp);
                if (cc || cp) msr_receipt("  compiled on the Switch so far: %u (%u ms each on average, longest %u ms), %u in progress", cc, ca, cm, cp);
                void dkr_gpu_times(unsigned* draw_ms, unsigned* vram_ms, unsigned* other_ms);
                unsigned gd, gv, go;
                dkr_gpu_times(&gd, &gv, &go);
                msr_receipt("  GPU time: draws %u ms, VRAM work %u ms, elsewhere or idle %u ms", gd, gv, go);
                void dkr_target_report(void);
                dkr_target_report();
                {
                    void dkr_latch_stats(unsigned* waits, unsigned* ms);
                    unsigned lw, lm;
                    dkr_latch_stats(&lw, &lm);
                    static unsigned shown_last;
                    msr_receipt("  screen: %u new top-screen pictures shown; show newest frame %s, %u waits (%u ms)",
                                g_shown_total - shown_last, g_set.newest ? "on" : "off", lw, lm);
                    shown_last = g_shown_total;
                    unsigned long long msr_interp_steps(void);
                    static unsigned long long steps_last;
                    unsigned long long st = msr_interp_steps();
                    if (st != steps_last) msr_receipt("  interpreter: %llu instructions run", st - steps_last);
                    steps_last = st;
                }
                extern unsigned eye_runs[2], eye_total[2];
                static unsigned er0, er1, et0, et1;
                msr_receipt("  eyes: left %u draws run of %u, right %u run of %u (the frame plan skips the rest)", eye_runs[0] - er0, eye_total[0] - et0,
                            eye_runs[1] - er1, eye_total[1] - et1);
                er0 = eye_runs[0]; er1 = eye_runs[1]; et0 = eye_total[0]; et1 = eye_total[1];
                void dkr_barrier_stats(unsigned* asked, unsigned* done);
                unsigned ba, bd;
                dkr_barrier_stats(&ba, &bd);
                msr_receipt("  GPU barriers: %u recorded of %u asked (GPU sync %s)", bd, ba,
                            g_set.sync == 0 ? "Safe" : g_set.sync == 1 ? "Fewer waits" : "Lean");
                unsigned dkr_ez_stats(void);
                msr_receipt("  early depth: %u draws", dkr_ez_stats());
                void dkr_vram_stats(unsigned* out);
                unsigned vo[16];   // K_COUNT (13) entries; was 9, written past its end
                dkr_vram_stats(vo);
                msr_receipt("  VRAM on the GPU: %u fills, %u copies (%u from images), %u transfers (%u from images), %u stores, %u loads, %u texture decodes",
                            vo[0], vo[1] + vo[8], vo[8], vo[2] + vo[7], vo[7], vo[3], vo[4], vo[5]);
                {
                    {   // the game's threads waiting for something (idle) and the graphics thread at work
                    extern unsigned long long msr_idle_ns, msr_idle_gx_ns, msr_gx_busy_ns, msr_frame_worst_ns;
                    extern unsigned msr_frame_gaps[3];
                    extern volatile int msr_gx_async;
                    static unsigned long long i0, g0, b0;
                    static unsigned f0[3];
                    unsigned long long i1 = msr_idle_ns, g1 = msr_idle_gx_ns, b1 = msr_gx_busy_ns;
                    msr_receipt("  threads per 5 s: game idle %llu ms (%llu of it waiting for the graphics thread, %llu for the screen or a timer), graphics thread busy %llu ms (graphics thread %s)",
                                (i1 - i0) / 1000000ull, (g1 - g0) / 1000000ull, ((i1 - i0) - (g1 - g0)) / 1000000ull,
                                (b1 - b0) / 1000000ull, msr_gx_async ? "on" : "off");
                    unsigned fg[3] = {msr_frame_gaps[0], msr_frame_gaps[1], msr_frame_gaps[2]};
                    if (g_set.fps60)
                        msr_receipt("  game frames: %u, %u of them late (missed a 60 Hz slot), %u missed two or more; longest %.1f ms",
                                    fg[0] - f0[0], fg[1] - f0[1], fg[2] - f0[2], msr_frame_worst_ns / 1e6);
                    else
                        msr_receipt("  game frames: %u (frame rate 30), %u more than 35 ms after the last; longest %.1f ms",
                                    fg[0] - f0[0], fg[2] - f0[2], msr_frame_worst_ns / 1e6);
                    msr_frame_worst_ns = 0;
                    for (int k = 0; k < 3; k++) f0[k] = fg[k];
                    i0 = i1; g0 = g1; b0 = b1;
                }
                unsigned dkr_hires_count(void), dkr_shadow_count(void);
                    msr_receipt("  3D screen resolution: %u pictures made at the larger size, %u shown from them, %u at native size; %u textures read at the larger size",
                                dkr_hires_count(), g_hires_shown, g_native_shown, dkr_shadow_count());
                    g_hires_shown = g_native_shown = 0;
                }
                static unsigned last_skip, last_total, last_realign;
                extern unsigned fp_realigned;
                msr_receipt("  frame plan: %u of %u GPU operations skipped (right eye and other unseen work), %u realigned",
                            fp_skipped_ops - last_skip, fp_total_ops - last_total, fp_realigned - last_realign);
                last_skip = fp_skipped_ops; last_total = fp_total_ops; last_realign = fp_realigned;
            }
            last_swaps = sw;
            next_report = now + 5000000000ull;
            // pictures of both screens every 30 seconds, for comparing against the reference renderer
            if (++reports % 6 == 0 && g_debug) shot_at = now;
        }
        {   // the top screen mostly white (the white-out): capture that frame, up to twice a run. Off
            // since test 16: the white-out is fixed, the game's own white screens tripped it, and
            // saving the 80 MB capture stalls the game
            static unsigned long long next_check;
            static int auto_caps = 2;
            if (g_gpu_render && auto_caps < 2 && now >= next_check) {
                next_check = now + 1000000000ull;
                unsigned white = 0, total = 0;
                for (u32 y = 8; y < 240; y += 16)
                    for (u32 x = 8; x < 400; x += 16) {
                        u32 c = g_upload[y * 400 + x];
                        if ((c & 0xFF) > 235 && ((c >> 8) & 0xFF) > 235 && ((c >> 16) & 0xFF) > 235) white++;
                        total++;
                    }
                if (white * 10 >= total * 8) {   // 80% of the samples
                    char path[300];
                    snprintf(path, sizeof path, "%s/autocapture%d.bin", g_run_dir, ++auto_caps);
                    void msr_capture_request(const char* path);
                    msr_capture_request(path);
                    msr_receipt("white-out detected (%u of %u samples white): capturing the next frame (%s)", white, total, path);
                    shot_at = now;   // and pictures of both screens
                    next_check = now + 20000000000ull;
                }
            }
        }
        if (g_shot_request) { g_shot_request = 0; shot_at = now + 300000000ull; }   // the pause view: once it has settled
        if (shot_at && now >= shot_at) {
            char tag[48];
            snprintf(tag, sizeof tag, "t%03d", (int)((now - g_start_ns) / 1000000000ull));
            save_shots(tag);
            shot_at = 0;
        }
        if ((padGetButtons(&g_pad) & (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_L | HidNpadButton_R)) ==
            (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_L | HidNpadButton_R))
            break;   // L + R + plus + minus: quit
        // Look at the screens every 2 ms at both rates. At 30 this slept 16.7 ms on top of the loop's
        // own work, so a new picture waited anywhere from 0 to about 20 ms before being shown and
        // landed on an uneven refresh: pictures held for 1, 2 or 3 refreshes (judder). A look with
        // nothing new returns at once, so this costs almost nothing.
        svcSleepThread(2000000);
    }
    port_menu_clocks_restore();
    msr_receipt("quit");
    msr_log_close();
    appletUnlockExit();
    exit(0);
}
