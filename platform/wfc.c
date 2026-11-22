#include "port.h"
#include "hostapi.h"

#include <stdio.h>
#include <string.h>

#include "types.h"
#include "thread_scheduler.h"
#include "00/Lib/GameSpy/socket_calls.h"
#include "00/Lib/NitroWiFi/socket_api_prototypes.h"
#include "00/Lib/NitroWiFi/socket_library_startup.h"
#include "00/Lib/NitroWiFi/socket_shutdown_all.h"
#include "00/Lib/NitroWiFi/wireless_connection_manager.h"
#include "00/Lib/NitroWiFi/rssi_link_level.h"
#include "00/Lib/NitroWiFi/wifi_stack_bridge.h"
#include "00/Lib/DWC/access_point_manager.h"
#include "00/Lib/CPS/tcp_ip_stack.h"
#include "00/Lib/CPS/https_enable_flag.h"

#define WFC_DOMAIN "wiimmfi.de"
#define SOCKETS 32
#define HANDLE_BASE 0x100
#define NO_ACCESS_POINT ((s32) 0xffff3c4d)

enum { ST_FREE, ST_OPEN, ST_CONNECTING, ST_CONNECTED, ST_FAILED };

typedef struct Socket {
    int32_t fd, handle;
    uint8_t state, tcp, block;
} Socket;

static Socket s_socks[SOCKETS];
static bool s_online;
static uint32_t s_my_ip;

static u32 swap32(u32 v) { return (v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24); }
static u16 swap16(u16 v) { return (u16) ((v >> 8) | (v << 8)); }

static uint32_t s_serial;

static Socket *sock_of(s32 handle) {
    if (handle < HANDLE_BASE) return NULL;
    Socket *s = &s_socks[(handle - HANDLE_BASE) % SOCKETS];
    if (s->state == ST_FREE || s->handle != handle) return NULL;
    return s;
}

static s32 new_handle(int i) {
    s_socks[i].handle = HANDLE_BASE + (s32) (s_serial++ & 0xffff) * SOCKETS + i;
    return s_socks[i].handle;
}

static void wait_a_moment(void) { OS_Sleep(1); }

static void lookup_name(const char *name, char *out, size_t size) {
    static const char old_domain[] = "nintendowifi.net";
    size_t n = strlen(name), d = sizeof(old_domain) - 1;
    if (n >= d && strcmp(name + n - d, old_domain) == 0 && (n == d || name[n - d - 1] == '.')) {
        snprintf(out, size, "%.*s%s", (int) (n - d), name, WFC_DOMAIN);
    } else {
        snprintf(out, size, "%s", name);
    }
}

