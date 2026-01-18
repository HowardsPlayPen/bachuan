#include "video/writer.h"
#include "utils/logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace bachuan {

// ImageWriter implementation

bool ImageWriter::save_jpeg(const DecodedFrame& frame, const std::string& filename, int quality) {
    if (frame.rgb_data.empty() || frame.width <= 0 || frame.height <= 0) {
        LOG_ERROR("Invalid frame data for JPEG save");
        return false;
    }

    // Find MJPEG encoder
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        LOG_ERROR("MJPEG encoder not found");
        return false;
    }

    // Allocate codec context
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        LOG_ERROR("Failed to allocate JPEG codec context");
        return false;
    }

    // Configure encoder
    ctx->width = frame.width;
    ctx->height = frame.height;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;  // JPEG uses YUVJ format
    ctx->time_base = {1, 1};

    // Set quality (1-31, lower is better for FFmpeg)
    // Convert our 0-100 quality to FFmpeg's qmin/qmax
    int q = 31 - (quality * 30 / 100);
    if (q < 1) q = 1;
    if (q > 31) q = 31;
    ctx->qmin = q;
    ctx->qmax = q;

    // Open encoder
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        LOG_ERROR("Failed to open JPEG encoder");
        avcodec_free_context(&ctx);
        return false;
    }

    // Create scaler for RGB24 to YUVJ420P
    SwsContext* sws = sws_getContext(
        frame.width, frame.height, AV_PIX_FMT_RGB24,
        frame.width, frame.height, AV_PIX_FMT_YUVJ420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws) {
        LOG_ERROR("Failed to create scaler for JPEG");
        avcodec_free_context(&ctx);
        return false;
    }

    // Allocate frame and packet
    AVFrame* av_frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();

    if (!av_frame || !packet) {
        LOG_ERROR("Failed to allocate frame/packet for JPEG");
        if (av_frame) av_frame_free(&av_frame);
        if (packet) av_packet_free(&packet);
        sws_freeContext(sws);
        avcodec_free_context(&ctx);
        return false;
    }

    // Setup frame
    av_frame->format = AV_PIX_FMT_YUVJ420P;
    av_frame->width = frame.width;
    av_frame->height = frame.height;

    if (av_frame_get_buffer(av_frame, 0) < 0) {
        LOG_ERROR("Failed to allocate JPEG frame buffer");
        av_frame_free(&av_frame);
        av_packet_free(&packet);
        sws_freeContext(sws);
        avcodec_free_context(&ctx);
        return false;
    }

    // Convert RGB to YUV
    const uint8_t* src_data[4] = {frame.rgb_data.data(), nullptr, nullptr, nullptr};
    int src_linesize[4] = {frame.width * 3, 0, 0, 0};

    sws_scale(sws, src_data, src_linesize, 0, frame.height,
              av_frame->data, av_frame->linesize);

    // Encode frame
    bool success = false;
    int ret = avcodec_send_frame(ctx, av_frame);
    if (ret >= 0) {
        ret = avcodec_receive_packet(ctx, packet);
        if (ret >= 0) {
            // Write to file
            FILE* f = fopen(filename.c_str(), "wb");
            if (f) {
                fwrite(packet->data, 1, packet->size, f);
                fclose(f);
                success = true;
                LOG_INFO("Saved JPEG: {} ({}x{})", filename, frame.width, frame.height);
            } else {
                LOG_ERROR("Failed to open file for writing: {}", filename);
            }
        } else {
            LOG_ERROR("Failed to encode JPEG: {}", ret);
        }
    } else {
        LOG_ERROR("Failed to send frame to JPEG encoder: {}", ret);
    }

    // Cleanup
    av_packet_free(&packet);
    av_frame_free(&av_frame);
    sws_freeContext(sws);
    avcodec_free_context(&ctx);

    return success;
}

// VideoWriter implementation

VideoWriter::VideoWriter() = default;

VideoWriter::~VideoWriter() {
    close();
}

