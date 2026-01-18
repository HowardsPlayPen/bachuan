#pragma once

#include "protocol/bc_header.h"
#include "protocol/bc_crypto.h"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <set>

namespace bachuan {

class Connection {
public:
    Connection();
    ~Connection();

    // Connect to camera
    bool connect(const std::string& host, uint16_t port = 9000);

    // Disconnect
    void disconnect();

    // Check if connected
    bool is_connected() const { return socket_fd_ >= 0; }

    // Send a message (handles encryption if needed)
    bool send_message(const BcMessage& msg);

    // Receive a message (handles decryption if needed)
    // timeout_ms: 0 = no timeout, otherwise wait up to timeout_ms milliseconds
    std::optional<BcMessage> receive_message(int timeout_ms = 5000);

    // Get next message number for sequencing
    uint16_t next_msg_num() { return ++msg_num_counter_; }

    // Set encryption protocol after login negotiation
    void set_encryption(BcCrypto&& crypto) { crypto_ = std::move(crypto); }
    BcCrypto& encryption() { return crypto_; }

    // Get the current offset for encryption (based on bytes sent/received)
    uint32_t send_offset() const { return send_offset_; }
    uint32_t recv_offset() const { return recv_offset_; }

    // Reset encryption offsets (done after login)
    void reset_encryption_offsets() {
        send_offset_ = 0;
        recv_offset_ = 0;
    }

    // Callback for received video frames (set by stream handler)
    using MessageCallback = std::function<void(const BcMessage&)>;
    void set_message_callback(MessageCallback cb) { message_callback_ = std::move(cb); }

private:
    int socket_fd_ = -1;
    std::string host_;
    uint16_t port_ = 9000;

    BcCrypto crypto_;
    std::atomic<uint16_t> msg_num_counter_{0};

    // Encryption offset tracking
    uint32_t send_offset_ = 0;
    uint32_t recv_offset_ = 0;

    // Receive buffer
    std::vector<uint8_t> recv_buffer_;

    // Thread safety
    mutable std::mutex send_mutex_;
    mutable std::mutex recv_mutex_;

    // Callback for messages
    MessageCallback message_callback_;

    // Binary mode tracking per msg_num (for FullAes)
    // Once a msg_num has binaryData=1, all subsequent messages with that msg_num are binary
    std::set<uint16_t> binary_mode_nums_;

    // Low-level socket operations
    bool send_raw(const uint8_t* data, size_t len);
    bool recv_raw(uint8_t* data, size_t len, int timeout_ms);
    bool wait_for_data(int timeout_ms);
};

} // namespace bachuan
