#pragma once

#include "client/connection.h"
#include "protocol/bc_media.h"
#include <functional>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <set>

namespace baichuan {

// Stream configuration
struct StreamConfig {
    uint8_t channel_id = 0;
    uint32_t handle = STREAM_HANDLE_MAIN;  // 0=main, 256=sub, 1024=extern
    std::string stream_type = "mainStream";
};

// Callback types for stream events
using FrameCallback = std::function<void(const BcMediaFrame&)>;
using StreamInfoCallback = std::function<void(const BcMediaInfo&)>;
using ErrorCallback = std::function<void(const std::string&)>;

class VideoStream {
public:
    explicit VideoStream(Connection& conn);
    ~VideoStream();

    // Start video stream
    bool start(const StreamConfig& config = StreamConfig{});

    // Stop video stream
    void stop();

    // Check if streaming
    bool is_streaming() const { return streaming_.load(); }

    // Set callbacks
    void on_frame(FrameCallback cb) { frame_callback_ = std::move(cb); }
    void on_stream_info(StreamInfoCallback cb) { stream_info_callback_ = std::move(cb); }
    void on_error(ErrorCallback cb) { error_callback_ = std::move(cb); }

    // Get stream info (available after first info frame is received)
    const BcMediaInfo* stream_info() const {
        return stream_info_received_ ? &stream_info_ : nullptr;
    }

    // Get statistics
    struct Stats {
        uint64_t frames_received = 0;
        uint64_t bytes_received = 0;
        uint64_t i_frames = 0;
        uint64_t p_frames = 0;
    };
    Stats stats() const { return stats_; }

private:
    Connection& conn_;
    StreamConfig config_;

    std::atomic<bool> streaming_{false};
    std::thread receive_thread_;

    // Stream info
    BcMediaInfo stream_info_;
    bool stream_info_received_ = false;

    // Callbacks
    FrameCallback frame_callback_;
    StreamInfoCallback stream_info_callback_;
    ErrorCallback error_callback_;

    // Statistics
    Stats stats_;

    // Binary mode tracking (msg_num for which we're in binary mode)
    std::set<uint16_t> binary_mode_nums_;
    std::mutex binary_mode_mutex_;

    // Media data buffer - accumulates data across BC messages
    std::vector<uint8_t> media_buffer_;

    // Internal methods
    bool send_start_request();
    bool send_stop_request();
    void receive_loop();
    void process_message(const BcMessage& msg);
    void process_media_data(const std::vector<uint8_t>& data);
};

} // namespace baichuan
