#include "utils/net_compat.h"

#include <cstring>

namespace baichuan::net {

#ifdef _WIN32

std::string error_string(int err) {
    char* buf = nullptr;
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(err),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string msg = (n && buf) ? std::string(buf, n) : ("error " + std::to_string(err));
    if (buf) LocalFree(buf);
    // Trim trailing CR/LF that FormatMessage appends
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r')) msg.pop_back();
    return msg;
}

bool global_init() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void global_cleanup() {
    WSACleanup();
}

// No socketpair() on Windows: emulate over a 127.0.0.1 loopback connection.
int make_socketpair(socket_t sv[2]) {
    sv[0] = kInvalidSocket;
    sv[1] = kInvalidSocket;

    socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listener == kInvalidSocket) return -1;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // let the OS pick a free port

    int rc = -1;
    socket_t client = kInvalidSocket;
    socket_t server = kInvalidSocket;
    do {
        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) break;
        int len = sizeof(addr);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) break;
        if (::listen(listener, 1) != 0) break;

        client = ::socket(AF_INET, SOCK_STREAM, 0);
        if (client == kInvalidSocket) break;
        if (::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) break;

        server = ::accept(listener, nullptr, nullptr);
        if (server == kInvalidSocket) break;

        sv[0] = server;
        sv[1] = client;
        client = kInvalidSocket;  // ownership transferred
        rc = 0;
    } while (false);

    if (client != kInvalidSocket) close_socket(client);
    close_socket(listener);
    return rc;
}

#else  // POSIX

std::string error_string(int err) {
    return std::strerror(err);
}

bool global_init()   { return true; }
void global_cleanup() {}

int make_socketpair(socket_t sv[2]) {
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}

#endif

} // namespace baichuan::net
