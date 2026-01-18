#pragma once

#include "video/video_source.h"
#include "protocol/bc_media.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

// Forward declarations for FFmpeg
struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;

namespace baichuan {

// RTSP video source using FFmpeg libavformat
class RtspSource : public IVideoSource {
public:
    RtspSource();
    ~RtspSource() override;

    // Set RTSP URL (must be called before connect)
    // Format: rtsp://[username:password@]host[:port]/path
    void set_url(const std::string& url);

    // Set connection timeout in seconds (default: 5)
    void set_timeout(int seconds);

    // Set transport protocol: "tcp" or "udp" (default: tcp)
    void set_transport(const std::string& transport);

    // IVideoSource interface
    bool connect() override;
    bool start() override;
    void stop() override;
    bool is_connected() const override;
    bool is_streaming() const override;

    void on_frame(FrameCallback cb) override;
    void on_error(ErrorCallback cb) override;
    void on_info(InfoCallback cb) override;

private:
    std::string url_;
    std::string transport_ = "tcp";
    int timeout_seconds_ = 5;

    AVFormatContext* fmt_ctx_ = nullptr;
    int video_stream_idx_ = -1;
    VideoCodec codec_ = VideoCodec::H264;

    // Codec extradata (SPS/PPS for H.264/H.265)
    std::vector<uint8_t> extradata_;
    std::atomic<bool> got_keyframe_{false};

    std::thread receive_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    FrameCallback frame_callback_;
    ErrorCallback error_callback_;
    InfoCallback info_callback_;
    std::mutex callback_mutex_;

    void receive_loop();
    void cleanup();
    VideoCodec detect_codec(int codec_id);
};

} // namespace baichuan
