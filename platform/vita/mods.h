#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VITA_MODS_DIR VITA_DATA_DIR "/mods"
#define VITA_MOD_NAME 48

int vita_mods_list(char names[][VITA_MOD_NAME], int max);
bool vita_mods_pending(void);
void vita_mods_install(void);
bool vita_mod_paths(const char *name, char *rom, char *save, size_t size);
