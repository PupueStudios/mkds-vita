#include "port.h"
#include "frame.h"
#include "guest_if.h"
#include "present.h"
#include "gl3d.h"
#include "hostapi.h"
#include "vita.h"
#include "menu.h"
#include "mods.h"

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/power.h>
#include <psp2/system_param.h>
#include <psp2/touch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <vitaGL.h>

int _newlib_heap_size_user = 128 * 1024 * 1024;
#define VGL_RAM_POOL (40 * 1024 * 1024)
#define CDIALOG_MEM_SIZE 0x8C6000
unsigned int sceUserMainThreadStackSize = 1024 * 1024;

#define LOG_LINES 8
static char s_log_lines[LOG_LINES][128];
static int s_log_next;
static bool s_gl_ready;
static atomic_bool s_fatal;
static SceUID s_main_thread;

void vita_log_line(const char *line) {
    snprintf(s_log_lines[s_log_next], sizeof(s_log_lines[0]), "%s", line);
    s_log_next = (s_log_next + 1) % LOG_LINES;
}

static void show_message(const char *text) {
    if (!s_gl_ready) return;
    SceMsgDialogUserMessageParam msg;
    memset(&msg, 0, sizeof(msg));
    msg.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    msg.msg = (const SceChar8 *) text;
    SceMsgDialogParam param;
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg;
    if (sceMsgDialogInit(&param) < 0) return;
    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        vglSwapBuffers(GL_TRUE);
    }
    sceMsgDialogTerm();
}

void vita_fatal_screen(void) {
    if (sceKernelGetThreadId() != s_main_thread) {
        atomic_store(&s_fatal, true);
        for (;;) sceKernelDelayThread(1000000);
    }
    char text[LOG_LINES * 130] = "The game stopped:\n";
    for (int i = 0; i < LOG_LINES; i++) {
        const char *l = s_log_lines[(s_log_next + i) % LOG_LINES];
        if (l[0]) {
            strncat(text, l, sizeof(text) - strlen(text) - 2);
            strcat(text, "\n");
        }
    }
    show_message(text);
}

uint64_t port_os_last_vblank_abs_ns(void);
void port_os_request_vblank_shift(int64_t ns);
static volatile int32_t s_pipeline_ns;
static int pacing_thread(SceSize args, void *argp) {
    (void) args;
    (void) argp;
    for (;;) {
        sceDisplayWaitVblankStart();
        uint64_t now = port_now_ns(), last = port_os_last_vblank_abs_ns();
        if (!last || !g_vblank_rate_mhz) continue;
        int64_t period = (int64_t) (1000000000000ull / g_vblank_rate_mhz);
        int64_t lat = s_pipeline_ns, after = 1000000;
        if (lat > 0) {
            after = (period / 2 - lat) % period;
            if (after < 0) after += period;
        }
        int64_t err = (int64_t) (now - last) - (period - after);
        err %= period;
        if (err > period / 2) err -= period;
        if (err < -period / 2) err += period;
        int64_t step = err / 8;
        if (step > 200000) step = 200000;
        if (step < -200000) step = -200000;
        if (step) port_os_request_vblank_shift(step);
    }
    return 0;
}

static void fail(const char *text) {
    port_log("%s", text);
    show_message(text);
    hapi_log_flush();
    sceKernelExitProcess(0);
}

static bool s_single, s_swapped, s_shown_race, s_shown_dual, s_hud_active, s_hud_shown;
static bool s_redraw = true;
static int s_layout[8];
static PresentHudLayout s_hud_layout;

static void set_rect(int *r, int x, int y, int w, int h) { r[0] = x, r[1] = y, r[2] = w, r[3] = h; }

int port_one_screen(void) { return !s_single && !s_swapped && !vita_menu_classic(); }