static uint32_t resolve(const char *name) {
    char host[256];
    lookup_name(name, host, sizeof(host));
    uint32_t ip = hapi_resolve(host);
    if (ip) port_log("Online: %s is %u.%u.%u.%u", host, ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    else port_log("Online: cannot find %s", host);
    return ip;
}

static bool go_online(void) {
    if (hapi_inet_state() != 1) return false;
    s_my_ip = hapi_inet_ip();
    if (!s_my_ip) return false;
    CPSMyIp = s_my_ip;
    s_online = true;
    return true;
}

static enum { AC_IDLE, AC_WAITING, AC_DONE, AC_FAILED } s_ac;
static uint64_t s_ac_start;

BOOL DWC_AC_Create(DWCACConfig *config) {
    (void) config;
    s_ac = AC_WAITING;
    s_ac_start = hapi_now_ns();
    port_log("Online: connecting");
    return 1;
}

s32 DWC_AC_Process(void) {
    if (s_ac == AC_WAITING) {
        if (go_online()) {
            s_ac = AC_DONE;
            port_log("Online: connected as %u.%u.%u.%u", s_my_ip >> 24, (s_my_ip >> 16) & 255, (s_my_ip >> 8) & 255,
                     s_my_ip & 255);
        } else if (hapi_now_ns() - s_ac_start > 20000000000ull) {
            s_ac = AC_FAILED;
            port_log("Online: the Vita is not connected to the internet");
        }
    }
    if (s_ac == AC_DONE) return 1;
    if (s_ac == AC_FAILED) return -1;
    return 0;
}

u32 DWC_AC_GetStatus(void) {
    switch (s_ac) {
    case AC_DONE: return 5;
    case AC_FAILED: return (u32) NO_ACCESS_POINT;
    case AC_WAITING: return 1;
    default: return 0;
    }
}

BOOL DWC_AC_Destroy(void) {
    s_ac = AC_IDLE;
    return 1;
}

u32 WCM_GetPhase(void) {
    if (s_ac != AC_DONE) return WIFI_LINK_PHASE_NONE;
    return hapi_inet_state() == 1 ? WIFI_LINK_PHASE_CONNECTED : WIFI_LINK_PHASE_FAILED;
}

u32 WCM_GetLinkLevel(void) { return s_ac == AC_DONE ? 3 : 0; }

u8 *WCM_GetApMacAddress(void) {
    static u8 bssid[6] = {0x00, 0x09, 0xbf, 0x56, 0x49, 0x54};
    return bssid;
}

u8 *WCM_GetApEssid(u16 *length) {
    static u8 essid[32] = "PSVITA";
    if (length) *length = 6;
    return essid;
}

s32 SOC_Startup(void *configPointer) {
    (void) configPointer;
    return go_online() ? 0 : -1;
}

s32 SOC_Cleanup(void) {
    for (int i = 0; i < SOCKETS; i++) {
        if (s_socks[i].state != ST_FREE) hapi_sock_close(s_socks[i].fd);
        s_socks[i].state = ST_FREE;
    }
    return 0;
}

s32 SOCL_CalmDown(void) { return 0; }

s32 SOCL_GetHostID(void) { return s_online ? (s32) s_my_ip : 0; }

u32 SOC_GetHostID(void) { return swap32((u32) SOCL_GetHostID()); }

void SOC_SetResolver(u32 *firstDns, u32 *secondDns) { (void) firstDns, (void) secondDns; }

s32 SOC_Socket(s32 protocolFamily, s32 socketType, s32 protocol) {
    (void) protocolFamily, (void) protocol;
    for (int i = 0; i < SOCKETS; i++) {
        if (s_socks[i].state != ST_FREE) continue;
        bool tcp = socketType == 1;
        int32_t fd = hapi_sock_open(tcp);
        if (fd < 0) return -1;
        s_socks[i].fd = fd;
        s_socks[i].state = ST_OPEN;
        s_socks[i].tcp = tcp;
        s_socks[i].block = 1;
        return new_handle(i);
    }
    return -1;
}

s32 SOC_Bind(s32 socket, const void *sockAddr) {
    Socket *s = sock_of(socket);
    const SOCSockAddrIn *addr = sockAddr;
    if (!s) return -0x27;
    return hapi_sock_bind(s->fd, swap16(addr->mPort)) < 0 ? -1 : 0;
}

static s32 finish_connect(Socket *s) {
    int bits = hapi_sock_poll(s->fd);
    if (bits & HAPI_POLL_ERROR) {
        s->state = ST_FAILED;
        return -1;
    }
    if (bits & HAPI_POLL_WRITE) {
        s->state = ST_CONNECTED;
        return 0;
    }
    return -26;
}

s32 SOC_Connect(SOCLSocket *socket, SOCSockAddrIn *sockAddr) {
    Socket *s = sock_of((s32) (intptr_t) socket);
    if (!s) return -0x27;
    uint32_t ip = swap32(sockAddr->mAddr);
    uint16_t port = swap16(sockAddr->mPort);
    if (!s->tcp) {
        s->state = ST_CONNECTED;
        return hapi_sock_connect(s->fd, ip, port) < 0 ? -1 : 0;
    }
    int32_t r = hapi_sock_connect(s->fd, ip, port);
    if (r == 0) {
        s->state = ST_CONNECTED;
        return 0;
    }
    if (r != HAPI_SOCK_CONNECTING) {
        s->state = ST_FAILED;
        return -1;
    }
    s->state = ST_CONNECTING;
    if (!s->block) return -26;
    uint64_t start = hapi_now_ns();
    for (;;) {
        r = finish_connect(s);
        if (r != -26) return r;
        if (hapi_now_ns() - start > 20000000000ull) {
            s->state = ST_FAILED;
            return -1;
        }
        wait_a_moment();
    }
}

static s32 recv_from(s32 handle, void *buffer, s32 length, s32 flags, u32 *ip, u16 *port) {
    Socket *s = sock_of(handle);
    if (!s) return -0x1c;
    if (s->tcp && s->state == ST_CONNECTING && finish_connect(s) == -26) return -6;
    if (s->tcp && s->state != ST_CONNECTED) return -0x38;
    bool block = s->block && !(flags & 4);
    for (;;) {
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int32_t n = hapi_sock_recv(s->fd, buffer, (uint32_t) length, &from_ip, &from_port);
        if (n >= 0) {
            if (ip) *ip = from_ip;
            if (port) *port = from_port;
            return n;
        }
        if (n != HAPI_SOCK_AGAIN) return n == HAPI_SOCK_RESET ? -15 : -1;
        if (!block) return -6;
        wait_a_moment();
    }
}

s32 SOC_Recv(s32 sockObj, void *buffer, s32 length, s32 flags) {
    return recv_from(sockObj, buffer, length, flags, NULL, NULL);
}

s32 SOC_RecvFrom(s32 sockObj, void *buffer, s32 length, s32 flags, void *sockFrom) {
    u32 ip;
    u16 port;
    s32 n = recv_from(sockObj, buffer, length, flags, &ip, &port);
    if (n >= 0 && sockFrom) {
        SOCSockAddrIn *addr = sockFrom;
        addr->mLen = 8;
        addr->mFamily = SOCKET_FAMILY_IPV4;
        addr->mPort = swap16(port);
        addr->mAddr = swap32(ip);
    }
    return n;
}

static s32 send_to(s32 handle, const void *buffer, s32 length, s32 flags, u32 ip, u16 port) {
    Socket *s = sock_of(handle);
    if (!s) return -0x1c;
    if (s->tcp && s->state == ST_CONNECTING && finish_connect(s) == -26) return -6;
    if (s->tcp && s->state != ST_CONNECTED) return -0x38;
    bool block = s->block && !(flags & 4);
    s32 sent = 0;
    while (sent < length) {
        int32_t n = hapi_sock_send(s->fd, (const u8 *) buffer + sent, (uint32_t) (length - sent), ip, port);
        if (n >= 0) {
            sent += n;
            if (!s->tcp) break;
            continue;
        }
        if (n != HAPI_SOCK_AGAIN) return sent ? sent : (n == HAPI_SOCK_RESET ? -15 : -1);
        if (!block) return sent ? sent : -6;
        wait_a_moment();
    }
    return sent;
}

s32 SOC_Send(s32 sockObj, void *buffer, s32 length, s32 flags) { return send_to(sockObj, buffer, length, flags, 0, 0); }

s32 SOC_SendTo(s32 sockObj, void *buffer, s32 length, s32 flags, void *sockTo) {
    SOCSockAddrIn *addr = sockTo;
    if (!addr) return send_to(sockObj, buffer, length, flags, 0, 0);
    return send_to(sockObj, buffer, length, flags, swap32(addr->mAddr), swap16(addr->mPort));
}

s32 SOC_Shutdown(s32 descriptor) {
    Socket *s = sock_of(descriptor);
    if (!s) return -0x1c;
    hapi_sock_shutdown(s->fd);
    return 0;
}

s32 SOC_Close(s32 descriptor) {
    Socket *s = sock_of(descriptor);
    if (!s) return -0x1c;
    hapi_sock_close(s->fd);
    s->state = ST_FREE;
    return 0;
}

SOCHostEnt *SOC_GetHostByName(const char *hostname) {
    static SOCHostEnt entry;
    static u32 address, *list[2];
    static char name[256];
    uint32_t ip = resolve(hostname);
    if (!ip) return NULL;
    snprintf(name, sizeof(name), "%s", hostname);
    address = swap32(ip);
    list[0] = &address;
    list[1] = NULL;
    entry.mName = name;
    entry.mAliases = NULL;
    entry.mAddrType = SOCKET_FAMILY_IPV4;
    entry.mLength = SOCKET_ADDRESS_LENGTH_IPV4;
    entry.mAddrList = list;
    return &entry;
}

s32 SOC_GetSockName(u32 socketHandle, void *sockAddr) {
    Socket *s = sock_of((s32) socketHandle);
    SOCSockAddrIn *addr = sockAddr;
    if (!s) return -0x27;
    addr->mLen = 8;
    addr->mFamily = SOCKET_FAMILY_IPV4;
    addr->mPort = swap16(hapi_sock_port(s->fd));
    addr->mAddr = swap32(s_online ? s_my_ip : 0);
    return 0;
}

s32 SOC_Listen(s32 descriptor) {
    Socket *s = sock_of(descriptor);
    if (!s) return -0x1c;
    return hapi_sock_listen(s->fd) < 0 ? -1 : 0;
}

s32 SOC_Accept(SOCLSocket *socket, SOCSockAddrIn *sockAddr, s32 unusedFlags) {
    (void) unusedFlags;
    Socket *s = sock_of((s32) (intptr_t) socket);
    if (!s) return -0x1c;
    for (;;) {
        uint32_t ip;
        uint16_t port;
        int32_t fd = hapi_sock_accept(s->fd, &ip, &port);
        if (fd >= 0) {
            for (int i = 0; i < SOCKETS; i++) {
                if (s_socks[i].state != ST_FREE) continue;
                s_socks[i].fd = fd;
                s_socks[i].state = ST_CONNECTED;
                s_socks[i].tcp = 1;
                s_socks[i].block = 1;
                if (sockAddr) {
                    sockAddr->mPort = swap16(port);
                    sockAddr->mAddr = swap32(ip);
                }
                return new_handle(i);
            }
            hapi_sock_close(fd);
            return -1;
        }
        if (fd != HAPI_SOCK_AGAIN) return -1;
        if (!s->block) return -6;
        wait_a_moment();
    }
}

s32 SOC_Fcntl(SOCLSocket *socket, s32 command, u32 mode) {
    Socket *s = sock_of((s32) (intptr_t) socket);
    if (!s) return -1;
    switch (command) {
    case SOC_F_GETFL: return s->block ? 0 : SOC_O_NONBLOCK;
    case SOC_F_SETFL: s->block = (mode & SOC_O_NONBLOCK) ? 0 : 1; break;
    }
    return 0;
}

static u32 status_of(s32 handle) {
    Socket *s = sock_of(handle);
    if (!s) return 0x80;
    if (s->tcp && s->state == ST_CONNECTING && finish_connect(s) == -26) return 0;
    if (s->state == ST_FAILED) return 0x20 | 0x40;
    if (s->tcp && s->state != ST_CONNECTED) return 0x40;
    int bits = hapi_sock_poll(s->fd);
    u32 status = 0;
    if (bits & HAPI_POLL_READ) status |= 1;
    if (bits & HAPI_POLL_WRITE || !s->tcp) status |= 8;
    if (bits & HAPI_POLL_ERROR) status |= 0x20;
    if (bits & HAPI_POLL_HUP) status |= 0x40;
    return status;
}

s32 SOC_Poll(void *entries, s32 entryCount, s64 ticksLeft) {
    bool timeout = ticksLeft != -1;
    for (;;) {
        SocketPollEntry *entry = entries;
        s32 ready = 0;
        for (s32 i = 0; i < entryCount; i++, entry++) {
            u32 status = (u32) (entry->mEvents | 0xe0) & status_of((s32) (intptr_t) entry->mFd);
            if (status) ready++;
            entry->mRevents = (u16) status;
        }
        if (ready > 0 || (timeout && ticksLeft <= 0)) return ready;
        wait_a_moment();
        ticksLeft -= 0x20b;
    }
}

typedef struct CpsSocket {
    OSThread *thread;
    int32_t fd;
    uint32_t ip;
    uint16_t port;
    bool closed;
    uint32_t have;
    u8 data[0x1000];
} CpsSocket;

static CpsSocket s_cps[4];
extern OSThread **OSi_CurrentThreadPtr;

static CpsSocket *cps_current(void) {
    OSThread *me = *OSi_CurrentThreadPtr;
    for (int i = 0; i < 4; i++) {
        if (s_cps[i].thread == me) return &s_cps[i];
    }
    return NULL;
}

void CPS_SocRegister(void *socket) {
    (void) socket;
    CpsSocket *c = cps_current();
    for (int i = 0; !c && i < 4; i++) {
        if (!s_cps[i].thread) c = &s_cps[i];
    }
    if (!c) return;
    memset(c, 0, sizeof(*c));
    c->thread = *OSi_CurrentThreadPtr;
    c->fd = -1;
}

void CPS_SocUnRegister(void) {
    CpsSocket *c = cps_current();
    if (c) c->thread = NULL;
}

u32 CPS_Resolve(const char *name) { return resolve(name); }

void CPS_SocUse(void) {}
void CPS_SocRelease(void) {}

void CPS_SetSsl(u8 useHttps) {
    if (useHttps) port_log("Online: a secure connection was asked for, which is not available");
}

void CPS_SocBind(int ownPort, u16 remotePort, int remoteAddress) {
    (void) ownPort;
    CpsSocket *c = cps_current();
    if (!c) return;
    c->ip = (uint32_t) remoteAddress;
    c->port = remotePort;
}

u32 CPS_TcpConnect(void) {
    CpsSocket *c = cps_current();
    if (!c) return 1;
    c->fd = hapi_sock_open(true);
    if (c->fd < 0) return 1;
    int32_t r = hapi_sock_connect(c->fd, c->ip, c->port);
    uint64_t start = hapi_now_ns();
    while (r == HAPI_SOCK_CONNECTING) {
        int bits = hapi_sock_poll(c->fd);
        if (bits & HAPI_POLL_ERROR) break;
        if (bits & HAPI_POLL_WRITE) {
            r = 0;
            break;
        }
        if (hapi_now_ns() - start > 20000000000ull) break;
        wait_a_moment();
    }
    if (r != 0) {
        hapi_sock_close(c->fd);
        c->fd = -1;
        return 1;
    }
    return 0;
}

s32 CPS_SocWrite(const u8 *buffer, s32 length) {
    CpsSocket *c = cps_current();
    if (!c || c->fd < 0) return 0;
    s32 sent = 0;
    while (sent < length) {
        int32_t n = hapi_sock_send(c->fd, buffer + sent, (uint32_t) (length - sent), 0, 0);
        if (n > 0) sent += n;
        else if (n == HAPI_SOCK_AGAIN) wait_a_moment();
        else break;
    }
    return sent;
}

void CPS_SocFlush(void) {}

s32 CPS_SocGetLength(void) {
    CpsSocket *c = cps_current();
    if (!c || c->fd < 0) return -1;
    if (!c->have && !c->closed) {
        int32_t n = hapi_sock_recv(c->fd, c->data, sizeof(c->data), NULL, NULL);
        if (n > 0) c->have = (uint32_t) n;
        else if (n != HAPI_SOCK_AGAIN) c->closed = true;
    }
    if (c->have) return (s32) c->have;
    return c->closed ? -1 : 0;
}

u8 *CPS_SocRead(u32 *length) {
    CpsSocket *c = cps_current();
    if (!c || !c->have) return NULL;
    *length = c->have;
    return c->data;
}

void CPS_SocConsume(u32 length) {
    CpsSocket *c = cps_current();
    if (!c) return;
    if (length >= c->have) {
        c->have = 0;
    } else {
        memmove(c->data, c->data + length, c->have - length);
        c->have -= length;
    }
}

void CPS_TcpShutdown(void) {
    CpsSocket *c = cps_current();
    if (c && c->fd >= 0) hapi_sock_shutdown(c->fd);
}

void CPS_TcpClose(void) {
    CpsSocket *c = cps_current();
    if (c && c->fd >= 0) {
        hapi_sock_close(c->fd);
        c->fd = -1;
    }
}
