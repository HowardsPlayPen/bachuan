#pragma once

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

    int unix_fd_ = -1;
    int tcp_fd_ = -1;
    int quit_pipe_[2] = {-1, -1};  // self-pipe for clean shutdown

    std::thread listener_thread_;
    std::atomic<bool> running_{false};

    CommandHandler handler_;

    void listener_loop();
    void handle_connection(int client_fd);
    int create_unix_socket(const std::string& path);
    int create_tcp_socket(int port);
};

} // namespace baichuan