static void apply_screens_layout(void);
static void apply_screens(void) {
    s_redraw = true;
    apply_screens_layout();
    int bottom_area = s_layout[6] * s_layout[7];
    present_set_screen_areas(s_layout[2] * s_layout[3], s_hud_active && !s_hud_shown && !bottom_area ? 1 : bottom_area);
    present_set_hud(s_hud_active ? &s_hud_layout : NULL, s_hud_shown);
}
static void apply_screens_layout(void) {
    const int w = VITA_SCREEN_W, h = VITA_SCREEN_H;
    const int w43 = h * 4 / 3;
    int *top = s_layout, *bottom = s_layout + 4;
    memset(s_layout, 0, sizeof(s_layout));
    s_hud_shown = false;
    s_shown_race = present_is_race();
    s_shown_dual = present_is_dual_3d();
    s_hud_active = !s_single && !s_swapped && s_shown_race && !vita_menu_classic();
    if (s_single) {
        if (s_shown_race != s_swapped) {
            top[2] = w, top[3] = h;
        } else {
            bottom[0] = (w - w43) / 2, bottom[2] = w43, bottom[3] = h;
        }
        return;
    }
    if (s_hud_active) {
        const int m = 10;
        set_rect(s_hud_layout.list, 15, 145, 333, 250);
        set_rect(s_hud_layout.time, (w - 272) / 2, 6, 272, 48);
        set_rect(s_hud_layout.side, w - 154 - m, h - 217 - m, 154, 217);
        set_rect(s_hud_layout.map, w - 290 - m, h - 217 - m, 290, 217);
        set_rect(s_hud_layout.round, w - m - 110 - 180, h - m - 110 - 146, 361, 271);
        set_rect(s_hud_layout.whole, w - 290 - m, h - 217 - m, 290, 217);
        set_rect(s_hud_layout.pause, (w - 435) / 2, (h - 326) / 2, 435, 326);
        if (present_hud_usable()) {
            top[2] = w, top[3] = h;
            s_hud_shown = true;
            return;
        }
    }
    if (port_one_screen() && s_shown_dual) {
        top[2] = w, top[3] = h;
        return;
    }
    int ih = h * 32 / 100, side = (w - w43) / 2 * 3 / 4;
    if (side > ih) ih = side;
    int iw = ih * 4 / 3;
    int *main_rect = s_swapped ? bottom : top, *inset = s_swapped ? top : bottom;
    if (s_swapped) {
        main_rect[0] = (w - w43) / 2, main_rect[1] = 0, main_rect[2] = w43, main_rect[3] = h;
    } else {
        main_rect[0] = 0, main_rect[1] = 0, main_rect[2] = w, main_rect[3] = h;
    }
    inset[0] = w - iw, inset[1] = 0, inset[2] = iw, inset[3] = ih;
}

static const struct {
    uint32_t vita;
    uint16_t ds;
} s_buttons[] = {
    {SCE_CTRL_CIRCLE, PAD_A}, {SCE_CTRL_CROSS, PAD_B},   {SCE_CTRL_TRIANGLE, PAD_X}, {SCE_CTRL_SQUARE, PAD_Y},
    {SCE_CTRL_LTRIGGER, PAD_L}, {SCE_CTRL_RTRIGGER, PAD_R}, {SCE_CTRL_START, PAD_START},
    {SCE_CTRL_UP, PAD_UP},    {SCE_CTRL_DOWN, PAD_DOWN}, {SCE_CTRL_LEFT, PAD_LEFT},  {SCE_CTRL_RIGHT, PAD_RIGHT},
};

static uint32_t s_prev_buttons;
static bool s_select_combo;
static int s_select_pulse;

