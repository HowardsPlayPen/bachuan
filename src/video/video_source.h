#pragma once

#include <functional>
#include <string>
#include <cstdint>
#include <vector>

namespace baichuan {

// Common video codec enum (shared with bc_media.h)
enum class VideoCodec;

// Unified interface for video sources (Baichuan, RTSP, etc.)
class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    // Connect to the video source
    virtual bool connect() = 0;

    // Start receiving video
    virtual bool start() = 0;

    // Stop receiving video
    virtual void stop() = 0;

    // Check if connected
    virtual bool is_connected() const = 0;

    // Check if streaming
    virtual bool is_streaming() const = 0;

    // Callbacks
    using FrameCallback = std::function<void(const uint8_t* data, size_t len, VideoCodec codec)>;
    using ErrorCallback = std::function<void(const std::string& error)>;
    using InfoCallback = std::function<void(int width, int height, int fps)>;

    virtual void on_frame(FrameCallback cb) = 0;
    virtual void on_error(ErrorCallback cb) = 0;
    virtual void on_info(InfoCallback cb) = 0;
};

} // namespace baichuan