bool VideoWriter::open(const std::string& filename, int width, int height, int fps) {
    if (is_open_) {
        close();
    }

    filename_ = filename;
    width_ = width;
    height_ = height;

    // Allocate format context
    int ret = avformat_alloc_output_context2(&fmt_ctx_, nullptr, nullptr, filename.c_str());
    if (ret < 0 || !fmt_ctx_) {
        LOG_ERROR("Failed to create output context for: {}", filename);
        return false;
    }

    // Find encoder - prefer H264, fallback to MPEG4
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOG_WARN("H264 encoder not found, trying MPEG4");
        codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    }
    if (!codec) {
        LOG_ERROR("No suitable video encoder found");
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    // Create stream
    stream_ = avformat_new_stream(fmt_ctx_, nullptr);
    if (!stream_) {
        LOG_ERROR("Failed to create video stream");
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    // Allocate codec context
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        LOG_ERROR("Failed to allocate codec context");
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
        return false;
    }

    // Configure encoder
    codec_ctx_->width = width;
    codec_ctx_->height = height;
    codec_ctx_->time_base = {1, fps};
    codec_ctx_->framerate = {fps, 1};
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_->gop_size = 12;
    codec_ctx_->max_b_frames = 2;

    // Set bitrate
    codec_ctx_->bit_rate = 4000000;  // 4 Mbps

    // For H264 specific options
    if (codec->id == AV_CODEC_ID_H264) {
        av_opt_set(codec_ctx_->priv_data, "preset", "medium", 0);
        av_opt_set(codec_ctx_->priv_data, "crf", "23", 0);
    }

    // Some formats require global header
    if (fmt_ctx_->oformat->flags & AVFMT_GLOBALHEADER) {
        codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // Open encoder
    ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to open video encoder: {}", ret);
        avcodec_free_context(&codec_ctx_);
        avformat_free_context(fmt_ctx_);
        codec_ctx_ = nullptr;
        fmt_ctx_ = nullptr;
        return false;
    }

    // Copy codec parameters to stream
    ret = avcodec_parameters_from_context(stream_->codecpar, codec_ctx_);
    if (ret < 0) {
        LOG_ERROR("Failed to copy codec parameters: {}", ret);
        close();
        return false;
    }

    stream_->time_base = codec_ctx_->time_base;

    // Allocate frame and packet
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();

    if (!frame_ || !packet_) {
        LOG_ERROR("Failed to allocate frame/packet");
        close();
        return false;
    }

    frame_->format = codec_ctx_->pix_fmt;
    frame_->width = width;
    frame_->height = height;

    ret = av_frame_get_buffer(frame_, 0);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate frame buffer: {}", ret);
        close();
        return false;
    }

    // Create scaler for RGB24 to YUV420P
    sws_ctx_ = sws_getContext(
        width, height, AV_PIX_FMT_RGB24,
        width, height, AV_PIX_FMT_YUV420P,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!sws_ctx_) {
        LOG_ERROR("Failed to create scaler");
        close();
        return false;
    }

    // Open output file
    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fmt_ctx_->pb, filename.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOG_ERROR("Failed to open output file: {}", filename);
            close();
            return false;
        }
    }

    // Write header
    ret = avformat_write_header(fmt_ctx_, nullptr);
    if (ret < 0) {
        LOG_ERROR("Failed to write header: {}", ret);
        close();
        return false;
    }

    is_open_ = true;
    pts_ = 0;
    frames_written_ = 0;

    LOG_INFO("Opened video file: {} ({}x{} @ {} fps, codec: {})",
             filename, width, height, fps, codec->name);

    return true;
}

void VideoWriter::close() {
    if (is_open_ && fmt_ctx_) {
        // Flush encoder
        encode_frame(nullptr);

        // Write trailer
        av_write_trailer(fmt_ctx_);
    }

    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }

    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    if (frame_) {
        av_frame_free(&frame_);
        frame_ = nullptr;
    }

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }

    if (fmt_ctx_) {
        if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmt_ctx_->pb);
        }
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    if (is_open_) {
        LOG_INFO("Closed video file: {} ({} frames written)", filename_, frames_written_);
    }

    is_open_ = false;
    stream_ = nullptr;
}

bool VideoWriter::write_frame(const DecodedFrame& frame) {
    if (!is_open_) {
        LOG_ERROR("Video writer not open");
        return false;
    }

    if (frame.width != width_ || frame.height != height_) {
        LOG_ERROR("Frame size mismatch: {}x{} vs expected {}x{}",
                  frame.width, frame.height, width_, height_);
        return false;
    }

    // Make frame writable
    int ret = av_frame_make_writable(frame_);
    if (ret < 0) {
        LOG_ERROR("Failed to make frame writable: {}", ret);
        return false;
    }

    // Convert RGB to YUV
    const uint8_t* src_data[4] = {frame.rgb_data.data(), nullptr, nullptr, nullptr};
    int src_linesize[4] = {frame.width * 3, 0, 0, 0};

    sws_scale(sws_ctx_, src_data, src_linesize, 0, frame.height,
              frame_->data, frame_->linesize);

    frame_->pts = pts_++;

    return encode_frame(frame_);
}

bool VideoWriter::encode_frame(AVFrame* frame) {
    int ret = avcodec_send_frame(codec_ctx_, frame);
    if (ret < 0) {
        LOG_ERROR("Error sending frame to encoder: {}", ret);
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(codec_ctx_, packet_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            LOG_ERROR("Error receiving packet from encoder: {}", ret);
            return false;
        }

        // Rescale timestamps
        av_packet_rescale_ts(packet_, codec_ctx_->time_base, stream_->time_base);
        packet_->stream_index = stream_->index;

        // Write packet
        ret = av_interleaved_write_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            LOG_ERROR("Error writing packet: {}", ret);
            return false;
        }

        frames_written_++;
    }

    return true;
}

} // namespace bachuan