static void read_input(void) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof(pad));
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = pad.buttons, pressed = b & ~s_prev_buttons, released = ~b & s_prev_buttons;
    s_prev_buttons = b;

    PortInput in;
    memset(&in, 0, sizeof(in));
    SceTouchData touch;
    memset(&touch, 0, sizeof(touch));
    sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);
    bool was_open = vita_menu_open();
    if (vita_menu_input(touch.reportNum, touch.report[0].x * VITA_SCREEN_W / 1920,
                        touch.report[0].y * VITA_SCREEN_H / 1088, pressed, b)) {
        if (vita_menu_open() || was_open) s_redraw = true;
        if (vita_menu_take_layout_change()) apply_screens();
        s_select_combo = false;
        s_select_pulse = 0;
        guest_input_set(&in);
        return;
    }
    if (b & SCE_CTRL_SELECT) {
        if (pressed & SCE_CTRL_RTRIGGER) {
            s_swapped = !s_swapped;
            s_select_combo = true;
            apply_screens();
        }
        if (pressed & SCE_CTRL_LTRIGGER) {
            s_single = !s_single;
            s_swapped = false;
            s_select_combo = true;
            apply_screens();
        }
        b &= ~(SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER);
        if (!s_select_combo) in.buttons |= PAD_SELECT;
    } else if (released & SCE_CTRL_SELECT) {
        if (!s_select_combo) s_select_pulse = 4;
        s_select_combo = false;
    }
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); i++) {
        if (b & s_buttons[i].vita) in.buttons |= s_buttons[i].ds;
    }
    if (s_select_pulse > 0) {
        s_select_pulse--;
        in.buttons |= PAD_SELECT;
    }
    if (pad.lx < 128 - 48) in.buttons |= PAD_LEFT;
    if (pad.lx > 128 + 48) in.buttons |= PAD_RIGHT;
    if (pad.ly < 128 - 48) in.buttons |= PAD_UP;
    if (pad.ly > 128 + 48) in.buttons |= PAD_DOWN;

    const int *r = s_layout + 4;
    int hx, hy;
    if (touch.reportNum > 0 && s_hud_shown &&
        present_hud_touch(touch.report[0].x * VITA_SCREEN_W / 1920, touch.report[0].y * VITA_SCREEN_H / 1088, &hx, &hy)) {
        in.touching = true;
        in.touch_x = (uint16_t) hx;
        in.touch_y = (uint16_t) hy;
    } else if (touch.reportNum > 0 && r[2] > 0 && r[3] > 0) {
        int x = touch.report[0].x * VITA_SCREEN_W / 1920, y = touch.report[0].y * VITA_SCREEN_H / 1088;
        if (x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3]) {
            in.touching = true;
            in.touch_x = (uint16_t) ((x - r[0]) * 256 / r[2]);
            in.touch_y = (uint16_t) ((y - r[1]) * 192 / r[3]);
        }
    }
    guest_input_set(&in);
}

static int system_language(void) {
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_GB;
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    switch (lang) {
    case SCE_SYSTEM_PARAM_LANG_FRENCH: return 2;
    case SCE_SYSTEM_PARAM_LANG_GERMAN: return 3;
    case SCE_SYSTEM_PARAM_LANG_ITALIAN: return 4;
    case SCE_SYSTEM_PARAM_LANG_SPANISH: return 5;
    default: return 1;
    }
}

extern GLboolean is_shark_online;
static bool shader_compiler_installed(void) {
    SceIoStat st;
    return sceIoGetstat("ur0:data/libshacccg.suprx", &st) >= 0 ||
           sceIoGetstat("ur0:data/external/libshacccg.suprx", &st) >= 0;
}

static void log_free_memory(void) {
    SceKernelFreeMemorySizeInfo info;
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) >= 0) port_log("Free memory: %d MB", info.size_user / (1024 * 1024));
}

static uint64_t clock_us(void) { return sceKernelGetProcessTimeWide(); }

static int s_main_priority;
void present_async_thread_started(void) {
    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_2);
    int prio = s_main_priority > 0 ? s_main_priority : sceKernelGetThreadCurrentPriority();
    if (prio > 0 && prio + 4 <= 191) sceKernelChangeThreadPriority(0, prio + 4);
}

