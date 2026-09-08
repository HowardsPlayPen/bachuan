#pragma once

#include "utils/net_compat.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <vector>

namespace baichuan {

// Callback: receives raw JSON command string, returns response JSON string
using CommandHandler = std::function<std::string(const std::string&)>;

class CommandServer {
public:
    CommandServer(const std::string& unix_path = "", int tcp_port = 0);
    ~CommandServer();

    // Set the handler called for each received command
    void set_handler(CommandHandler handler);

    // Start listener thread(s)
    bool start();

    // Stop and clean up (closes sockets, joins thread, unlinks unix socket)
    void stop();

    bool is_running() const { return running_.load(); }

private:
    std::string unix_path_;
    int tcp_port_ = 0;

    net::socket_t unix_fd_ = net::kInvalidSocket;
    net::socket_t tcp_fd_ = net::kInvalidSocket;
    // Shutdown-wakeup channel (connected socket pair). [1] is written by stop(),
    // [0] is watched by the listener loop.
    net::socket_t quit_pipe_[2] = {net::kInvalidSocket, net::kInvalidSocket};

    std::thread listener_thread_;
    std::atomic<bool> running_{false};

    CommandHandler handler_;

    void listener_loop();
    void handle_connection(net::socket_t client_fd);
#ifndef _WIN32
    net::socket_t create_unix_socket(const std::string& path);
#endif
    net::socket_t create_tcp_socket(int port);
};

} // namespace baichuan
