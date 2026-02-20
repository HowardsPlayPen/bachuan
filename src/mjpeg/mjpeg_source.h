#pragma once

#include "video/decoder.h"  // For DecodedFrame
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace baichuan {

// MJPEG over HTTP video source
// Connects to HTTP MJPEG streams and decodes JPEG frames using libjpeg
class MjpegSource {
public:
    MjpegSource();
    ~MjpegSource();

    // Set stream URL (must be called before connect)
    // Format: http://[username:password@]host[:port]/path
    void set_url(const std::string& url);

    // Set connection timeout in seconds (default: 10)
    void set_timeout(int seconds);

    // Connect to the MJPEG stream
    bool connect();

    // Start receiving and decoding frames
    bool start();

    // Stop streaming
    void stop();

    // Check connection state
    bool is_connected() const;
    bool is_streaming() const;

    // Callbacks - MJPEG delivers decoded frames directly (no VideoDecoder needed)
    using DecodedFrameCallback = std::function<void(const DecodedFrame&)>;
    using ErrorCallback = std::function<void(const std::string&)>;
    using InfoCallback = std::function<void(int width, int height, int fps)>;

    void on_frame(DecodedFrameCallback cb);
    void on_error(ErrorCallback cb);
    void on_info(InfoCallback cb);

    // Statistics
    struct Stats {
        uint64_t frames_received = 0;
        uint64_t bytes_received = 0;
        uint64_t decode_errors = 0;
    };
    Stats stats() const { return stats_; }

private:
    std::string url_;
    std::string host_;
    uint16_t port_ = 80;
    std::string path_;
    std::string auth_header_;  // Base64 encoded credentials
    std::string boundary_;     // Multipart boundary string

    int socket_fd_ = -1;
    int timeout_seconds_ = 10;

    std::thread receive_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    DecodedFrameCallback frame_callback_;
    ErrorCallback error_callback_;
    InfoCallback info_callback_;
    std::mutex callback_mutex_;

    Stats stats_;
    bool info_sent_ = false;

    // URL parsing
    bool parse_url();
    std::string base64_encode(const std::string& input);

    // HTTP handling
    bool send_http_request();
    bool read_http_headers();
    std::string read_line();
    bool read_bytes(std::vector<uint8_t>& buffer, size_t count);

    // Multipart parsing
    bool find_boundary();
    bool read_part_headers(size_t& content_length);
    bool read_jpeg_frame(std::vector<uint8_t>& jpeg_data, size_t content_length);

    // JPEG decoding (using libjpeg)
    bool decode_jpeg(const std::vector<uint8_t>& jpeg_data, DecodedFrame& frame);

    // Main loop
    void receive_loop();

    // Cleanup
    void cleanup();
};

} // namespace baichuan
