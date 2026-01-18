#include "video/decoder.h"
#include "utils/logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace baichuan {

VideoDecoder::VideoDecoder() = default;

VideoDecoder::~VideoDecoder() {
    shutdown();
}

bool VideoDecoder::init(VideoCodec codec) {
    if (initialized_) {
        shutdown();
    }

    codec_ = codec;

    // Find decoder
    AVCodecID codec_id = (codec == VideoCodec::H265) ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
    const AVCodec* decoder = avcodec_find_decoder(codec_id);
    if (!decoder) {
        LOG_ERROR("Failed to find {} decoder", codec == VideoCodec::H265 ? "H265" : "H264");
        return false;
    }

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(decoder);
    if (!codec_ctx_) {
        LOG_ERROR("Failed to allocate codec context");
        return false;
    }

    // Set options for low latency
    codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

    // Open codec
    if (avcodec_open2(codec_ctx_, decoder, nullptr) < 0) {
        LOG_ERROR("Failed to open codec");
        avcodec_free_context(&codec_ctx_);
        return false;
    }

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

    LOG_INFO("Video decoder initialized: {}", codec == VideoCodec::H265 ? "H265" : "H264");
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

    initialized_ = false;
    output_width_ = 0;
    output_height_ = 0;
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

        // Setup scaler if needed (first frame or resolution change)
        if (frame_->width != output_width_ || frame_->height != output_height_) {
            if (!setup_scaler(frame_->width, frame_->height)) {
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

bool VideoDecoder::setup_scaler(int width, int height) {
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    // Create scaler context for YUV420P to RGB24 conversion
    sws_ctx_ = sws_getContext(
        width, height, codec_ctx_->pix_fmt,
        width, height, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) {
        LOG_ERROR("Failed to create scaler context");
        return false;
    }

    // Setup RGB frame buffer
    int rgb_buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
    if (rgb_buf_size < 0) {
        LOG_ERROR("Failed to get RGB buffer size");
        return false;
    }

    output_width_ = width;
    output_height_ = height;

    LOG_DEBUG("Scaler setup: {}x{}", width, height);
    return true;
}

bool VideoDecoder::convert_to_rgb(DecodedFrame& output) {
    if (!sws_ctx_) {
        return false;
    }

    int width = frame_->width;
    int height = frame_->height;

    // Allocate RGB buffer
    int rgb_buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
    output.rgb_data.resize(rgb_buf_size);

    // Setup destination frame
    uint8_t* dst_data[4] = {output.rgb_data.data(), nullptr, nullptr, nullptr};
    int dst_linesize[4] = {width * 3, 0, 0, 0};

    // Convert YUV to RGB
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

    output.width = width;
    output.height = height;

    return true;
}

} // namespace baichuan
