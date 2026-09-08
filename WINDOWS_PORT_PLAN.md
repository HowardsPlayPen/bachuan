# Windows Port Implementation Plan

Port the `baichuan` / `dashboard` applications to Windows, keeping a single
cross-platform codebase. Testing is done in a Windows VM.

## Locked decisions

| Decision | Choice |
|----------|--------|
| Toolchain | **MSYS2 UCRT64** (GCC), CMake + Ninja, pkg-config |
| GUI | **Keep GTK3** (no UI rewrite) |
| Control interface on Windows | **TCP-only** (Unix-domain socket compiled out) |
| Codebase | **Single tree**, thin platform layer + `#ifdef _WIN32` |
| Test target | Run in VM (MSYS2 runtime first; standalone DLL bundle as follow-up) |

## Scope summary

Everything except the networking layer is already portable (FFmpeg, OpenSSL,
libxml2, libjpeg, GTK3/Cairo, `std::thread`/`mutex`/`atomic`, `getopt_long` via
MinGW). The real work is:

1. A small socket-compatibility layer (Winsock2 vs BSD sockets).
2. Adapting the 3 socket-using files to it.
3. TCP-only + cross-platform shutdown wakeup in the command server.
4. Config-path + WSA init + CMake link/flag adjustments.
5. Packaging DLLs + GTK runtime assets for the VM.

`src/video/writer.cpp` (recording) uses only `fopen`/FFmpeg `avio_open` and is
already portable; it is also not part of the dashboard build.

---

## Phase 0 — Environment setup (in the Windows VM, or a Windows dev box)

Install MSYS2, then from the **UCRT64** shell:

```bash
pacman -Syu   # (restart shell if it asks)
pacman -S --needed \
  mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-gtk3 \
  mingw-w64-ucrt-x86_64-cairo \
  mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-openssl \
  mingw-w64-ucrt-x86_64-libxml2 \
  mingw-w64-ucrt-x86_64-libjpeg-turbo
```

**De-risk first:** before touching code, verify a trivial GTK "hello window"
builds and runs in the VM with this toolchain. If GTK shows a window, the hard
part of the port is already proven.

---

## Phase 1 — Socket compatibility layer (new)

Create `src/utils/net_compat.h` (header-only where possible; one `.cpp` for
`WSAStartup` and the Windows socketpair emulation).

Responsibilities:

- Unified types: `net::socket_t` (`int` on POSIX, `SOCKET` on Windows),
  `net::kInvalidSocket`, and `pollfd_t` (aliasing `struct pollfd` / `WSAPOLLFD`
  — identical layout, `POLLIN` works on both).
- Helpers so call sites contain no `#ifdef`:
  - `close_socket(s)` → `close` / `closesocket`
  - `set_nonblocking(s, bool)` → `fcntl(O_NONBLOCK)` / `ioctlsocket(FIONBIO)`
  - `poll_sockets(fds, n, timeout_ms)` → `poll` / `WSAPoll`
  - `last_error()` → `errno` / `WSAGetLastError()`
  - `error_string(e)`, `would_block(e)`, `in_progress(e)` (connect: `EINPROGRESS`
    vs `WSAEWOULDBLOCK`)
  - `set_recv_timeout(s, ms)` / `set_send_timeout(s, ms)` — **critical
    difference:** Windows `SO_RCVTIMEO` takes a `DWORD` of milliseconds, POSIX
    takes `struct timeval`.
  - `shutdown_both(s)` → `SHUT_RDWR` / `SD_BOTH`
  - `make_socketpair(sv[2])` → `::socketpair(AF_UNIX,…)` on POSIX; loopback
    `127.0.0.1` connect-to-self emulation on Windows (replaces the `pipe()`
    self-pipe in the command server).
  - `global_init()` / `global_cleanup()` → `WSAStartup`/`WSACleanup` (no-op on
    POSIX).

**Header-ordering hazard (must handle):** GTK on Windows pulls in `<windows.h>`,
which pulls in the old `<winsock.h>` and clashes with `<winsock2.h>`. Mitigate by
defining `WIN32_LEAN_AND_MEAN` and ensuring `net_compat.h` (→ `<winsock2.h>`,
`<ws2tcpip.h>`) is included **before** any GTK header in the socket TUs. Socket
code and GTK code live in different translation units, so this is easy to keep
separated.

Sketch:

