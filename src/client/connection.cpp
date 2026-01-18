#include "client/connection.h"
#include "utils/logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

namespace baichuan {

Connection::Connection() {
    recv_buffer_.reserve(65536);
}

Connection::~Connection() {
    disconnect();
}

bool Connection::connect(const std::string& host, uint16_t port) {
    if (socket_fd_ >= 0) {
        disconnect();
    }

    LOG_INFO("Connecting to {}:{}", host, port);

    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        LOG_ERROR("Failed to create socket: {}", strerror(errno));
        return false;
    }

    // Set TCP_NODELAY to disable Nagle's algorithm
    int flag = 1;
    if (setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
        LOG_WARN("Failed to set TCP_NODELAY: {}", strerror(errno));
    }

    // Set socket receive buffer
    int recv_buf_size = 256 * 1024;
    if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &recv_buf_size, sizeof(recv_buf_size)) < 0) {
        LOG_WARN("Failed to set SO_RCVBUF: {}", strerror(errno));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid address: {}", host);
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Set non-blocking for connect with timeout
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

    int result = ::connect(socket_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (result < 0 && errno != EINPROGRESS) {
        LOG_ERROR("Failed to connect: {}", strerror(errno));
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Wait for connection with timeout
    struct pollfd pfd;
    pfd.fd = socket_fd_;
    pfd.events = POLLOUT;
    pfd.revents = 0;

    result = poll(&pfd, 1, 10000); // 10 second timeout
    if (result <= 0) {
        LOG_ERROR("Connection timeout or error");
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Check for connection error
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error != 0) {
        LOG_ERROR("Connection failed: {}", error != 0 ? strerror(error) : "getsockopt error");
        close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    // Set back to blocking mode
    fcntl(socket_fd_, F_SETFL, flags);

    host_ = host;
    port_ = port;

    LOG_INFO("Connected to {}:{}", host, port);
    return true;
}

void Connection::disconnect() {
    if (socket_fd_ >= 0) {
        LOG_INFO("Disconnecting from {}:{}", host_, port_);
        close(socket_fd_);
        socket_fd_ = -1;
    }
    recv_buffer_.clear();
    send_offset_ = 0;
    recv_offset_ = 0;
}

bool Connection::send_message(const BcMessage& msg) {
    std::lock_guard<std::mutex> lock(send_mutex_);

    if (socket_fd_ < 0) {
        LOG_ERROR("Not connected");
        return false;
    }

    // Serialize the message
    std::vector<uint8_t> data = msg.serialize();

    // Encrypt the body portion if needed (header is never encrypted)
    size_t header_size = msg.header.header_size();
    if (data.size() > header_size && crypto_.type() != EncryptionType::Unencrypted) {
        std::vector<uint8_t> body(data.begin() + header_size, data.end());
        LOG_DEBUG("Encrypting {} bytes with offset {}, encryption type {}",
                  body.size(), send_offset_, static_cast<int>(crypto_.type()));
        LOG_DEBUG("First 32 bytes of plaintext: {}",
                  Logger::bytes_to_hex(body.data(), std::min(body.size(), size_t(32))));
        std::vector<uint8_t> encrypted = crypto_.encrypt(msg.header.channel_id, body);
        LOG_DEBUG("First 32 bytes of ciphertext: {}",
                  Logger::bytes_to_hex(encrypted.data(), std::min(encrypted.size(), size_t(32))));
        std::copy(encrypted.begin(), encrypted.end(), data.begin() + header_size);
    } else if (data.size() > header_size) {
        LOG_DEBUG("Sending {} bytes unencrypted", data.size() - header_size);
    }

    LOG_DEBUG("Sending {} message, {} bytes, msg_num={}",
              BcHeader::msg_id_name(msg.header.msg_id), data.size(), msg.header.msg_num);

    bool result = send_raw(data.data(), data.size());
    if (result) {
        send_offset_ += static_cast<uint32_t>(data.size() - header_size);
    }
    return result;
}

std::optional<BcMessage> Connection::receive_message(int timeout_ms) {
    std::lock_guard<std::mutex> lock(recv_mutex_);

    if (socket_fd_ < 0) {
        LOG_ERROR("Not connected");
        return std::nullopt;
    }

    // First, ensure we have at least 24 bytes for the largest possible header
    while (recv_buffer_.size() < HEADER_SIZE_24) {
        if (!wait_for_data(timeout_ms)) {
            return std::nullopt;
        }

        uint8_t temp[1024];
        ssize_t n = recv(socket_fd_, temp, sizeof(temp), 0);
        if (n <= 0) {
            if (n == 0) {
                LOG_ERROR("Connection closed by peer");
            } else {
                LOG_ERROR("Receive error: {}", strerror(errno));
            }
            return std::nullopt;
        }
        recv_buffer_.insert(recv_buffer_.end(), temp, temp + n);
    }

    // Parse header
    BcHeader header;
    size_t header_size = BcHeader::deserialize(recv_buffer_.data(), recv_buffer_.size(), header);
    if (header_size == 0) {
        LOG_ERROR("Failed to parse header");
        return std::nullopt;
    }

    // Wait for complete message
    size_t total_size = header_size + header.body_len;
    while (recv_buffer_.size() < total_size) {
        if (!wait_for_data(timeout_ms)) {
            LOG_ERROR("Timeout waiting for message body ({} of {} bytes)",
                      recv_buffer_.size(), total_size);
            return std::nullopt;
        }

        uint8_t temp[4096];
        ssize_t n = recv(socket_fd_, temp, sizeof(temp), 0);
        if (n <= 0) {
            if (n == 0) {
                LOG_ERROR("Connection closed by peer");
            } else {
                LOG_ERROR("Receive error: {}", strerror(errno));
            }
            return std::nullopt;
        }
        recv_buffer_.insert(recv_buffer_.end(), temp, temp + n);
    }

    // Extract message body
    BcMessage msg;
    msg.header = header;

    if (header.body_len > 0) {
        std::vector<uint8_t> body(recv_buffer_.begin() + header_size,
                                   recv_buffer_.begin() + total_size);

        // Split into extension and payload based on payload_offset FIRST
        // Then decrypt selectively - extension is always XML (encrypted),
        // but payload may be binary (not encrypted in BCEncrypt mode)
        if (header.payload_offset && *header.payload_offset > 0) {
            uint32_t offset = *header.payload_offset;
            if (offset <= body.size()) {
                msg.extension_data.assign(body.begin(), body.begin() + offset);
                msg.payload_data.assign(body.begin() + offset, body.end());

                // Decrypt extension (always XML)
                if (crypto_.type() != EncryptionType::Unencrypted && !msg.extension_data.empty()) {
                    msg.extension_data = crypto_.decrypt(header.channel_id, msg.extension_data);
                }

                // Check if payload is binary (video/audio)
                // Binary mode is indicated by binaryData=1 in extension XML
                // For FullAes, encryptLen tells us the actual data length (before AES padding)
                // Once a msg_num is flagged as binary, all subsequent messages with that msg_num are binary
                bool is_binary = false;
                bool in_binary_from_extension = false;
                std::optional<uint32_t> encrypt_len;
                if (!msg.extension_data.empty()) {
                    std::string ext_str(msg.extension_data.begin(), msg.extension_data.end());
                    in_binary_from_extension = ext_str.find("<binaryData>1</binaryData>") != std::string::npos;

                    // Track binary mode per msg_num
                    if (in_binary_from_extension) {
                        binary_mode_nums_.insert(header.msg_num);
                    }

                    // Parse encryptLen for FullAes mode
                    size_t enc_start = ext_str.find("<encryptLen>");
                    size_t enc_end = ext_str.find("</encryptLen>");
                    if (enc_start != std::string::npos && enc_end != std::string::npos) {
                        enc_start += 12; // length of "<encryptLen>"
                        std::string enc_val = ext_str.substr(enc_start, enc_end - enc_start);
                        encrypt_len = static_cast<uint32_t>(std::stoul(enc_val));
                    }

                    // Debug: log extension for video messages
                    if (header.msg_id == MSG_ID_VIDEO) {
                        LOG_DEBUG("Video extension: binary={}, encryptLen={}, ext={}",
                                  in_binary_from_extension ? "yes" : "no",
                                  encrypt_len ? std::to_string(*encrypt_len) : "none",
                                  ext_str.substr(0, std::min(ext_str.size(), size_t(200))));
                    }
                }

                // Check if this msg_num is in binary mode (either from this extension or previously set)
                is_binary = in_binary_from_extension || binary_mode_nums_.count(header.msg_num) > 0;

                // Decrypt payload based on encryption type:
                // - FullAes with encryptLen: only first encryptLen bytes are encrypted
                // - FullAes without encryptLen: all encrypted
                // - BCEncrypt/Aes: decrypt only XML, not binary
                if (crypto_.type() == EncryptionType::FullAes && is_binary && encrypt_len && *encrypt_len > 0) {
                    // FullAes binary mode: decrypt ALL bytes, but only first encryptLen bytes
                    // are actual encrypted data. The rest should already be cleartext that
                    // was included after the encrypted portion.
                    //
                    // However, AES-CFB is a stream cipher - decrypting raw cleartext
                    // would produce garbage. So we need to:
                    // 1. Decrypt only first encryptLen bytes
                    // 2. Keep rest as-is (it's cleartext continuation data)
                    if (*encrypt_len < msg.payload_data.size()) {
                        // Split: encrypted part + cleartext part
                        std::vector<uint8_t> encrypted_part(msg.payload_data.begin(),
                                                            msg.payload_data.begin() + *encrypt_len);
                        std::vector<uint8_t> clear_part(msg.payload_data.begin() + *encrypt_len,
                                                        msg.payload_data.end());
                        // Decrypt only the encrypted portion
                        auto decrypted = crypto_.decrypt(header.channel_id, encrypted_part);

                        // Debug: show what we got
                        if (decrypted.size() >= 8) {
                            char hex[64];
                            snprintf(hex, sizeof(hex), "%02x %02x %02x %02x %02x %02x %02x %02x",
                                     decrypted[0], decrypted[1], decrypted[2], decrypted[3],
                                     decrypted[4], decrypted[5], decrypted[6], decrypted[7]);
                            LOG_DEBUG("Decrypted first 8 bytes: {}", hex);
                        }

                        // Combine: decrypted + cleartext
                        msg.payload_data = std::move(decrypted);
                        msg.payload_data.insert(msg.payload_data.end(), clear_part.begin(), clear_part.end());
                    } else {
                        // All data is encrypted (encryptLen >= payload size)
                        msg.payload_data = crypto_.decrypt(header.channel_id, msg.payload_data);
                    }
                } else if (crypto_.type() == EncryptionType::FullAes && !is_binary) {
                    // Non-binary XML payload - decrypt all
                    msg.payload_data = crypto_.decrypt(header.channel_id, msg.payload_data);
                } else if (crypto_.type() != EncryptionType::Unencrypted && !is_binary) {
                    // BCEncrypt/Aes: decrypt only XML, not binary
                    msg.payload_data = crypto_.decrypt(header.channel_id, msg.payload_data);
                }
                // Binary without encryptLen: leave as raw
            } else {
                msg.payload_data = std::move(body);
                if (crypto_.type() != EncryptionType::Unencrypted) {
                    msg.payload_data = crypto_.decrypt(header.channel_id, msg.payload_data);
                }
            }
        } else {
            msg.payload_data = std::move(body);
            // No extension - could be XML or binary
            // Check if this msg_num is in binary mode (from a previous message with binaryData=1)
            bool is_binary = binary_mode_nums_.count(header.msg_num) > 0;
            bool is_video_msg = (header.msg_id == MSG_ID_VIDEO || header.msg_id == MSG_ID_VIDEO_STOP);

            // For binary mode (tracked by msg_num) or video messages: data is raw, don't decrypt
            // Only decrypt non-binary payloads (XML responses)
            if (crypto_.type() != EncryptionType::Unencrypted && !is_binary && !is_video_msg) {
                msg.payload_data = crypto_.decrypt(header.channel_id, msg.payload_data);
            }
            // Binary data without extension is always raw (even for FullAes)
        }
    }

    // Update receive offset and remove processed data from buffer
    recv_offset_ += header.body_len;
    recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin() + total_size);

    LOG_DEBUG("Received {} message, {} bytes, response={}, msg_num={}, payload_offset={}",
              BcHeader::msg_id_name(msg.header.msg_id),
              total_size, msg.header.response_code, msg.header.msg_num,
              msg.header.payload_offset ? static_cast<int>(*msg.header.payload_offset) : -1);

    return msg;
}

bool Connection::send_raw(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(socket_fd_, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            LOG_ERROR("Send error: {}", strerror(errno));
            return false;
        }
        sent += n;
    }
    return true;
}

bool Connection::recv_raw(uint8_t* data, size_t len, int timeout_ms) {
    size_t received = 0;
    while (received < len) {
        if (!wait_for_data(timeout_ms)) {
            return false;
        }

        ssize_t n = recv(socket_fd_, data + received, len - received, 0);
        if (n <= 0) {
            if (n == 0) {
                LOG_ERROR("Connection closed by peer");
            } else {
                LOG_ERROR("Receive error: {}", strerror(errno));
            }
            return false;
        }
        received += n;
    }
    return true;
}

bool Connection::wait_for_data(int timeout_ms) {
    if (timeout_ms == 0) {
        return true; // No timeout, assume data will arrive
    }

    struct pollfd pfd;
    pfd.fd = socket_fd_;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int result = poll(&pfd, 1, timeout_ms);
    if (result < 0) {
        LOG_ERROR("Poll error: {}", strerror(errno));
        return false;
    }
    if (result == 0) {
        return false; // Timeout
    }
    return true;
}

} // namespace baichuan
