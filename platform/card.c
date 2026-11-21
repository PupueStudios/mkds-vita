// Game card: ROM reads come from the ROM image in memory, the backup chip (256 KiB flash for this game) is a file.
#include "port.h"
#include <pthread.h>
#include <stdatomic.h>

#include <stdlib.h>
#include <string.h>

#include "hostapi.h"

#include "types.h"
#include "card_enable_and_lock.h"
#include "card_removal_handling.h"
#include "card_rom_backup_access.h"
#include "reset_interrupts_halt.h"

void port_os_poll(void);

static const uint8_t *g_rom;
static uint32_t g_rom_size;

// ------------------------------------------------------------------------------------------------ ROM
void CARD_Init(void) {
    g_rom = hapi_rom(&g_rom_size);
    // The firmware leaves the ROM header in main memory for the game.
    memcpy(DS_PTR(0x027ffe00), g_rom, 0x160);
    memcpy(&SYSTEM_WORK->mRomHeader, g_rom, sizeof(SYSTEM_WORK->mRomHeader));
    memcpy(HW_CARD_ROM_HEADER, g_rom, 0x160);
}

u32 CARD_IsEnabled(void) { return 1; }
void CARD_CheckEnabled(void) {}
void CARD_Enable(u32 enable) { (void) enable; }
void CARD_LockRom(u16 lockId) { (void) lockId; }
void CARD_UnlockRom(u16 lockId) { (void) lockId; }
int CARD_WaitRomAsync(void) { return 1; }
u32 CARD_IsPulledOut(void) { return 0; }
void CARD_SetPulledOutCallback(CARDPulledOutCallback callback) { (void) callback; }
void CARD_InitPulledOutCallback(void) {}
void CARD_TerminateForPulledOut(void) {}

typedef struct DeferredCall {
    CARDCallback fn;
    void *arg;
} DeferredCall;

static void run_callback(void *p) {
    DeferredCall *call = p;
    CARDCallback fn = call->fn;
    void *arg = call->arg;
    free(call);
    fn(arg);
}

static void complete(CARDCallback callback, void *arg, u32 isAsync) {
    if (!callback) {
        return;
    }
    if (isAsync) {
        DeferredCall *call = malloc(sizeof(*call));
        call->fn = callback;
        call->arg = arg;
        port_os_defer(run_callback, call);
    } else {
        callback(arg);
    }
}

void CARDi_ReadRom(u32 dma, u32 src, u32 dst, u32 len, CARDCallback callback, void *callbackArg, u32 isAsync) {
    (void) dma;
    if ((uint64_t) src + len > g_rom_size) {
        port_log("Game: read past the end of the game file (%08x+%x)", src, len);
        len = src < g_rom_size ? g_rom_size - src : 0;
    }
    memcpy((void *) (uintptr_t) dst, g_rom + src, len);
    complete(callback, callbackArg, isAsync);
    port_os_poll(); // file loading is where the DS takes its VBlank interrupts
}

// ------------------------------------------------------------------------------------------------ backup
#define SAVE_SIZE 0x40000u
static uint8_t s_save[SAVE_SIZE], s_save_copy[SAVE_SIZE];
static bool s_save_loaded;
static u32 s_result;
static void *s_save_lock, *s_save_write_lock, *s_save_wake;
static atomic_uint s_save_gen, s_saved_gen;
static atomic_bool s_save_thread;

static void save_load(void) {
    if (s_save_loaded) return;
    memset(s_save, 0xff, SAVE_SIZE); // erased flash
    hapi_save_read(s_save, SAVE_SIZE);
    s_save_lock = hapi_mutex_new();
    s_save_write_lock = hapi_mutex_new();
    s_save_wake = hapi_sem_new();
    s_save_loaded = true;
}

static void save_changed(void) { atomic_fetch_add(&s_save_gen, 1); }

static bool save_write_now(void) {
    hapi_mutex_lock(s_save_write_lock);
    hapi_mutex_lock(s_save_lock);
    unsigned gen = atomic_load(&s_save_gen);
    memcpy(s_save_copy, s_save, SAVE_SIZE);
    hapi_mutex_unlock(s_save_lock);
    bool ok = gen == atomic_load(&s_saved_gen) || hapi_save_write(s_save_copy, SAVE_SIZE);
    if (ok) atomic_store(&s_saved_gen, gen);
    hapi_mutex_unlock(s_save_write_lock);
    return ok;
}

static void *save_thread(void *arg) {
    (void) arg;
    for (;;) {
        hapi_sem_wait(s_save_wake);
        while (atomic_load(&s_save_gen) != atomic_load(&s_saved_gen)) {
            if (!save_write_now()) {
                hapi_sleep_ns(1000000000ull);
                break;
            }
        }
    }
    return NULL;
}

