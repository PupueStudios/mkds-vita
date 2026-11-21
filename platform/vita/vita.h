#pragma once

#include <stdbool.h>

#define VITA_DATA_DIR "ux0:data/mkds"
#define VITA_ROM_PATH VITA_DATA_DIR "/rom.nds"
#define VITA_SAVE_PATH VITA_DATA_DIR "/mkds.sav"

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

#define VITA_GAME_STACK (512 * 1024)

void vita_log_line(const char *line);
void vita_fatal_screen(void);

int vita_language_screen(int system_language);

#define VITA_CPU_MASK_USER_3 0x00080000
extern bool g_vita_core3;
bool vita_probe_core3(void);

bool vita_audio_start(void);
void vita_audio_pause(bool paused);