```cpp
// src/utils/net_compat.h
#pragma once
#include <string>
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  namespace baichuan::net {
    using socket_t = SOCKET;
    inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    using pollfd_t = WSAPOLLFD;
    inline int  close_socket(socket_t s)      { return ::closesocket(s); }
    inline int  last_error()                  { return ::WSAGetLastError(); }
    inline bool would_block(int e)            { return e == WSAEWOULDBLOCK; }
    inline bool in_progress(int e)            { return e == WSAEWOULDBLOCK; }
    inline int  set_nonblocking(socket_t s, bool nb){ u_long m = nb; return ::ioctlsocket(s, FIONBIO, &m); }
    inline int  poll_sockets(pollfd_t* f, unsigned long n, int t){ return ::WSAPoll(f, n, t); }
    std::string error_string(int e);
    bool global_init();  void global_cleanup();
    int  make_socketpair(socket_t sv[2]);
  }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <sys/un.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <cstring>
  #include <cerrno>
  namespace baichuan::net {
    using socket_t = int;
    inline constexpr socket_t kInvalidSocket = -1;
    using pollfd_t = struct pollfd;
    inline int  close_socket(socket_t s)      { return ::close(s); }
    inline int  last_error()                  { return errno; }
    inline bool would_block(int e)            { return e == EWOULDBLOCK || e == EAGAIN; }
    inline bool in_progress(int e)            { return e == EINPROGRESS; }
    inline int  set_nonblocking(socket_t s, bool nb){ int fl = ::fcntl(s,F_GETFL,0); return ::fcntl(s,F_SETFL, nb? (fl|O_NONBLOCK):(fl&~O_NONBLOCK)); }
    inline int  poll_sockets(pollfd_t* f, nfds_t n, int t){ return ::poll(f, n, t); }
    inline std::string error_string(int e)    { return std::strerror(e); }
    inline bool global_init()                 { return true; }
    inline void global_cleanup()              {}
    int  make_socketpair(socket_t sv[2]);     // ::socketpair(AF_UNIX,SOCK_STREAM,0,sv)
  }
#endif
```

---

## Phase 2 — Adapt the socket call sites

### 2a. `src/client/connection.{h,cpp}` (Baichuan TCP client)
- `int socket_fd_` → `net::socket_t socket_fd_ = net::kInvalidSocket;`
- Replace `socket_fd_ >= 0` / `< 0` checks with `!= kInvalidSocket` /
  `== kInvalidSocket` (**required** — `SOCKET` is unsigned, so `>= 0` is always
  true and would silently break every guard).
- `is_connected()` uses the same comparison.
- `fcntl` non-blocking connect → `net::set_nonblocking`.
- `poll(&pfd,1,10000)` connect-wait → `net::poll_sockets`.
- `getsockopt(SO_ERROR)` after connect: `optval` is `char*` on Windows — use a
  small helper or cast; logic unchanged.
- `close()` → `net::close_socket`; `strerror(errno)` → `net::error_string(net::last_error())`.
- `recv` return: use `int n` (buffers are ≤4096; POSIX `ssize_t`→`int` is safe here).
- `inet_pton`/`htons`/`sockaddr_in` are identical on both (via `ws2tcpip.h`).

### 2b. `src/mjpeg/mjpeg_source.cpp` (HTTP MJPEG client)
- Same `socket_t`/sentinel/`close`/error changes.
- **`SO_RCVTIMEO`/`SO_SNDTIMEO`:** replace the `struct timeval` calls with
  `net::set_recv_timeout(s, ms)` / `set_send_timeout(s, ms)` (DWORD ms on Windows).
- `shutdown(fd, SHUT_RDWR)` → `net::shutdown_both(fd)`.
- `send`/`recv` unchanged apart from return-type width.
- Uses blocking connect + timeouts (no `fcntl`), so minimal change.

### 2c. `src/control/command_server.{h,cpp}` (runtime control) — TCP-only on Windows
- Member fd types → `net::socket_t`, sentinels updated.
- Wrap the **entire Unix-domain path** in `#ifndef _WIN32`:
  `create_unix_socket`, `AF_UNIX`/`sockaddr_un`/`unlink`, and the `unix_fd_`
  poll branch. On Windows only the TCP listener is built (spec: TCP-only).
- **Self-pipe replacement:** `pipe(quit_pipe_)` → `net::make_socketpair(quit_pipe_)`
  (two `socket_t`). `stop()` writes a byte with `send`; `close()` → `close_socket`.
  This keeps the existing `poll`-based listener loop intact on both platforms —
  the wakeup fd is just a socket instead of a pipe.
- `poll(fds,…)` → `net::poll_sockets`; `fds` vector element type → `pollfd_t`.
- `accept` returns `net::socket_t`; `handle_connection(socket_t)`.
- `EINTR` retry branch: harmless on Windows (never returned) — guard or leave.
- `handle_connection`'s `SO_RCVTIMEO timeval` → `net::set_recv_timeout`.
- CommandServer construction is already gated by config in `dashboard_main.cpp`
  (`unix_path.empty()` etc.), so a Windows config simply omits `control.unix`.

