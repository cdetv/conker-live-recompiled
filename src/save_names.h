/* Save folders named after their saves (save_names.c). */
#pragma once

#include <wchar.h>

/* After xbox_path_init and decensor_init: renames old-layout save folders
 * and starts translating the game's save paths. */
void save_names_init(const char *save_dir);

/* XAPI is turning a save name into its folder name (0x0042E6BB). */
void save_names_seen(const wchar_t *name);
