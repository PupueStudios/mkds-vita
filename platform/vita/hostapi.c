#include "port.h"
#include "hostapi.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vita.h"

static SceUID s_log = -1;

static void log_write(const char *line);

void hapi_log(const char *line) {
    static char last[256];
    static unsigned repeats;
    if (strncmp(line, last, sizeof(last) - 1) == 0) {
        repeats++;
        return;
    }
    if (repeats) {
        char note[64];
        snprintf(note, sizeof(note), "(the line above repeated %u more times)", repeats);
        repeats = 0;
        log_write(note);
    }
    snprintf(last, sizeof(last), "%s", line);
    log_write(line);
}

#define LOG_RING (64 * 1024)
static char s_ring[LOG_RING];
static uint32_t s_ring_head, s_ring_tail, s_ring_lost;
static pthread_mutex_t s_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ring_cond = PTHREAD_COND_INITIALIZER;
static bool s_ring_started;

static void log_open(void) {
    if (s_log >= 0) return;
    sceIoMkdir(VITA_DATA_DIR, 0777);
    s_log = sceIoOpen(VITA_DATA_DIR "/mkds.log", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
}

static void log_flush_locked(void) {
    static char chunk[LOG_RING];
    while (s_ring_tail != s_ring_head) {
        uint32_t n = 0;
        while (s_ring_tail != s_ring_head && n < sizeof(chunk)) chunk[n++] = s_ring[s_ring_tail++ % LOG_RING];
        log_open();
        if (s_log >= 0) sceIoWrite(s_log, chunk, n);
    }
}

static void *log_thread(void *arg) {
    (void) arg;
    static char chunk[LOG_RING];
    pthread_mutex_lock(&s_ring_lock);
    for (;;) {
        while (s_ring_tail == s_ring_head) pthread_cond_wait(&s_ring_cond, &s_ring_lock);
        uint32_t n = 0;
        while (s_ring_tail != s_ring_head && n < sizeof(chunk)) chunk[n++] = s_ring[s_ring_tail++ % LOG_RING];
        pthread_mutex_unlock(&s_ring_lock);
        log_open();
        if (s_log >= 0) sceIoWrite(s_log, chunk, n);
        pthread_mutex_lock(&s_ring_lock);
    }
    return NULL;
}

static void log_write(const char *line) {
    vita_log_line(line);
    size_t len = strlen(line);
    pthread_mutex_lock(&s_ring_lock);
    if (!s_ring_started) {
        s_ring_started = true;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 * 1024);
        pthread_t t;
        if (pthread_create(&t, &attr, log_thread, NULL) != 0) s_ring_started = false;
        pthread_attr_destroy(&attr);
    }
    if (s_ring_head - s_ring_tail + len + 1 > LOG_RING) {
        s_ring_lost++;
    } else {
        for (size_t i = 0; i < len; i++) s_ring[s_ring_head++ % LOG_RING] = line[i];
        s_ring[s_ring_head++ % LOG_RING] = '\n';
    }
    if (s_ring_started) pthread_cond_signal(&s_ring_cond);
    else log_flush_locked();
    pthread_mutex_unlock(&s_ring_lock);
}

void hapi_log_flush(void) {
    pthread_mutex_lock(&s_ring_lock);
    log_flush_locked();
    pthread_mutex_unlock(&s_ring_lock);
}

void hapi_abort(void) {
    hapi_log_flush();
    if (s_log >= 0) sceIoClose(s_log);
    vita_fatal_screen();
    sceKernelExitProcess(1);
    for (;;) {
    }
}

uint64_t hapi_now_ns(void) { return (uint64_t) sceKernelGetProcessTimeWide() * 1000ull; }

void hapi_sleep_ns(uint64_t ns) {
    uint64_t us = ns / 1000;
    sceKernelDelayThread(us ? (SceUInt) us : 1);
}

bool g_vita_core3;
static volatile int s_core3_ran;

static int core3_probe(SceSize args, void *argp) {
    (void) args;
    (void) argp;
    s_core3_ran = 1;
    return 0;
}

bool vita_probe_core3(void) {
    SceUID t = sceKernelCreateThread("mkds_core3_probe", core3_probe, 0x60, 0x1000, 0, VITA_CPU_MASK_USER_3, NULL);
    if (t < 0) return g_vita_core3 = false;
    if (sceKernelStartThread(t, 0, NULL) < 0) {
        sceKernelDeleteThread(t);
        return g_vita_core3 = false;
    }
    SceUInt timeout = 200000;
    sceKernelWaitThreadEnd(t, NULL, &timeout);
    bool ran = s_core3_ran != 0;
    if (ran) sceKernelDeleteThread(t);
    return g_vita_core3 = ran;
}

int g_vita_main_priority;
static SceUID s_worker_uid[3];
static int s_worker_prio[3];

