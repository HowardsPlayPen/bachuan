#include "control/command_server.h"
#include "utils/logger.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <cerrno>
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

int CommandServer::create_unix_socket(const std::string& path) {
    // Remove stale socket file
    unlink(path.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("CommandServer: Failed to create Unix socket: {}", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        LOG_ERROR("CommandServer: Unix socket path too long: {}", path);
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("CommandServer: Failed to bind Unix socket {}: {}", path, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        LOG_ERROR("CommandServer: Failed to listen on Unix socket: {}", strerror(errno));
        close(fd);
        unlink(path.c_str());
        return -1;
    }

    LOG_INFO("CommandServer: Listening on Unix socket {}", path);
    return fd;
}

int CommandServer::create_tcp_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("CommandServer: Failed to create TCP socket: {}", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("CommandServer: Failed to bind TCP port {}: {}", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 5) < 0) {
        LOG_ERROR("CommandServer: Failed to listen on TCP port {}: {}", port, strerror(errno));
        close(fd);
        return -1;
    }

    LOG_INFO("CommandServer: Listening on TCP port {}", port);
    return fd;
}

bool CommandServer::start() {
    if (unix_path_.empty() && tcp_port_ <= 0) {
        LOG_DEBUG("CommandServer: No listeners configured, not starting");
        return true;
    }

    // Create self-pipe for shutdown signaling
    if (pipe(quit_pipe_) < 0) {
        LOG_ERROR("CommandServer: Failed to create quit pipe: {}", strerror(errno));
        return false;
    }
    // Make read end non-blocking
    fcntl(quit_pipe_[0], F_SETFL, O_NONBLOCK);

    if (!unix_path_.empty()) {
        unix_fd_ = create_unix_socket(unix_path_);
        if (unix_fd_ < 0) return false;
    }

    if (tcp_port_ > 0) {
        tcp_fd_ = create_tcp_socket(tcp_port_);
        if (tcp_fd_ < 0) {
            if (unix_fd_ >= 0) { close(unix_fd_); unix_fd_ = -1; }
            if (!unix_path_.empty()) unlink(unix_path_.c_str());
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

    // Signal the listener to wake up via quit pipe
    if (quit_pipe_[1] >= 0) {
        char c = 'q';
        ssize_t ret = write(quit_pipe_[1], &c, 1);
        (void)ret;
    }

    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }

    if (unix_fd_ >= 0) { close(unix_fd_); unix_fd_ = -1; }
    if (tcp_fd_ >= 0) { close(tcp_fd_); tcp_fd_ = -1; }
    if (quit_pipe_[0] >= 0) { close(quit_pipe_[0]); quit_pipe_[0] = -1; }
    if (quit_pipe_[1] >= 0) { close(quit_pipe_[1]); quit_pipe_[1] = -1; }

    if (!unix_path_.empty()) {
        unlink(unix_path_.c_str());
    }

    LOG_INFO("CommandServer: Stopped");
}

void CommandServer::listener_loop() {
    LOG_DEBUG("CommandServer: Listener thread started");

    // Build poll fd set: quit_pipe + unix + tcp
    std::vector<struct pollfd> fds;

    // Always include quit pipe as first entry
    fds.push_back({quit_pipe_[0], POLLIN, 0});

    int unix_poll_idx = -1;
    if (unix_fd_ >= 0) {
        unix_poll_idx = static_cast<int>(fds.size());
        fds.push_back({unix_fd_, POLLIN, 0});
    }

    int tcp_poll_idx = -1;
    if (tcp_fd_ >= 0) {
        tcp_poll_idx = static_cast<int>(fds.size());
        fds.push_back({tcp_fd_, POLLIN, 0});
    }

    while (running_.load()) {
        int ret = poll(fds.data(), fds.size(), 1000);  // 1s timeout
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("CommandServer: poll error: {}", strerror(errno));
            break;
        }
        if (ret == 0) continue;  // timeout

        // Check quit pipe
        if (fds[0].revents & POLLIN) {
            break;
        }

        // Check unix socket
        if (unix_poll_idx >= 0 && (fds[unix_poll_idx].revents & POLLIN)) {
            int client = accept(unix_fd_, nullptr, nullptr);
            if (client >= 0) {
                handle_connection(client);
            }
        }

        // Check TCP socket
        if (tcp_poll_idx >= 0 && (fds[tcp_poll_idx].revents & POLLIN)) {
            int client = accept(tcp_fd_, nullptr, nullptr);
            if (client >= 0) {
                handle_connection(client);
            }
        }
    }

    LOG_DEBUG("CommandServer: Listener thread exiting");
}

void CommandServer::handle_connection(int client_fd) {
    // Set a read timeout so we don't block forever
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Read until newline or EOF (max 4KB)
    std::string request;
    char buf[1024];
    bool got_newline = false;

    while (request.size() < 4096) {
        ssize_t n = read(client_fd, buf, sizeof(buf));
        if (n <= 0) break;

        for (ssize_t i = 0; i < n; i++) {
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
        close(client_fd);
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
    ssize_t ret = write(client_fd, response.c_str(), response.size());
    (void)ret;

    close(client_fd);
}

} // namespace baichuan