static volatile uint32_t s_loop_count;
static const char *volatile s_loop_stage = "start";
static int watchdog(SceSize args, void *argp) {
    (void) args, (void) argp;
    uint32_t last_loop = 0, last_staged = 0, still_loop = 0, still_stage = 0;
    for (;;) {
        sceKernelDelayThread(1000000);
        uint32_t loop = s_loop_count, staged = g_present_stats.staged;
        still_loop = loop == last_loop ? still_loop + 1 : 0;
        bool busy = strcmp((const char *) g_present_stage, "idle") != 0;
        still_stage = busy && staged == last_staged ? still_stage + 1 : 0;
        if (still_loop == 3) {
            port_log("Stuck: the screen has not updated for 3 seconds (at %s, 2D step %s, frame %llu)",
                     (const char *) s_loop_stage, (const char *) g_present_stage,
                     (unsigned long long) guest_frame_count());
        }
        if (still_stage == 3) {
            port_log("Stuck: the 2D step has been at %s for 3 seconds (frame %llu)",
                     (const char *) g_present_stage, (unsigned long long) guest_frame_count());
        }
        last_loop = loop;
        last_staged = staged;
    }
    return 0;
}

int main(void) {
    s_main_thread = sceKernelGetThreadId();
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);
    SceCommonDialogConfigParam dialog;
    sceCommonDialogConfigParamInit(&dialog);
    sceCommonDialogSetConfigParam(&dialog);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);


    port_log("Build: %s %s", __DATE__, __TIME__);
    bool memory_ok = port_mem_init();
    char mod[VITA_MOD_NAME], mod_rom[256], save_path[256] = VITA_SAVE_PATH;
    vita_menu_mod_setting(mod, sizeof(mod));
    bool rom_ok = memory_ok && guest_load(), base_loaded = false;
    if (rom_ok && vita_mods_pending()) {
        rom_ok = base_loaded = port_rom_open(VITA_ROM_PATH);
        if (rom_ok) vita_mods_install();
    }
    if (rom_ok && vita_mod_paths(mod, mod_rom, save_path, sizeof(save_path)) && port_rom_open(mod_rom)) {
        port_log("Mod: %s", mod);
    } else {
        mod[0] = 0;
        snprintf(save_path, sizeof(save_path), "%s", VITA_SAVE_PATH);
        rom_ok = rom_ok && (base_loaded || port_rom_open(VITA_ROM_PATH));
    }
    vita_menu_set_active_mod(mod);

    vglSetupRuntimeShaderCompiler(SHARK_OPT_UNSAFE, SHARK_ENABLE, SHARK_DISABLE, SHARK_DISABLE);
    vglUseCachedMem(GL_TRUE);
    SceKernelFreeMemorySizeInfo mem;
    mem.size = sizeof(mem);
    sceKernelGetFreeMemorySize(&mem);
    int threshold = mem.size_user > VGL_RAM_POOL + 8 * 1024 * 1024 ? mem.size_user - VGL_RAM_POOL : 8 * 1024 * 1024;
    if (vglInitWithCustomThreshold(0, VITA_SCREEN_W, VITA_SCREEN_H, threshold, 0, 0,
                                   CDIALOG_MEM_SIZE, SCE_GXM_MULTISAMPLE_NONE)) {
        port_log("Graphics: the display resolution was lowered");
    }
    s_gl_ready = true;
    log_free_memory();

    if (!memory_ok) {
        fail("The game's memory could not be set up (not enough free memory).");
    }
    if (!rom_ok) {
        fail("The game could not be loaded.\n\nPut your own dump of Mario Kart DS (Europe, AMCP) at\n" VITA_ROM_PATH);
    }
    if (!shader_compiler_installed()) {
        fail("The shader compiler is missing.\n\nExtract libshacccg.suprx with ShaRKBR33D (it puts it in "
             "ur0:data/libshacccg.suprx), then start the game again.");
    }
    port_save_set_path(save_path);
    guest_set_language(vita_language_screen(system_language()));
    port_log("Game loaded");
    present_init();
    g_present_clock_us = clock_us;
    port_log("Graphics ready");
    float aspect = (float) VITA_SCREEN_W / VITA_SCREEN_H;
    present_set_wide(aspect);
    guest_set_aspect((int32_t) (aspect * 4096.0f + 0.5f));
    guest_set_vblank_rate(59826);
    s_main_priority = sceKernelGetThreadCurrentPriority();
    { extern int g_vita_main_priority; g_vita_main_priority = s_main_priority; }
    g_frame_ready_hook = present_async_frame_ready;
    present_async_start(port_frame_take, port_frame_release, 2);
    vita_menu_load();
    g_gx3d_async = true;
    g_gx3d_gpu_vertices = gl3d_gpu_vertices_supported();
    {
        extern bool g_gx3d_gpu_lists;
        g_gx3d_gpu_lists = gl3d_meshes_supported();
        if (!g_gx3d_gpu_lists) port_log("Graphics: 3D models drawn without GPU meshes (slower)");
    }
    g_gx3d_keep_hidden = false;
    if (!vita_probe_core3()) {
        fail("This port needs the CapUnlocker plugin (it lets the game use the Vita's fourth CPU core).\n\n"
             "Put CapUnlocker.skprx in ur0:tai/, add the line\n  ur0:tai/CapUnlocker.skprx\nunder *KERNEL in "
             "ur0:tai/config.txt, reboot the Vita and run HENkaku again, then start the game.");
    }
    port_log("CapUnlocker: using the fourth CPU core");
    {
        float hz = 0.0f;
        if (sceDisplayGetRefreshRate(&hz) >= 0 && hz > 50.0f && hz < 70.0f) g_vblank_rate_mhz = (uint32_t) (hz * 1000.0f);
        else g_vblank_rate_mhz = 59940;
        port_log("Screen: %.2f Hz", g_vblank_rate_mhz / 1000.0);
    }
    guest_start_game();
    vita_audio_start();
    hapi_inet_state();
    apply_screens();

    sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_1);
    s_main_priority = sceKernelGetThreadCurrentPriority();
    SceUID wd = sceKernelCreateThread("mkds_watchdog", watchdog, 0x40, 0x4000, 0, SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (wd >= 0) sceKernelStartThread(wd, 0, NULL);
    SceUID pc = sceKernelCreateThread("mkds_pacing", pacing_thread, 0x48, 0x2000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    if (pc >= 0) sceKernelStartThread(pc, 0, NULL);
    for (;;) {
        if (atomic_load(&s_fatal)) {
            vita_fatal_screen();
            guest_save_sync();
            hapi_log_flush();
            sceKernelExitProcess(0);
        }
        s_loop_count++;
        uint64_t now_us = sceKernelGetProcessTimeWide();
        static uint64_t s_input_us;
        if (now_us - s_input_us >= 15000) {
            s_input_us = now_us;
            s_loop_stage = "input";
            read_input();
        }
        s_loop_stage = "commit";
        bool fresh = present_async_commit();
        if (fresh) {
            if (present_is_race() != s_shown_race) {
                s_swapped = false;
                apply_screens();
            } else if ((s_hud_active && present_hud_usable() != s_hud_shown) || present_is_dual_3d() != s_shown_dual) {
                apply_screens();
            }
        }
        if (fresh || s_redraw) {
            s_redraw = false;
            s_loop_stage = "draw";
            present_draw(s_layout, VITA_SCREEN_W, VITA_SCREEN_H, 1.0f);
            vita_menu_draw();
            if (fresh) {
                uint64_t shot = present_shown_time_ns(), nowns = port_now_ns();
                if (shot && nowns > shot && nowns - shot < 100000000ull) {
                    int64_t lat = (int64_t) (nowns - shot);
                    s_pipeline_ns = (int32_t) (s_pipeline_ns ? s_pipeline_ns + (lat - s_pipeline_ns) / 16 : lat);
                }
            }
            s_loop_stage = "swap";
            vglSwapBuffers(GL_FALSE);
            present_async_swapped();
        } else {
            s_loop_stage = "wait";
            sceKernelDelayThread(1000);
        }
        s_loop_stage = "loop";
        static uint64_t s_save_us;
        if (now_us - s_save_us >= 2000000) {
            s_save_us = now_us;
            guest_save_flush();
        }
    }
    return 0;
}
