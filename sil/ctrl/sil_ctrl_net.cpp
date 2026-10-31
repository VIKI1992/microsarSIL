// sil_ctrl_net.cpp - TCP side of the control server. Kept free of AUTOSAR headers: windows.h declares
// SetEvent(HANDLE) which clashes with the OS API SetEvent(TaskType, EventMaskType) in C++.
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sil_socket_t;
#define SIL_INVALID_SOCKET INVALID_SOCKET
#define sil_closesocket closesocket
#define SIL_SHUT_RDWR SD_BOTH
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int sil_socket_t;
#define SIL_INVALID_SOCKET (-1)
#define sil_closesocket close
#define SIL_SHUT_RDWR SHUT_RDWR
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "sil_ctrl.h"
#include "sil_ctrl_internal.h"
#include "sil_port.h"

namespace {

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<sil_socket_t> g_listen{SIL_INVALID_SOCKET};
std::atomic<sil_socket_t> g_client{SIL_INVALID_SOCKET};

bool send_all(sil_socket_t s, const std::string &data) {
    size_t off = 0;
    while (off < data.size()) {
#if defined(MSG_NOSIGNAL)
        int n = static_cast<int>(send(s, data.data() + off, data.size() - off, MSG_NOSIGNAL));
#else
        int n = send(s, data.data() + off, static_cast<int>(data.size() - off), 0);
#endif
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

void serve_client(sil_socket_t c) {
    std::string pending;
    char buf[4096];
    while (!g_stop.load()) {
        int n = recv(c, buf, sizeof buf, 0);
        if (n <= 0) break;
        pending.append(buf, static_cast<size_t>(n));
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (line.empty() || line == "\r") continue;
            std::string reply = silctrl_handle_line(line);
            reply += "\n";
            if (!send_all(c, reply)) return;
            silctrl_after_reply();
        }
    }
}

void server_thread() {
    while (!g_stop.load()) {
        sockaddr_in from{};
#if defined(_WIN32)
        int len = sizeof from;
#else
        socklen_t len = sizeof from;
#endif
        sil_socket_t c = accept(g_listen.load(), reinterpret_cast<sockaddr *>(&from), &len);
        if (c == SIL_INVALID_SOCKET) {
            if (g_stop.load()) break;
            sil_sleep_ns(10000000u);
            continue;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&one), sizeof one);
        g_client.store(c);
        serve_client(c);
        g_client.store(SIL_INVALID_SOCKET);
        sil_closesocket(c);
    }
}

} // namespace

extern "C" int SilCtrl_Start(const char *host, unsigned port, unsigned *boundPort) {
#if defined(_WIN32)
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
#endif
    if (host == nullptr || host[0] == '\0') host = "127.0.0.1";
    sil_socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == SIL_INVALID_SOCKET) return -1;
#if !defined(_WIN32)
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&one), sizeof one);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(host);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || listen(s, 4) != 0) {
        sil_closesocket(s);
        sil_log("control: cannot listen on %s:%u", host, port);
        return -1;
    }
    sockaddr_in bound{};
#if defined(_WIN32)
    int blen = sizeof bound;
#else
    socklen_t blen = sizeof bound;
#endif
    getsockname(s, reinterpret_cast<sockaddr *>(&bound), &blen);
    unsigned p = ntohs(bound.sin_port);
    if (boundPort != nullptr) *boundPort = p;
    g_stop.store(false);
    g_listen.store(s);
    silctrl_on_start();
    g_thread = std::thread(server_thread);
    sil_log("control: listening on %s:%u", host, p);
    return 0;
}

extern "C" void SilCtrl_Stop(void) {
    if (!g_thread.joinable()) return;
    g_stop.store(true);
    sil_socket_t c = g_client.exchange(SIL_INVALID_SOCKET);
    if (c != SIL_INVALID_SOCKET) shutdown(c, SIL_SHUT_RDWR);
    sil_socket_t l = g_listen.exchange(SIL_INVALID_SOCKET);
    if (l != SIL_INVALID_SOCKET) {
        shutdown(l, SIL_SHUT_RDWR);
        sil_closesocket(l);
    }
    g_thread.join();
}