void gpu2d_worker_started(int index) {
    static const int masks[3] = {SCE_KERNEL_CPU_MASK_USER_0, VITA_CPU_MASK_USER_3, VITA_CPU_MASK_USER_3};
    sceKernelChangeThreadCpuAffinityMask(0, masks[index >= 0 && index < 3 ? index : 1]);
    int prio = sceKernelGetThreadCurrentPriority();
    int lower = index == 0 ? 4 : 2;
    if (lower < 0) lower = 0;
    if (prio > 0 && prio + lower <= 191) prio += lower;
    sceKernelChangeThreadPriority(0, prio);
    if (index >= 0 && index < 3) {
        s_worker_prio[index] = prio;
        s_worker_uid[index] = sceKernelGetThreadId();
    }
}

void gpu2d_worker_boost(int index, bool on) {
    if (index < 0 || index >= 3 || s_worker_uid[index] <= 0) return;
    sceKernelChangeThreadPriority(s_worker_uid[index], on ? 0x48 : s_worker_prio[index]);
}

void gx3d_worker_started(void) { sceKernelChangeThreadCpuAffinityMask(0, VITA_CPU_MASK_USER_3); }

void hapi_local_time(HostTime *out) {
    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    out->year = t.year;
    out->month = t.month;
    out->day = t.day;
    out->weekday = sceRtcGetDayOfWeek(t.year, t.month, t.day);
    out->hour = t.hour;
    out->minute = t.minute;
    out->second = t.second;
}

void *hapi_mutex_new(void) {
    pthread_mutex_t *m = calloc(1, sizeof(*m));
    pthread_mutex_init(m, NULL);
    return m;
}
void hapi_mutex_lock(void *m) { pthread_mutex_lock(m); }
void hapi_mutex_unlock(void *m) { pthread_mutex_unlock(m); }

void *hapi_sem_new(void) {
    SceUID id = sceKernelCreateSema("mkds_sem", 0, 0, 0x7fffffff, NULL);
    if (id < 0) port_fatal("host: sceKernelCreateSema failed (%08x)", (unsigned) id);
    return (void *) (intptr_t) id;
}
void hapi_sem_free(void *s) { sceKernelDeleteSema((SceUID) (intptr_t) s); }
void hapi_sem_post(void *s) { sceKernelSignalSema((SceUID) (intptr_t) s, 1); }
void hapi_sem_wait(void *s) { sceKernelWaitSema((SceUID) (intptr_t) s, 1, NULL); }

static pthread_key_t s_tls_key;
static pthread_once_t s_tls_once = PTHREAD_ONCE_INIT;
static void tls_init(void) { pthread_key_create(&s_tls_key, NULL); }
void *hapi_tls_get(void) {
    pthread_once(&s_tls_once, tls_init);
    return pthread_getspecific(s_tls_key);
}
void hapi_tls_set(void *value) {
    pthread_once(&s_tls_once, tls_init);
    if (!pthread_getspecific(s_tls_key)) sceKernelChangeThreadCpuAffinityMask(0, SCE_KERNEL_CPU_MASK_USER_0);
    pthread_setspecific(s_tls_key, value);
}

void host_start_guest_thread(void (*fn)(void *), void *arg);
void hapi_thread_start(void (*fn)(void *), void *arg) { host_start_guest_thread(fn, arg); }
void hapi_thread_exit(void) {
    pthread_exit(NULL);
    for (;;) {
    }
}

uint8_t *hapi_vram_banks(void) {
    port_vram_lock();
    port_vram_sync();
    port_vram_unlock();
    return g_vram_banks;
}
void hapi_vram_remap(int bank) { port_vram_remap(bank); }

static const uint8_t *s_rom;
static uint32_t s_rom_size;
static char s_save_path[256];

bool port_rom_open(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        port_log("Game: cannot open %s", path);
        return false;
    }
    SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    uint8_t *p = size >= 0x4000 && size < 0x10000000 ? malloc((size_t) size) : NULL;
    SceOff done = 0;
    while (p && done < size) {
        int n = sceIoRead(fd, p + done, (SceSize) (size - done > 0x100000 ? 0x100000 : size - done));
        if (n <= 0) break;
        done += n;
    }
    sceIoClose(fd);
    if (!p || done != size) {
        port_log("Game: cannot read %s", path);
        free(p);
        return false;
    }
    if (memcmp(p + 0x0c, "AMCP", 4) != 0) {
        port_log("Game: this is %.4s, the European version (AMCP) is needed", (const char *) p + 0x0c);
        free(p);
        return false;
    }
    s_rom = p;
    s_rom_size = (uint32_t) size;
    return true;
}

const uint8_t *hapi_rom(uint32_t *size) {
    *size = s_rom_size;
    return s_rom;
}

void port_save_set_path(const char *path) { snprintf(s_save_path, sizeof(s_save_path), "%s", path); }

uint32_t hapi_save_read(void *data, uint32_t size) {
    SceUID fd = sceIoOpen(s_save_path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        port_log("Save: none yet, a new one will be created");
        return 0;
    }
    int n = sceIoRead(fd, data, size);
    sceIoClose(fd);
    port_log(n == (int) size ? "Save loaded" : "Save: the file is incomplete (%d bytes)", n);
    return n > 0 ? (uint32_t) n : 0;
}

