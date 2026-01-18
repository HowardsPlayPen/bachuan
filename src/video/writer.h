#pragma once

#include "video/decoder.h"
#include <string>
#include <cstdint>

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace baichuan {

// Save a single frame as JPEG image
class ImageWriter {
public:
    // Save decoded RGB frame as JPEG
    // Returns true on success
    static bool save_jpeg(const DecodedFrame& frame, const std::string& filename, int quality = 90);
};

// Write video frames to file (MPEG4 container with H264)
class VideoWriter {
public:
    VideoWriter();
    ~VideoWriter();

    // Initialize writer with output filename and frame dimensions
    // Supported formats: .mp4, .mpg, .avi (based on extension)
    bool open(const std::string& filename, int width, int height, int fps = 25);

    // Close and finalize the video file
    void close();

    // Check if writer is open
    bool is_open() const { return is_open_; }

    // Write a decoded RGB frame
    bool write_frame(const DecodedFrame& frame);

    // Get number of frames written
    uint64_t frames_written() const { return frames_written_; }

private:
    bool is_open_ = false;
    std::string filename_;

    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext* codec_ctx_ = nullptr;
    AVStream* stream_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int64_t pts_ = 0;
    uint64_t frames_written_ = 0;

    bool encode_frame(AVFrame* frame);
};

} // namespace baichuan
