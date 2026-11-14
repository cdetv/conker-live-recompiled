/**
 * Log switches that can change while the game runs (to-do 171): see
 * dev_logs.h. A level is read from its variable on first use, then only the
 * console changes it; any thread reads it.
 */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "dev_logs.h"

static const struct {
    const char *name, *env, *what;
    int max;
} s_logs[DLOG_COUNT] = {
    { "audio", "RECOMP_AUDIO_LOG", "[AUDIO] the game's own sound / music messages, [TUNE] "
      "who starts a tune; 2 adds the sound emitters", 2 },
    { "xact", "RECOMP_XACT_LOG", "[XACT] / [XACTSND] XACT sounds and tracks, who stopped one", 1 },
    { "volume", "RECOMP_VOLUME_LOG", "[VOLUME] each voice's sound channel, cutscene on / off; "
      "2 every volume call", 2 },
    { "char", "RECOMP_CHAR_LOG", "[ANIMREQ] animation requests, [CHAR] moves; 2 adds [CTRAJ] "
      "after an animation change", 2 },
    { "skip", "RECOMP_SKIP_LOG", "[SKIP] cutscene skip: why refused, hold, skip; 2 what each "
      "call read", 2 },
    { "cutevent", "RECOMP_CUTEVENT_LOG", "[CUTEVT] each cutscene event with its record", 1 },
    { "dof", "RECOMP_DOF_LOG", "[DOF] cutscene blur focus per change", 1 },
    { "attach", "RECOMP_ATTACH_LOG", "[ATTACH] / [MOUTH] faces, mouths, held props", 1 },
    { "modern", "RECOMP_MODERN_LOG", "[MODERN] what the Modern layout reads (zone, weapon, B)", 1 },
    { "noclip", "RECOMP_NOCLIP_LOG", "[NOCLIP] noclip's flight", 1 },
    { "menu", "RECOMP_MENU_LOG", "[TEXT] / [MENU] menu texts and item lists, [MENUSPEED]", 1 },
    { "particle", "RECOMP_PARTICLE_LOG", "[PARTICLES] ambient particle effects", 1 },
    { "phys30", "RECOMP_PHYS30_LOG", "[PHYS30] 30 Hz physics sims; 2 adds [P30TIP] the tail tip "
      "every frame", 2 },
    { "shot30", "RECOMP_SHOT30_LOG", "[SHOT30] projectile age at lifetime out", 1 },
    { "cflash", "RECOMP_CUTSCENE_FLASH_LOG", "[CFLASH] cutscene muzzle flashes", 1 },
    { "lightmap", "RECOMP_LIGHTMAP_LOG", "[LIGHTMAP] light map textures per level", 1 },
};

static volatile LONG s_level[DLOG_COUNT];
static volatile LONG s_read[DLOG_COUNT];

int dev_log(int id)
{
    if (id < 0 || id >= DLOG_COUNT)
        return 0;
    if (!s_read[id]) {
        const char *e = getenv(s_logs[id].env);
        int v = 0;
        if (e && *e && *e != '0')
            v = atoi(e) > 1 ? atoi(e) : 1;
        InterlockedCompareExchange(&s_level[id], v, 0);
        InterlockedExchange(&s_read[id], 1);
    }
    return (int)s_level[id];
}

void dev_log_set(int id, int level)
{
    if (id < 0 || id >= DLOG_COUNT)
        return;
    InterlockedExchange(&s_read[id], 1);
    InterlockedExchange(&s_level[id], level);
}

int dev_log_find(const char *name)
{
    for (int i = 0; i < DLOG_COUNT; i++)
        if (!_stricmp(name, s_logs[i].name) || !_stricmp(name, s_logs[i].env))
            return i;
    return -1;
}

const char *dev_log_name(int id)
{
    return id >= 0 && id < DLOG_COUNT ? s_logs[id].name : "";
}

const char *dev_log_env(int id)
{
    return id >= 0 && id < DLOG_COUNT ? s_logs[id].env : "";
}

const char *dev_log_what(int id)
{
    return id >= 0 && id < DLOG_COUNT ? s_logs[id].what : "";
}

int dev_log_max(int id)
{
    return id >= 0 && id < DLOG_COUNT ? s_logs[id].max : 0;
}