bool hapi_save_write(const void *data, uint32_t size) {
    if (!s_save_path[0]) return false;
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s_save_path);
    SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        port_log("Save: cannot write %s", tmp);
        return false;
    }
    int n = sceIoWrite(fd, data, size);
    sceIoClose(fd);
    if (n != (int) size) return false;
    sceIoRemove(s_save_path);
    return sceIoRename(tmp, s_save_path) >= 0;
}

#define NET_BEACON_PORT 23535

static int s_sock = -1, s_beacon_sock = -1;
static bool s_net_ready;

static bool net_init(void) {
    if (s_net_ready) return true;
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_NET) < 0) return false;
    static uint8_t memory[1024 * 1024];
    SceNetInitParam param = {memory, sizeof(memory), 0};
    int r = sceNetInit(&param);
    if (r < 0 && (unsigned) r != 0x80410110u) {
        port_log("Wireless: cannot start the network (%08x)", (unsigned) r);
        return false;
    }
    sceNetCtlInit();
    s_net_ready = true;
    return true;
}

static int udp_socket(uint16_t port) {
    int fd = sceNetSocket("mkds", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &one, sizeof(one));
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR, &one, sizeof(one));
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof(one));
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_addr.s_addr = 0;
    a.sin_port = sceNetHtons(port);
    if (sceNetBind(fd, (const SceNetSockaddr *) &a, sizeof(a)) < 0) {
        port_log("Wireless: port %u is in use", port);
        sceNetSocketClose(fd);
        return -1;
    }
    return fd;
}

bool hapi_net_open(void) {
    if (!net_init()) {
        port_log("Wireless: not connected to a Wi-Fi network");
        return false;
    }
    if (s_sock >= 0) return true;
    s_sock = udp_socket(0);
    s_beacon_sock = udp_socket(NET_BEACON_PORT);
    if (s_sock < 0) {
        port_log("Wireless: cannot open a connection");
        return false;
    }
    port_log(s_beacon_sock < 0 ? "Wireless ready (other games cannot be found)" : "Wireless ready");
    return true;
}

void hapi_net_close(void) {
    if (s_sock >= 0) sceNetSocketClose(s_sock);
    if (s_beacon_sock >= 0) sceNetSocketClose(s_beacon_sock);
    s_sock = s_beacon_sock = -1;
}

void hapi_net_mac(uint8_t *mac) {
    static uint8_t s_mac[6];
    static bool s_have;
    if (!s_have) {
        char path[300];
        snprintf(path, sizeof(path), "%s.mac", s_save_path[0] ? s_save_path : VITA_DATA_DIR "/mkds");
        SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
        if (fd < 0 || sceIoRead(fd, s_mac, 6) != 6) {
            uint8_t r[3];
            sceKernelGetRandomNumber(r, sizeof(r));
            s_mac[0] = 0x00, s_mac[1] = 0x09, s_mac[2] = 0xbf;
            s_mac[3] = r[0], s_mac[4] = r[1], s_mac[5] = r[2];
            if (fd >= 0) sceIoClose(fd);
            fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
            if (fd >= 0) sceIoWrite(fd, s_mac, 6);
        }
        if (fd >= 0) sceIoClose(fd);
        s_have = true;
    }
    memcpy(mac, s_mac, 6);
}

static void send_to(uint32_t ip, uint16_t port, const void *data, uint32_t size) {
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof(a));
    a.sin_len = sizeof(a);
    a.sin_family = SCE_NET_AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = sceNetHtons(port);
    sceNetSendto(s_sock, data, size, 0, (const SceNetSockaddr *) &a, sizeof(a));
}

void hapi_net_send(const HostNetAddr *to, const void *data, uint32_t size) {
    if (s_sock < 0) return;
    if (to) {
        send_to(to->ip, (uint16_t) to->port, data, size);
        return;
    }
    SceNetCtlInfo info;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) >= 0) {
        SceNetCtlInfo mask;
        uint32_t ip = 0, netmask = 0;
        sceNetInetPton(SCE_NET_AF_INET, info.ip_address, &ip);
        if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &mask) >= 0) {
            sceNetInetPton(SCE_NET_AF_INET, mask.netmask, &netmask);
        }
        if (ip && netmask) send_to(ip | ~netmask, NET_BEACON_PORT, data, size);
    }
    send_to(0xffffffffu, NET_BEACON_PORT, data, size);
}

int32_t hapi_net_recv(void *data, uint32_t capacity, HostNetAddr *from) {
    int socks[2] = {s_sock, s_beacon_sock};
    for (int i = 0; i < 2; i++) {
        if (socks[i] < 0) continue;
        SceNetSockaddrIn a;
        unsigned int len = sizeof(a);
        int n = sceNetRecvfrom(socks[i], data, capacity, 0, (SceNetSockaddr *) &a, &len);
        if (n >= 0) {
            if (from) {
                from->ip = a.sin_addr.s_addr;
                from->port = sceNetNtohs(a.sin_port);
            }
            return n;
        }
    }
    return -1;
}
