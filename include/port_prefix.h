/* Force-included in every decomp translation unit of the native port (clang).
 * Maps CodeWarrior intrinsics and extensions onto clang equivalents. */
#pragma once
#define PLATFORM_PORT
/* Every absolute DS address in the game goes through DS_ADDR. Each 16 MiB area of the DS address space (the top byte:
 * 0x01 ITCM, 0x02 main RAM, 0x04 I/O, 0x05 palettes, 0x06 VRAM, 0x07 OAM, 0x08 GBA slot) is wherever the platform put
 * it: g_ds_off[area] is added (all 0 where the DS addresses themselves are reserved: Linux, Android; separate blocks
 * on the PS Vita, port/platform/vita/memory.c). */
extern unsigned long g_ds_off[16];
#define DS_ADDR(address) ((unsigned long) (address) + g_ds_off[((unsigned long) (address) >> 24) & 15])
extern int g_port_aspect; // race camera aspect ratio (platform/frame.c)
// The identity of what the game draws, for frame interpolation (port/platform/gx3d.c, tools/portify.py).
extern unsigned int g_port_draw_id;
#define PORT_DRAW_ID(object) ((unsigned int) (unsigned long) (object))
extern void *port_player_name(void *saved); // the player's name for races and other players (platform/os.c)


/* mwcc intrinsics */
#define __memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define __builtin__clear(p, n) __builtin_memset((p), 0, (n))
#define __clear(p, n) __builtin_memset((p), 0, (n))

/* The game formats strings into the buffer it reads from (snprintf(buf, n, "%s...", buf, ...)), which CodeWarrior's
 * C library allows: these versions format into a temporary buffer first (port/platform/libc.c). */
#define sprintf port_sprintf
#define snprintf port_snprintf
#define vsprintf port_vsprintf
#define vsnprintf port_vsnprintf

/* wchar_t is 16 bits on the DS (-fshort-wchar); the host C library's wide functions use 32 bits. */
#define wcschr port_wcschr
#define wcscpy port_wcscpy
#define wcslen port_wcslen
#define wcsncat port_wcsncat
#define wcsncmp port_wcsncmp
#define wcsncpy port_wcsncpy
#define wmemchr port_wmemchr
#define wmemcpy port_wmemcpy
#define swprintf port_swprintf
#define vswprintf port_vswprintf
#define mbstowcs port_mbstowcs
#define wcstombs port_wcstombs
#define wctomb port_wctomb