---

## Phase 3 — Program entry points & platform glue

### `src/dashboard_main.cpp` and `src/main.cpp`
- Call `net::global_init()` at startup, `net::global_cleanup()` at exit
  (RAII guard object in `main` is cleanest).
- `getopt_long` / `<getopt.h>`: **works as-is on MinGW** — no change. (If a future
  MSVC build is ever wanted, swap in a `wingetopt` shim; out of scope now.)
- Signals: `std::signal(SIGINT,…)` works (Ctrl+C). `SIGTERM` compiles but is
  rarely delivered on Windows — keep it; optionally add `SetConsoleCtrlHandler`
  later for graceful window-close from console. Not required for VM testing.

### Default config path — `dashboard_main.cpp::default_config_path()`
- Add a Windows branch using `%APPDATA%`:
  `%APPDATA%\baichuan\config.json` (fallback `%USERPROFILE%\.config\baichuan\config.json`).
- Keep the existing XDG/`HOME` logic under `#ifndef _WIN32`.

---

## Phase 4 — Build system (`CMakeLists.txt`)

- Add `src/utils/net_compat.cpp` to `UTILS_SOURCES`.
- Replace the explicit `pthread` link with portable threading:
  ```cmake
  find_package(Threads REQUIRED)
  # link Threads::Threads instead of raw "pthread"
  ```
- Windows socket + define:
  ```cmake
  if(WIN32)
    target_link_libraries(<tgt> PRIVATE ws2_32)
    target_compile_definitions(<tgt> PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  endif()
  ```
  (`_WIN32_WINNT=0x0A00` = Windows 10, needed for `WSAPoll`.)
- `pkg_check_modules(GTK3 / FFMPEG / LIBJPEG …)` is unchanged — MSYS2 provides
  the `.pc` files. The existing `HELP_URL` compile definition is unaffected.
- Warning flags (`-Wall -Wextra -Wpedantic`) are honored by GCC/Clang in MSYS2 —
  no gating needed for this toolchain.

