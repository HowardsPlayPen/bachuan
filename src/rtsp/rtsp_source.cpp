#include "rtsp/rtsp_source.h"
#include "utils/logger.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
}

namespace baichuan {

RtspSource::RtspSource() = default;

RtspSource::~RtspSource() {
    stop();
    cleanup();
}

void RtspSource::set_url(const std::string& url) {
    url_ = url;
}

void RtspSource::set_timeout(int seconds) {
    timeout_seconds_ = seconds;
}

void RtspSource::set_transport(const std::string& transport) {
    transport_ = transport;
}

bool RtspSource::connect() {
    if (url_.empty()) {
        LOG_ERROR("RTSP URL not set");
        return false;
    }

    cleanup();

    // Allocate format context
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        LOG_ERROR("Failed to allocate AVFormatContext");
        return false;
    }

    // Set options
    AVDictionary* options = nullptr;

    // Set RTSP transport (tcp/udp)
    av_dict_set(&options, "rtsp_transport", transport_.c_str(), 0);

    // Set connection timeout (in microseconds)
    char timeout_str[32];
    snprintf(timeout_str, sizeof(timeout_str), "%d", timeout_seconds_ * 1000000);
    av_dict_set(&options, "timeout", timeout_str, 0);
    av_dict_set(&options, "stimeout", timeout_str, 0);

    // Set buffer size for lower latency
    av_dict_set(&options, "buffer_size", "1024000", 0);

    // Set max delay for lower latency
    av_dict_set(&options, "max_delay", "500000", 0);

    LOG_INFO("Connecting to RTSP: {} (transport: {})", url_, transport_);

    // Open input
    int ret = avformat_open_input(&fmt_ctx_, url_.c_str(), nullptr, &options);
    av_dict_free(&options);

    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to open RTSP stream: {}", errbuf);
        cleanup();
        return false;
    }

    // Find stream info
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to find stream info: {}", errbuf);
        cleanup();
        return false;
    }

    // Find video stream
    video_stream_idx_ = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; i++) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx_ = static_cast<int>(i);
            break;
        }
    }

    if (video_stream_idx_ < 0) {
        LOG_ERROR("No video stream found in RTSP source");
        cleanup();
        return false;
    }

    // Get codec info
    AVStream* video_stream = fmt_ctx_->streams[video_stream_idx_];
    AVCodecParameters* codecpar = video_stream->codecpar;

    codec_ = detect_codec(codecpar->codec_id);

    int width = codecpar->width;
    int height = codecpar->height;

    // Extract extradata (SPS/PPS for H.264, VPS/SPS/PPS for H.265)
    if (codecpar->extradata && codecpar->extradata_size > 0) {
        extradata_.assign(codecpar->extradata,
                         codecpar->extradata + codecpar->extradata_size);
        LOG_DEBUG("RTSP extradata: {} bytes", extradata_.size());
    }

    // Reset keyframe state
    got_keyframe_.store(false);

    // Calculate FPS
    int fps = 25; // default
    if (video_stream->avg_frame_rate.den > 0) {
        fps = video_stream->avg_frame_rate.num / video_stream->avg_frame_rate.den;
    } else if (video_stream->r_frame_rate.den > 0) {
        fps = video_stream->r_frame_rate.num / video_stream->r_frame_rate.den;
    }

    LOG_INFO("RTSP connected: {}x{} @ {} fps, codec: {}",
             width, height, fps,
             codec_ == VideoCodec::H265 ? "H265" : "H264");

    connected_.store(true);

    // Notify info callback
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (info_callback_) {
            info_callback_(width, height, fps);
        }
    }

    return true;
}

bool RtspSource::start() {
    if (!connected_.load()) {
        LOG_ERROR("RTSP not connected");
        return false;
    }

    if (running_.load()) {
        LOG_WARN("RTSP already streaming");
        return true;
    }

    running_.store(true);
    receive_thread_ = std::thread(&RtspSource::receive_loop, this);

    LOG_INFO("RTSP streaming started");
    return true;
}

void RtspSource::stop() {
    running_.store(false);

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    LOG_INFO("RTSP streaming stopped");
}

bool RtspSource::is_connected() const {
    return connected_.load();
}

bool RtspSource::is_streaming() const {
    return running_.load();
}

void RtspSource::on_frame(FrameCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = std::move(cb);
}

void RtspSource::on_error(ErrorCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = std::move(cb);
}

void RtspSource::on_info(InfoCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    info_callback_ = std::move(cb);
}

void RtspSource::receive_loop() {
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        LOG_ERROR("Failed to allocate AVPacket");
        return;
    }

    while (running_.load()) {
        int ret = av_read_frame(fmt_ctx_, packet);

        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_INFO("RTSP stream ended");
            } else if (ret != AVERROR(EAGAIN)) {
                char errbuf[256];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Error reading RTSP frame: {}", errbuf);

                std::lock_guard<std::mutex> lock(callback_mutex_);
                if (error_callback_) {
                    error_callback_(std::string("Read error: ") + errbuf);
                }
            }

            // On error or EOF, wait a bit before retrying or exit
            if (ret == AVERROR_EOF) {
                running_.store(false);
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Check if this is the video stream
        if (packet->stream_index == video_stream_idx_) {
            bool is_keyframe = (packet->flags & AV_PKT_FLAG_KEY) != 0;

            // Wait for first keyframe before delivering frames
            if (!got_keyframe_.load()) {
                if (!is_keyframe) {
                    av_packet_unref(packet);
                    continue;
                }
                got_keyframe_.store(true);
                LOG_DEBUG("RTSP: Got first keyframe, starting decode");
            }

            // Deliver frame data to callback
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (frame_callback_ && packet->data && packet->size > 0) {
                // For first keyframe, prepend extradata if available
                if (is_keyframe && !extradata_.empty()) {
                    std::vector<uint8_t> frame_data;
                    frame_data.reserve(extradata_.size() + packet->size);
                    frame_data.insert(frame_data.end(), extradata_.begin(), extradata_.end());
                    frame_data.insert(frame_data.end(), packet->data, packet->data + packet->size);
                    frame_callback_(frame_data.data(), frame_data.size(), codec_);
                } else {
                    frame_callback_(packet->data, static_cast<size_t>(packet->size), codec_);
                }
            }
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

void RtspSource::cleanup() {
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    video_stream_idx_ = -1;
    extradata_.clear();
    got_keyframe_.store(false);
    connected_.store(false);
}

VideoCodec RtspSource::detect_codec(int codec_id) {
    switch (codec_id) {
        case AV_CODEC_ID_H264:
            return VideoCodec::H264;
        case AV_CODEC_ID_HEVC:
            return VideoCodec::H265;
        default:
            LOG_WARN("Unknown codec ID {}, assuming H264", codec_id);
            return VideoCodec::H264;
    }
}

} // namespace baichuan
