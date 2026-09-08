#pragma once

// Cross-platform socket compatibility layer.
//
// Provides a single API over BSD sockets (POSIX) and Winsock2 (Windows) so the
// networking call sites contain no per-platform #ifdefs. Include this header
// BEFORE any GTK/<windows.h> header. Defining WIN32_LEAN_AND_MEAN (done here and
// via CMake) keeps <windows.h> from pulling in the old <winsock.h>, so the
// winsock.h/winsock2.h ordering clash never arises.

#include <string>

#ifdef _WIN32

  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  // <windows.h> must come first and explicitly: under WIN32_LEAN_AND_MEAN,
  // <winsock2.h> does NOT pull it in, which would leave FormatMessageA/LocalFree
  // undefined. WIN32_LEAN_AND_MEAN also keeps <windows.h> from including the old
  // <winsock.h>, so the winsock.h/winsock2.h clash cannot arise.
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>

  // POSIX spellings that Winsock lacks
  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0        // Windows has no SIGPIPE
  #endif

  namespace baichuan::net {
    using socket_t = SOCKET;
    inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    using pollfd_t = WSAPOLLFD;

    inline int  close_socket(socket_t s)          { return ::closesocket(s); }
    inline int  last_error()                      { return ::WSAGetLastError(); }
    inline bool would_block(int e)                { return e == WSAEWOULDBLOCK; }
    inline bool in_progress(int e)                { return e == WSAEWOULDBLOCK; }  // non-blocking connect
    inline int  set_nonblocking(socket_t s, bool nb) {
        u_long mode = nb ? 1u : 0u;
        return ::ioctlsocket(s, FIONBIO, &mode);
    }
    inline int  poll_sockets(pollfd_t* fds, unsigned long n, int timeout_ms) {
        return ::WSAPoll(fds, n, timeout_ms);
    }
    inline int  shutdown_both(socket_t s)         { return ::shutdown(s, SD_BOTH); }
    inline int  set_recv_timeout(socket_t s, int ms) {
        DWORD tv = static_cast<DWORD>(ms);
        return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                            reinterpret_cast<const char*>(&tv), sizeof(tv));
    }
    inline int  set_send_timeout(socket_t s, int ms) {
        DWORD tv = static_cast<DWORD>(ms);
        return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO,
                            reinterpret_cast<const char*>(&tv), sizeof(tv));
    }

    // Wait for a non-blocking connect() to settle.
    // Returns 1 if the socket became writable (connect resolved -- check SO_ERROR
    // for success/failure), 0 on timeout, -1 on error.
    //
    // WSAPoll must NOT be used here: it does not report a failed connection
    // attempt (documented Microsoft defect), so a refused connect would simply
    // hang until the caller's timeout. select() reports the failure in exceptfds.
    inline int wait_connect(socket_t s, int timeout_ms) {
        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_SET(s, &wfds);
        FD_ZERO(&efds); FD_SET(s, &efds);
        timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int rc = ::select(0, nullptr, &wfds, &efds, &tv);
        if (rc <= 0) return rc;                 // 0 = timeout, SOCKET_ERROR = -1
        return 1;                               // writable or failed; SO_ERROR decides
    }
  } // namespace baichuan::net

#else  // POSIX

  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <sys/un.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <cerrno>

  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif

  namespace baichuan::net {
    using socket_t = int;
    inline constexpr socket_t kInvalidSocket = -1;
    using pollfd_t = struct pollfd;

    inline int  close_socket(socket_t s)          { return ::close(s); }
    inline int  last_error()                      { return errno; }
    inline bool would_block(int e)                { return e == EWOULDBLOCK || e == EAGAIN; }
    inline bool in_progress(int e)                { return e == EINPROGRESS; }
    inline int  set_nonblocking(socket_t s, bool nb) {
        int fl = ::fcntl(s, F_GETFL, 0);
        if (fl < 0) return -1;
        return ::fcntl(s, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
    }
    inline int  poll_sockets(pollfd_t* fds, nfds_t n, int timeout_ms) {
        return ::poll(fds, n, timeout_ms);
    }
    inline int  shutdown_both(socket_t s)         { return ::shutdown(s, SHUT_RDWR); }
    inline int  set_recv_timeout(socket_t s, int ms) {
        struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
        return ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    inline int  set_send_timeout(socket_t s, int ms) {
        struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
        return ::setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    // Wait for a non-blocking connect() to settle.
    // Returns 1 if the socket became writable (connect resolved -- check SO_ERROR
    // for success/failure), 0 on timeout, -1 on error.
    inline int wait_connect(socket_t s, int timeout_ms) {
        pollfd_t pfd;
        pfd.fd = s;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        int rc = ::poll(&pfd, 1, timeout_ms);
        return rc <= 0 ? rc : 1;
    }
  } // namespace baichuan::net

#endif

namespace baichuan::net {

// Human-readable message for a socket error code from last_error().
std::string error_string(int err);

// One-time process startup/shutdown (WSAStartup/WSACleanup on Windows; no-op on POSIX).
bool global_init();
void global_cleanup();

// Create a connected pair of stream sockets (used as a shutdown-wakeup channel).
// POSIX: socketpair(AF_UNIX). Windows: 127.0.0.1 loopback emulation.
// Returns 0 on success and fills sv[0]/sv[1]; -1 on failure.
int make_socketpair(socket_t sv[2]);

// RAII helper: net::Init guard{}; at top of main().
struct Init {
    bool ok;
    Init()  : ok(global_init()) {}
    ~Init() { global_cleanup(); }
};

} // namespace baichuan::net
