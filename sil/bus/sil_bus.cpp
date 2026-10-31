// sil_bus.cpp - null, in-process loop and UDP multicast CAN bus backends.
#include "sil_bus.h"
#include "sil_port.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sil_socket_t;
#define SIL_INVALID_SOCKET INVALID_SOCKET
#define sil_closesocket closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int sil_socket_t;
#define SIL_INVALID_SOCKET (-1)
#define sil_closesocket close
#endif

namespace {

enum class Kind { Null, Loop, Udp };

struct LoopLink;

} // namespace

struct SilBus {
    Kind kind = Kind::Null;
    SilBus_RxFn rx = nullptr;
    void *user = nullptr;
    uint32_t nodeId = 0;
    std::string spec;
    // loop
    std::shared_ptr<LoopLink> link;
    // udp
    sil_socket_t sock = SIL_INVALID_SOCKET;
    sockaddr_in dest{};
    std::thread rxThread;
    std::atomic<bool> stop{false};
};

namespace {

struct LoopLink {
    std::mutex m;
    std::vector<SilBus *> endpoints;
};

std::mutex g_linksMutex;
std::map<std::string, std::shared_ptr<LoopLink>> g_links;

uint32_t make_node_id() {
    static std::atomic<uint32_t> counter{0};
    auto t = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    uint32_t id = (t * 2654435761u) ^ (counter.fetch_add(1) * 40503u) ^ static_cast<uint32_t>(sil_thread_self_id());
    return id == 0u ? 1u : id;
}

void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

#if defined(_WIN32)
void winsock_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}
#endif

bool parse_udp(const std::string &spec, std::string &group, uint16_t &port) {
    // "udp:239.0.0.1:52000" | "udp://239.0.0.1:52000" | "udp" | "udp:52000"
    std::string s = spec.substr(3);
    if (s.rfind("://", 0) == 0) s = s.substr(3);
    else if (!s.empty() && s[0] == ':') s = s.substr(1);
    group = "239.0.0.1";
    port = 52000;
    if (s.empty()) return true;
    auto colon = s.rfind(':');
    if (colon == std::string::npos) {
        if (s.find('.') == std::string::npos) port = static_cast<uint16_t>(std::atoi(s.c_str()));
        else group = s;
    } else {
        group = s.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(s.c_str() + colon + 1));
    }
    return true;
}

