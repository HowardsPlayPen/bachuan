#include "video/decoder.h"
#include "utils/logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libswscale/swscale.h>
}

namespace baichuan {

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() {
    shutdown();
}

bool VideoDecoder::try_open_decoder(const AVCodec* decoder) {
    // Silence FFmpeg's own log output (v4l2m2m probing, deprecated format, etc.)
    av_log_set_level(AV_LOG_QUIET);

    codec_ctx_ = avcodec_alloc_context3(decoder);
    if (!codec_ctx_) return false;

    // Low latency flags
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

    // Multi-threaded decoding (frame-level only, slice threading can cause issues)
    codec_ctx_->thread_count = 0;
    codec_ctx_->thread_type = FF_THREAD_FRAME;

    if (avcodec_open2(codec_ctx_, decoder, nullptr) < 0) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
        return false;
    }
    return true;
}

bool VideoDecoder::init(VideoCodec codec) {
    if (initialized_) {
        shutdown();
    }

    codec_ = codec;
    const char* codec_name = (codec == VideoCodec::H265) ? "H265" : "H264";
    AVCodecID codec_id = (codec == VideoCodec::H265) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;

    const AVCodec* decoder = avcodec_find_decoder(codec_id);
    if (!decoder) {
        LOG_ERROR("Failed to find {} decoder", codec_name);
        return false;
    }
    if (!try_open_decoder(decoder)) {
        LOG_ERROR("Failed to open {} decoder", codec_name);
        return false;
    }
    LOG_INFO("Video decoder initialized: {} ({})", codec_name, decoder->name);

    // Allocate frame and packet
    frame_ = av_frame_alloc();
    rgb_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    if (!frame_ || !rgb_frame_ || !packet_) {
        LOG_ERROR("Failed to allocate frame/packet");
        shutdown();
        return false;
    }

    initialized_ = true;
    stats_ = Stats{};
    return true;
}

void VideoDecoder::shutdown() {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    if (rgb_frame_) {
        av_frame_free(&rgb_frame_);
        rgb_frame_ = nullptr;
    }

    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    if (aligned_buf_) {
        av_free(aligned_buf_);
        aligned_buf_ = nullptr;
        aligned_buf_size_ = 0;
        aligned_stride_ = 0;
    }

    initialized_ = false;
    output_width_ = 0;
    output_height_ = 0;
    input_pix_fmt_ = -1;
}

bool VideoDecoder::decode(const uint8_t* data, size_t len, DecodedFrameCallback callback) {
    if (!initialized_) {
        LOG_ERROR("Decoder not initialized");
        return false;
    }

    // Set packet data
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(len);

    // Send packet to decoder
    int ret = avcodec_send_packet(codec_ctx_, packet_);
    if (ret < 0) {
        LOG_ERROR("Error sending packet to decoder: {}", ret);
        stats_.decode_errors++;
        return false;
    }

    // Receive decoded frames
    bool decoded = false;
    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LOG_ERROR("Error receiving frame from decoder: {}", ret);
            stats_.decode_errors++;
            break;
        }

        // Setup scaler if needed (first frame, resolution change, or format change)
        if (frame_->width != output_width_ || frame_->height != output_height_ ||
            frame_->format != input_pix_fmt_) {
            if (!setup_scaler(frame_->width, frame_->height, frame_->format)) {
                LOG_ERROR("Failed to setup scaler");
                continue;
            }
        }

        // Convert to RGB
        DecodedFrame output;
        if (convert_to_rgb(output)) {
            output.pts = frame_->pts;
            stats_.frames_decoded++;
            decoded = true;

            if (callback) {
                callback(output);
            }
        }
    }

    return decoded;
}

bool VideoDecoder::setup_scaler(int width, int height, int pix_fmt) {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    // Create scaler context using the actual decoded frame format -> BGRA (Cairo's native format)
    sws_ctx_ = sws_getContext(
        width, height, static_cast<AVPixelFormat>(pix_fmt),
        width, height, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) {
        LOG_ERROR("Failed to create scaler context");
        return false;
    }

    // Allocate aligned buffer for sws_scale output (NEON/SSE require 32-byte alignment)
    // Use stride aligned to 32 bytes for SIMD safety
    aligned_stride_ = (width * 4 + 31) & ~31;
    int buf_size = aligned_stride_ * height;

    if (buf_size != aligned_buf_size_) {
        if (aligned_buf_) av_free(aligned_buf_);
        aligned_buf_ = static_cast<uint8_t*>(av_malloc(buf_size));
        if (!aligned_buf_) {
            LOG_ERROR("Failed to allocate aligned buffer");
            aligned_buf_size_ = 0;
            return false;
        }
        aligned_buf_size_ = buf_size;
    }

    output_width_ = width;
    output_height_ = height;
    input_pix_fmt_ = pix_fmt;

    LOG_DEBUG("Scaler setup: {}x{} fmt={} (stride {})", width, height, pix_fmt, aligned_stride_);
    return true;
}

bool VideoDecoder::convert_to_rgb(DecodedFrame& output) {
    if (!sws_ctx_ || !aligned_buf_) {
        return false;
    }

    int width = frame_->width;
    int height = frame_->height;

    // sws_scale into the aligned buffer (safe for NEON/SSE)
    uint8_t* dst_data[4] = {aligned_buf_, nullptr, nullptr, nullptr};
    int dst_linesize[4] = {aligned_stride_, 0, 0, 0};

    int result = sws_scale(
        sws_ctx_,
        frame_->data, frame_->linesize,
        0, height,
        dst_data, dst_linesize
    );

    if (result != height) {
        LOG_ERROR("sws_scale returned {}, expected {}", result, height);
        return false;
    }

    // Copy from aligned buffer to output (BGRA, 4 bytes per pixel)
    int row_bytes = width * 4;
    output.rgb_data.resize(row_bytes * height);
    for (int y = 0; y < height; y++) {
        memcpy(output.rgb_data.data() + y * row_bytes,
               aligned_buf_ + y * aligned_stride_,
               row_bytes);
    }

    output.width = width;
    output.height = height;

    return true;
}

} // namespace baichuan