Configure/build in the UCRT64 shell:
```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

---

## Phase 5 — Packaging for the VM

**Option A — fastest to a green test (recommended first):** install MSYS2 in the
VM and run `./dashboard.exe` from the **UCRT64 shell**, where all DLLs and GTK
assets are already on `PATH`. Proves the port with zero packaging effort.

**Option B — standalone bundle (clean VM, nothing pre-installed):** assemble a
distributable folder next to `dashboard.exe`:
1. **DLLs:** MinGW runtime (`libstdc++-6`, `libgcc_s_seh-1`, `libwinpthread-1`) +
   all GTK/GLib/Cairo/Pango/FFmpeg/OpenSSL/libxml2/libjpeg DLLs. Enumerate with
   `ldd build/dashboard.exe | grep ucrt64` and copy each.
2. **GTK data assets** (DLLs alone are not enough):
   - gdk-pixbuf loaders → run `gdk-pixbuf-query-loaders` and ship `loaders.cache`
     under `lib/gdk-pixbuf-2.0/2.10.0/`.
   - GSettings schemas → copy `share/glib-2.0/schemas` and run
     `glib-compile-schemas`.
   - An icon theme → ship `share/icons/Adwaita` + `share/icons/hicolor`.
3. Provide a `scripts/bundle_windows.sh` that does the above so it's repeatable.

Deliver as a zip; unzip-and-run on a fresh Windows 10/11 VM.

---

## Phase 6 — Testing in the VM

1. **Smoke:** `dashboard.exe --help` prints usage + HELP_URL.
2. **GTK window:** launches, grid renders, Quit button works.
3. **Baichuan camera:** connect + decode + display (validates `connection.cpp`
   Winsock connect/poll/recv + FFmpeg decode on Windows).
4. **RTSP + MJPEG cameras:** validates FFmpeg RTSP and `mjpeg_source.cpp` timeouts.
5. **Keyboard hotkeys:** 1–9 focus, 0 overview, `r` resolution toggle, with the
   configured `hotkey_modifier`.
6. **Control (TCP):** from the host or VM,
   `echo '{"list":true}' | nc <vm-ip> 9100` returns the feed list; exercise
   `show` / `disconnect` / `fullscreen`.
7. **Shutdown:** Ctrl+C and window-close both exit cleanly (validates the
   socketpair wakeup replacing the self-pipe, and `WSACleanup`).
8. **Suspend/resume:** matches the existing reconnect behavior (regression check).

---

## Risks & mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|------------|
| `<windows.h>`/`<winsock.h>` vs `<winsock2.h>` clash via GTK | Med | `WIN32_LEAN_AND_MEAN`; keep socket TUs free of GTK headers; include order enforced in `net_compat.h` |
| `SOCKET` unsigned breaks `fd >= 0` guards | High if missed | Central `kInvalidSocket` + audit every comparison (Phase 2 checklist) |
| `SO_RCVTIMEO` type mismatch (silent, no timeout) | Med | `set_recv_timeout` helper wraps the platform difference |
| GTK runtime assets missing → blank icons / silent exit | Med | Phase 5 bundle script (loaders/schemas/icons) |
| WSAPoll edge cases on failed connect | Low | Only used for `accept`/`recv` readiness, not connect |
| MinGW GCC vs Linux GCC divergence | Low | Same compiler family; CI can build both |

---

## Deliverables / checklist

- [x] `src/utils/net_compat.h` + `net_compat.cpp` (WSAStartup, socketpair, error_string, timeouts)
- [x] `connection.{h,cpp}` migrated to `net::` layer + `socket_t`
- [x] `mjpeg_source.cpp` migrated (timeouts, shutdown, socket_t)
- [x] `command_server.{h,cpp}` TCP-only on Windows + socketpair wakeup
- [x] `dashboard_main.cpp` / `main.cpp`: WSA init, `%APPDATA%` config path
- [x] `CMakeLists.txt`: `Threads::Threads`, `ws2_32`, Win defines, new source
- [x] Still builds/runs on Linux (no regressions) — **verified**
- [x] Windows-only branches audited by hand (2 defects found + fixed — see Status)
- [ ] Builds clean in MSYS2 UCRT64 — **needs the VM/MSYS2 environment**
- [x] `scripts/bundle_windows.sh` for the standalone VM bundle
- [x] `BUILD.md`: Windows install + build + run + bundle walkthrough

**Status:** the cross-platform source changes (Phases 1–4) are complete and the
Linux build is green. What remains requires the Windows toolchain: the first
MSYS2 UCRT64 compile, the packaging bundle, and the in-VM test pass.

### Pre-compile audit of the Windows-only branches

The `#ifdef _WIN32` paths have never been through a compiler, so they were read
line-by-line against the Winsock API. Two defects were found and fixed:

1. **`net_compat.cpp` would not have compiled.** The header defines
   `WIN32_LEAN_AND_MEAN`, and under that macro `<winsock2.h>` deliberately does
   *not* include `<windows.h>` — leaving `FormatMessageA`, `LocalFree`, `LPSTR`
   and `MAKELANGID` undeclared in `error_string()`. Fixed by including
   `<windows.h>` explicitly, before `<winsock2.h>`, which is also the canonical
   ordering that keeps the old `<winsock.h>` out.

2. **`WSAPoll` cannot be used to wait on a non-blocking `connect`.** It does not
   report a failed connection attempt (documented Microsoft defect), so a
   *refused* connect — camera powered off — would not be signalled at all and
   `connection.cpp` would stall for the full 10 s timeout on every attempt,
   including each reconnect. Replaced with `net::wait_connect()`: `poll(POLLOUT)`
   on POSIX, `select()` with an **exceptfds** set on Windows, which does report
   the failure. Verified on Linux: refused connect resolves in 0 ms with
   `SO_ERROR=ECONNREFUSED`; the timeout path still honours its budget exactly.

Also audited and found correct: every `socket_t` sentinel comparison (no
surviving `fd >= 0` on an unsigned `SOCKET`), all `unlink`/`sockaddr_un` uses
sit inside `#ifndef _WIN32`, `net::Init` failure is checked in both `main()`s,
and the socketpair wakeup channel behaves (verified on Linux).

Remaining Windows-specific risk is now concentrated in the build environment
(pkg-config/GTK discovery under MSYS2) rather than in the source.

---

## Effort estimate

| Phase | Effort |
|-------|--------|
| 0 Env + GTK spike | 0.5 day |
| 1 net_compat layer | 0.5 day |
| 2 Adapt 3 socket files | 1–1.5 days |
| 3 Entry glue + config path | 0.5 day |
| 4 CMake | 0.25 day |
| 5 Packaging bundle | 0.5–1 day |
| 6 VM testing + fixes | 1 day |
| **Total** | **~4–5 days** |

The Linux build must keep working throughout — every change is either shared
(`net::` layer) or behind `#ifdef _WIN32`, so `main` stays green on both targets.
