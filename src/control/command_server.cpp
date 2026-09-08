#include "control/command_server.h"
#include "utils/logger.h"
#include "utils/net_compat.h"

#include <cstring>

namespace baichuan {

CommandServer::CommandServer(const std::string& unix_path, int tcp_port)
    : unix_path_(unix_path), tcp_port_(tcp_port) {
}

CommandServer::~CommandServer() {
    stop();
}

void CommandServer::set_handler(CommandHandler handler) {
    handler_ = std::move(handler);
}

#ifndef _WIN32
// Unix-domain control socket is POSIX-only. On Windows the control interface is
// TCP-only (see start()).
net::socket_t CommandServer::create_unix_socket(const std::string& path) {
    // Remove stale socket file
    unlink(path.c_str());

    net::socket_t fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == net::kInvalidSocket) {
        LOG_ERROR("CommandServer: Failed to create Unix socket: {}", net::error_string(net::last_error()));
        return net::kInvalidSocket;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        LOG_ERROR("CommandServer: Unix socket path too long: {}", path);
        net::close_socket(fd);
        return net::kInvalidSocket;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("CommandServer: Failed to bind Unix socket {}: {}", path, net::error_string(net::last_error()));
        net::close_socket(fd);
        return net::kInvalidSocket;
    }

    if (listen(fd, 5) < 0) {
        LOG_ERROR("CommandServer: Failed to listen on Unix socket: {}", net::error_string(net::last_error()));
        net::close_socket(fd);
        unlink(path.c_str());
        return net::kInvalidSocket;
    }

    LOG_INFO("CommandServer: Listening on Unix socket {}", path);
    return fd;
}
#endif  // !_WIN32

net::socket_t CommandServer::create_tcp_socket(int port) {
    net::socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == net::kInvalidSocket) {
        LOG_ERROR("CommandServer: Failed to create TCP socket: {}", net::error_string(net::last_error()));
        return net::kInvalidSocket;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("CommandServer: Failed to bind TCP port {}: {}", port, net::error_string(net::last_error()));
        net::close_socket(fd);
        return net::kInvalidSocket;
    }

    if (listen(fd, 5) < 0) {
        LOG_ERROR("CommandServer: Failed to listen on TCP port {}: {}", port, net::error_string(net::last_error()));
        net::close_socket(fd);
        return net::kInvalidSocket;
    }

    LOG_INFO("CommandServer: Listening on TCP port {}", port);
    return fd;
}

bool CommandServer::start() {
    if (unix_path_.empty() && tcp_port_ <= 0) {
        LOG_DEBUG("CommandServer: No listeners configured, not starting");
        return true;
    }

    // Create a connected socket pair used to wake the listener out of poll() on
    // shutdown (portable replacement for the POSIX self-pipe trick).
    if (net::make_socketpair(quit_pipe_) < 0) {
        LOG_ERROR("CommandServer: Failed to create quit socketpair: {}", net::error_string(net::last_error()));
        return false;
    }
    net::set_nonblocking(quit_pipe_[0], true);

#ifndef _WIN32
    if (!unix_path_.empty()) {
        unix_fd_ = create_unix_socket(unix_path_);
        if (unix_fd_ == net::kInvalidSocket) return false;
    }
#else
    if (!unix_path_.empty()) {
        LOG_WARN("CommandServer: Unix control socket not supported on Windows; use tcp_port instead");
    }
#endif

    if (tcp_port_ > 0) {
        tcp_fd_ = create_tcp_socket(tcp_port_);
        if (tcp_fd_ == net::kInvalidSocket) {
#ifndef _WIN32
            if (unix_fd_ != net::kInvalidSocket) { net::close_socket(unix_fd_); unix_fd_ = net::kInvalidSocket; }
            if (!unix_path_.empty()) unlink(unix_path_.c_str());
#endif
            return false;
        }
    }

    running_.store(true);
    listener_thread_ = std::thread(&CommandServer::listener_loop, this);

    return true;
}