void port_save_flush(void) {
    if (!s_save_loaded || atomic_load(&s_save_gen) == atomic_load(&s_saved_gen)) return;
    if (!atomic_exchange(&s_save_thread, true)) {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 32 * 1024);
        if (pthread_create(&t, &attr, save_thread, NULL) != 0) atomic_store(&s_save_thread, false);
        pthread_attr_destroy(&attr);
        if (!atomic_load(&s_save_thread)) {
            port_save_sync();
            return;
        }
    }
    hapi_sem_post(s_save_wake);
}

void port_save_sync(void) {
    if (!s_save_loaded) return;
    while (atomic_load(&s_save_gen) != atomic_load(&s_saved_gen)) {
        if (!save_write_now()) return;
    }
}

void CARD_LockBackup(u16 lockId) { (void) lockId; }
void CARD_UnlockBackup(u16 lockId) { (void) lockId; }
u32 CARD_GetResultCode(void) { return s_result; }
int CARD_IdentifyBackup(s32 type) {
    save_load();
    s_result = BACKUP_RESULT_OK;
    return 1;
}
#define BACKUP_PENDING 16
static DeferredCall s_backup_pending[BACKUP_PENDING];
static int s_backup_pending_count;

static void run_backup_pending(void *unused) {
    (void) unused;
    while (s_backup_pending_count) {
        DeferredCall call = s_backup_pending[0];
        for (int i = 1; i < s_backup_pending_count; i++) s_backup_pending[i - 1] = s_backup_pending[i];
        s_backup_pending_count--;
        call.fn(call.arg);
    }
}

static void complete_backup(CARDCallback callback, void *arg, u32 isAsync) {
    if (!callback) return;
    if (!isAsync || s_backup_pending_count == BACKUP_PENDING) {
        callback(arg);
        return;
    }
    s_backup_pending[s_backup_pending_count++] = (DeferredCall) {callback, arg};
    port_os_defer(run_backup_pending, NULL);
}

int CARD_WaitBackupAsync(void) {
    run_backup_pending(NULL);
    return s_result == BACKUP_RESULT_OK;
}
int CARD_TryWaitBackupAsync(void) { return s_backup_pending_count == 0; }

// Request types of the backup stream commands.
enum {
    CARD_REQ_READ_BACKUP = 6,
    CARD_REQ_WRITE_BACKUP = 7,
    CARD_REQ_PROGRAM_BACKUP = 8,
    CARD_REQ_VERIFY_BACKUP = 9,
    CARD_REQ_ERASE_PAGE_BACKUP = 10,
    CARD_REQ_ERASE_SECTOR_BACKUP = 11,
};

int CARDi_RequestStreamCommand(u32 src, u32 dst, u32 len, CARDCallback callback, void *callbackArg, u32 isAsync,
                               u32 reqType, u32 reqRetry, u32 reqMode) {
    (void) reqRetry, (void) reqMode;
    s_result = BACKUP_RESULT_OK;
    switch (reqType) {
    case CARD_REQ_READ_BACKUP:
        if ((uint64_t) src + len > SAVE_SIZE) {
            s_result = BACKUP_RESULT_BAD_ARGUMENT;
            break;
        }
        memcpy((void *) (uintptr_t) dst, s_save + src, len);
        break;
    case CARD_REQ_WRITE_BACKUP:
    case CARD_REQ_PROGRAM_BACKUP:
        if ((uint64_t) dst + len > SAVE_SIZE) {
            s_result = BACKUP_RESULT_BAD_ARGUMENT;
            break;
        }
        hapi_mutex_lock(s_save_lock);
        memcpy(s_save + dst, (const void *) (uintptr_t) src, len);
        hapi_mutex_unlock(s_save_lock);
        save_changed();
        break;
    case CARD_REQ_VERIFY_BACKUP:
        if ((uint64_t) dst + len > SAVE_SIZE || memcmp(s_save + dst, (const void *) (uintptr_t) src, len) != 0) {
            s_result = BACKUP_RESULT_FAILED;
        }
        break;
    case CARD_REQ_ERASE_PAGE_BACKUP:
    case CARD_REQ_ERASE_SECTOR_BACKUP:
        if ((uint64_t) dst + len <= SAVE_SIZE) {
            hapi_mutex_lock(s_save_lock);
            memset(s_save + dst, 0xff, len);
            hapi_mutex_unlock(s_save_lock);
            save_changed();
        }
        break;
    default:
        port_log("Save: unsupported request %u", reqType);
        s_result = BACKUP_RESULT_NOT_SUPPORTED;
        break;
    }
    complete_backup(callback, callbackArg, isAsync);
    return s_result == BACKUP_RESULT_OK;
}