void udp_rx_thread(SilBus *bus) {
    uint8_t buf[256];
    while (!bus->stop.load()) {
        sockaddr_in from{};
#if defined(_WIN32)
        int fromLen = sizeof(from);
        int n = recvfrom(bus->sock, reinterpret_cast<char *>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
#else
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(bus->sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fromLen);
#endif
        if (n <= 0) {
            if (bus->stop.load()) break;
            sil_sleep_ns(1000000u);
            continue;
        }
        SilBus_Frame f;
        uint32_t node;
        if (SilBus_Decode(buf, static_cast<unsigned>(n), &f, &node) != 0) continue;
        if (node == bus->nodeId) continue; // own frame (multicast loopback)
        if (bus->rx != nullptr) bus->rx(bus->user, &f);
    }
}

bool open_udp(SilBus *bus) {
#if defined(_WIN32)
    winsock_init();
#endif
    std::string group;
    uint16_t port;
    parse_udp(bus->spec, group, port);
    bus->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (bus->sock == SIL_INVALID_SOCKET) return false;
    int one = 1;
    setsockopt(bus->sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof(one));
#if defined(SO_REUSEPORT)
    setsockopt(bus->sock, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char *>(&one), sizeof(one));
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (bind(bus->sock, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        sil_closesocket(bus->sock);
        bus->sock = SIL_INVALID_SOCKET;
        return false;
    }
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(group.c_str());
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    setsockopt(bus->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char *>(&mreq), sizeof(mreq));
    unsigned char loop = 1, ttl = 1;
    setsockopt(bus->sock, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char *>(&loop), sizeof(loop));
    setsockopt(bus->sock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char *>(&ttl), sizeof(ttl));
    bus->dest.sin_family = AF_INET;
    bus->dest.sin_addr.s_addr = inet_addr(group.c_str());
    bus->dest.sin_port = htons(port);
    bus->rxThread = std::thread(udp_rx_thread, bus);
    return true;
}

} // namespace

extern "C" {

unsigned SilBus_Encode(const SilBus_Frame *frame, uint32_t nodeId, uint8_t *buf, unsigned bufLen) {
    if (frame == nullptr || buf == nullptr || frame->len > SILBUS_MAX_DATA || bufLen < 28u + frame->len) return 0;
    std::memcpy(buf, "SILCAN1", 8);
    put32(buf + 8, nodeId);
    put32(buf + 12, frame->id);
    buf[16] = frame->flags;
    buf[17] = frame->len;
    buf[18] = 0;
    buf[19] = 0;
    put64(buf + 20, frame->t_ns);
    std::memcpy(buf + 28, frame->data, frame->len);
    return 28u + frame->len;
}

int SilBus_Decode(const uint8_t *buf, unsigned len, SilBus_Frame *frame, uint32_t *nodeId) {
    if (buf == nullptr || frame == nullptr || len < 28u || std::memcmp(buf, "SILCAN1", 8) != 0) return -1;
    std::memset(frame, 0, sizeof(*frame));
    if (nodeId != nullptr) *nodeId = get32(buf + 8);
    frame->id = get32(buf + 12);
    frame->flags = buf[16];
    frame->len = buf[17];
    frame->t_ns = get64(buf + 20);
    if (frame->len > SILBUS_MAX_DATA || len < 28u + frame->len) return -1;
    std::memcpy(frame->data, buf + 28, frame->len);
    return 0;
}

SilBus *SilBus_Open(const char *spec, SilBus_RxFn rx, void *user) {
    auto *bus = new SilBus();
    bus->rx = rx;
    bus->user = user;
    bus->nodeId = make_node_id();
    bus->spec = (spec != nullptr) ? spec : "null";
    if (bus->spec.rfind("loop:", 0) == 0 || bus->spec == "loop") {
        std::string name = bus->spec.size() > 5 ? bus->spec.substr(5) : "default";
        bus->kind = Kind::Loop;
        std::lock_guard<std::mutex> l(g_linksMutex);
        auto &link = g_links[name];
        if (!link) link = std::make_shared<LoopLink>();
        bus->link = link;
        std::lock_guard<std::mutex> l2(link->m);
        link->endpoints.push_back(bus);
    } else if (bus->spec.rfind("udp", 0) == 0) {
        bus->kind = Kind::Udp;
        if (!open_udp(bus)) {
            sil_log("bus: cannot open %s", bus->spec.c_str());
            delete bus;
            return nullptr;
        }
    } else {
        bus->kind = Kind::Null;
    }
    return bus;
}

int SilBus_Send(SilBus *bus, const SilBus_Frame *frame) {
    if (bus == nullptr || frame == nullptr) return -1;
    switch (bus->kind) {
    case Kind::Null:
        return 0;
    case Kind::Loop: {
        std::vector<SilBus *> targets;
        {
            std::lock_guard<std::mutex> l(bus->link->m);
            for (SilBus *e : bus->link->endpoints)
                if (e != bus) targets.push_back(e);
        }
        for (SilBus *e : targets)
            if (e->rx != nullptr) e->rx(e->user, frame);
        return 0;
    }
    case Kind::Udp: {
        uint8_t buf[128];
        unsigned n = SilBus_Encode(frame, bus->nodeId, buf, sizeof(buf));
        if (n == 0) return -1;
        int r = sendto(bus->sock, reinterpret_cast<const char *>(buf), static_cast<int>(n), 0, reinterpret_cast<sockaddr *>(&bus->dest),
                       sizeof(bus->dest));
        return r < 0 ? -1 : 0;
    }
    }
    return -1;
}

void SilBus_Close(SilBus *bus) {
    if (bus == nullptr) return;
    if (bus->kind == Kind::Loop) {
        std::lock_guard<std::mutex> l(bus->link->m);
        auto &v = bus->link->endpoints;
        for (auto it = v.begin(); it != v.end(); ++it)
            if (*it == bus) {
                v.erase(it);
                break;
            }
    } else if (bus->kind == Kind::Udp) {
        bus->stop.store(true);
        if (bus->sock != SIL_INVALID_SOCKET) {
#if defined(_WIN32)
            shutdown(bus->sock, SD_BOTH);
#else
            shutdown(bus->sock, SHUT_RDWR);
#endif
            sil_closesocket(bus->sock);
        }
        if (bus->rxThread.joinable()) bus->rxThread.join();
    }
    delete bus;
}

const char *SilBus_Kind(const SilBus *bus) {
    if (bus == nullptr) return "none";
    switch (bus->kind) {
    case Kind::Loop: return "loop";
    case Kind::Udp: return "udp";
    default: return "null";
    }
}

uint32_t SilBus_NodeId(const SilBus *bus) { return bus != nullptr ? bus->nodeId : 0u; }

} // extern "C"