void CommandServer::stop() {
    if (!running_.load()) return;

    running_.store(false);

    // Signal the listener to wake up via the quit socket
    if (quit_pipe_[1] != net::kInvalidSocket) {
        char c = 'q';
        send(quit_pipe_[1], &c, 1, 0);
    }

    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    if (unix_fd_ != net::kInvalidSocket)    { net::close_socket(unix_fd_); unix_fd_ = net::kInvalidSocket; }
    if (tcp_fd_ != net::kInvalidSocket)     { net::close_socket(tcp_fd_); tcp_fd_ = net::kInvalidSocket; }
    if (quit_pipe_[0] != net::kInvalidSocket){ net::close_socket(quit_pipe_[0]); quit_pipe_[0] = net::kInvalidSocket; }
    if (quit_pipe_[1] != net::kInvalidSocket){ net::close_socket(quit_pipe_[1]); quit_pipe_[1] = net::kInvalidSocket; }

#ifndef _WIN32
    if (!unix_path_.empty()) {
        unlink(unix_path_.c_str());
    }
#endif

    LOG_INFO("CommandServer: Stopped");
}

void CommandServer::listener_loop() {
    LOG_DEBUG("CommandServer: Listener thread started");

    // Build poll fd set: quit socket + unix + tcp
    std::vector<net::pollfd_t> fds;

    // Always include the quit socket as the first entry
    fds.push_back({quit_pipe_[0], POLLIN, 0});

    int unix_poll_idx = -1;
    if (unix_fd_ != net::kInvalidSocket) {
        unix_poll_idx = static_cast<int>(fds.size());
        fds.push_back({unix_fd_, POLLIN, 0});
    }

    int tcp_poll_idx = -1;
    if (tcp_fd_ != net::kInvalidSocket) {
        tcp_poll_idx = static_cast<int>(fds.size());
        fds.push_back({tcp_fd_, POLLIN, 0});
    }

    while (running_.load()) {
        int ret = net::poll_sockets(fds.data(), fds.size(), 1000);  // 1s timeout
        if (ret < 0) {
            LOG_ERROR("CommandServer: poll error: {}", net::error_string(net::last_error()));
            break;
        }
        if (ret == 0) continue;  // timeout

        // Check quit socket
        if (fds[0].revents & POLLIN) {
            break;
        }

        // Check unix socket
        if (unix_poll_idx >= 0 && (fds[unix_poll_idx].revents & POLLIN)) {
            net::socket_t client = accept(unix_fd_, nullptr, nullptr);
            if (client != net::kInvalidSocket) {
                handle_connection(client);
            }
        }

        // Check TCP socket
        if (tcp_poll_idx >= 0 && (fds[tcp_poll_idx].revents & POLLIN)) {
            net::socket_t client = accept(tcp_fd_, nullptr, nullptr);
            if (client != net::kInvalidSocket) {
                handle_connection(client);
            }
        }
    }

    LOG_DEBUG("CommandServer: Listener thread exiting");
}

void CommandServer::handle_connection(net::socket_t client_fd) {
    // Set a read timeout so we don't block forever
    net::set_recv_timeout(client_fd, 5000);

    // Read until newline or EOF (max 4KB)
    std::string request;
    char buf[1024];
    bool got_newline = false;

    while (request.size() < 4096) {
        int n = recv(client_fd, buf, sizeof(buf), 0);
        if (n <= 0) break;

        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                got_newline = true;
                break;
            }
            request += buf[i];
        }
        if (got_newline) break;
    }

    // Trim whitespace
    while (!request.empty() && (request.back() == '\r' || request.back() == ' ')) {
        request.pop_back();
    }

    if (request.empty()) {
        net::close_socket(client_fd);
        return;
    }

    LOG_DEBUG("CommandServer: Received command: {}", request);

    std::string response;
    if (handler_) {
        response = handler_(request);
    } else {
        response = "{\"error\": \"no handler\"}";
    }

    response += "\n";
    send(client_fd, response.c_str(), static_cast<int>(response.size()), 0);

    net::close_socket(client_fd);
}

} // namespace baichuan
