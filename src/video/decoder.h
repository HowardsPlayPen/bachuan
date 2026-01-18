#pragma once

#include "protocol/bc_media.h"
#include <memory>
#include <functional>
#include <cstdint>

// Forward declarations for FFmpeg types
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace bachuan {

// Decoded frame with RGB data
struct DecodedFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgb_data;  // RGB24 format
    int64_t pts = 0;  // Presentation timestamp
};

// Callback for decoded frames
using DecodedFrameCallback = std::function<void(const DecodedFrame&)>;

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // Initialize decoder for specific codec
    bool init(VideoCodec codec);

    // Shutdown decoder
    void shutdown();

    // Check if initialized
    bool is_initialized() const { return initialized_; }

    // Get current codec
    VideoCodec codec() const { return codec_; }

    // Decode a video frame (IFrame or PFrame)
    // Returns true if frame was decoded successfully
    bool decode(const uint8_t* data, size_t len, DecodedFrameCallback callback);

    // Convenience overloads
    bool decode(const std::vector<uint8_t>& data, DecodedFrameCallback callback) {
        return decode(data.data(), data.size(), std::move(callback));
    }

    bool decode(const BcMediaIFrame& frame, DecodedFrameCallback callback) {
        return decode(frame.data.data(), frame.data.size(), std::move(callback));
    }

    bool decode(const BcMediaPFrame& frame, DecodedFrameCallback callback) {
        return decode(frame.data.data(), frame.data.size(), std::move(callback));
    }

    // Get decoder statistics
    struct Stats {
        uint64_t frames_decoded = 0;
        uint64_t decode_errors = 0;
    };
    Stats stats() const { return stats_; }

private:
    bool initialized_ = false;
    VideoCodec codec_ = VideoCodec::H264;

    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* rgb_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;

    int output_width_ = 0;
    int output_height_ = 0;

    Stats stats_;

    bool setup_scaler(int width, int height);
    bool convert_to_rgb(DecodedFrame& output);
};

} // namespace bachuan
