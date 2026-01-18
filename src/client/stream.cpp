#include "client/stream.h"
#include "protocol/bc_xml.h"
#include "utils/logger.h"

namespace bachuan {

VideoStream::VideoStream(Connection& conn) : conn_(conn) {}

VideoStream::~VideoStream() {
    stop();
}

bool VideoStream::start(const StreamConfig& config) {
    if (streaming_.load()) {
        LOG_WARN("Stream already running");
        return false;
    }

    config_ = config;
    stats_ = Stats{};
    stream_info_received_ = false;

    LOG_INFO("Starting video stream: channel={}, handle={}, type={}",
             config_.channel_id, config_.handle, config_.stream_type);

    if (!send_start_request()) {
        LOG_ERROR("Failed to send stream start request");
        return false;
    }

    // Wait for initial response
    auto response = conn_.receive_message(5000);
    if (!response) {
        LOG_ERROR("No response to stream start request");
        return false;
    }

    if (response->header.response_code != RESPONSE_CODE_OK) {
        LOG_ERROR("Stream start rejected with code: {}", response->header.response_code);
        return false;
    }

    // Check if extension indicates binary data mode
    if (!response->extension_data.empty()) {
        std::string ext_xml(response->extension_data.begin(), response->extension_data.end());
        auto ext = BcXmlBuilder::parse_extension(ext_xml);
        if (ext && ext->binary_data && *ext->binary_data == 1) {
            std::lock_guard<std::mutex> lock(binary_mode_mutex_);
            binary_mode_nums_.insert(response->header.msg_num);
            LOG_DEBUG("Binary mode enabled for msg_num {}", response->header.msg_num);
        }
    }

    streaming_.store(true);

    // Start receive thread
    receive_thread_ = std::thread([this]() {
        receive_loop();
    });

    LOG_INFO("Video stream started");
    return true;
}

void VideoStream::stop() {
    if (!streaming_.load()) {
        return;
    }

    LOG_INFO("Stopping video stream");
    streaming_.store(false);

    // Send stop request (best effort)
    send_stop_request();

    // Wait for receive thread to finish
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(binary_mode_mutex_);
        binary_mode_nums_.clear();
    }

    LOG_INFO("Video stream stopped");
}

bool VideoStream::send_start_request() {
    std::string xml = BcXmlBuilder::create_preview_request(
        config_.channel_id,
        config_.handle,
        config_.stream_type
    );

    BcMessage msg = BcMessage::create_with_payload(
        MSG_ID_VIDEO,
        conn_.next_msg_num(),
        xml,
        MSG_CLASS_MODERN_24
    );

    return conn_.send_message(msg);
}

bool VideoStream::send_stop_request() {
    std::string xml = BcXmlBuilder::create_preview_request(
        config_.channel_id,
        config_.handle,
        config_.stream_type
    );

    BcMessage msg = BcMessage::create_with_payload(
        MSG_ID_VIDEO_STOP,
        conn_.next_msg_num(),
        xml,
        MSG_CLASS_MODERN_24
    );

    return conn_.send_message(msg);
}

void VideoStream::receive_loop() {
    LOG_DEBUG("Receive loop started");

    while (streaming_.load()) {
        auto msg = conn_.receive_message(1000);
        if (!msg) {
            // Timeout is OK, just continue
            continue;
        }

        process_message(*msg);
    }

    LOG_DEBUG("Receive loop ended");
}

void VideoStream::process_message(const BcMessage& msg) {
    if (msg.header.msg_id != MSG_ID_VIDEO) {
        LOG_DEBUG("Ignoring non-video message: {}", BcHeader::msg_id_name(msg.header.msg_id));
        return;
    }

    // Check if this message indicates binary mode
    if (!msg.extension_data.empty()) {
        std::string ext_xml(msg.extension_data.begin(), msg.extension_data.end());
        auto ext = BcXmlBuilder::parse_extension(ext_xml);
        if (ext && ext->binary_data && *ext->binary_data == 1) {
            std::lock_guard<std::mutex> lock(binary_mode_mutex_);
            binary_mode_nums_.insert(msg.header.msg_num);
        }
    }

    // Process payload data as BcMedia
    if (!msg.payload_data.empty()) {
        process_media_data(msg.payload_data);
    }
}

void VideoStream::process_media_data(const std::vector<uint8_t>& data) {
    // Append new data to buffer
    media_buffer_.insert(media_buffer_.end(), data.begin(), data.end());

    // Debug: print first bytes of incoming data (only once when buffer is small)
    if (media_buffer_.size() == data.size() && data.size() >= 32) {
        std::string hex;
        for (size_t i = 0; i < 32 && i < data.size(); i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", data[i]);
            hex += buf;
        }
        LOG_DEBUG("First 32 bytes of video data: {}", hex);
    }

    // Try to parse complete frames from buffer
    size_t offset = 0;

    while (offset < media_buffer_.size()) {
        // Check if remaining data starts with a valid magic
        if (media_buffer_.size() - offset < 4) {
            break;  // Need more data
        }

        uint32_t magic = static_cast<uint32_t>(media_buffer_[offset]) |
                        (static_cast<uint32_t>(media_buffer_[offset + 1]) << 8) |
                        (static_cast<uint32_t>(media_buffer_[offset + 2]) << 16) |
                        (static_cast<uint32_t>(media_buffer_[offset + 3]) << 24);

        if (!BcMediaParser::is_bcmedia_magic(magic)) {
            // Unknown magic - skip one byte and try to resync
            char magic_str[64];
            snprintf(magic_str, sizeof(magic_str), "0x%08x bytes: %02x %02x %02x %02x",
                     magic,
                     media_buffer_[offset], media_buffer_[offset + 1],
                     media_buffer_[offset + 2], media_buffer_[offset + 3]);
            LOG_WARN("Unknown magic {} at offset {}", magic_str, offset);
            offset++;
            continue;
        }

        auto result = BcMediaParser::parse(media_buffer_.data() + offset, media_buffer_.size() - offset);
        if (!result) {
            // Not enough data for complete frame - wait for more
            char magic_str[32];
            snprintf(magic_str, sizeof(magic_str), "0x%08x", magic);
            LOG_DEBUG("Incomplete frame at offset {}, waiting for more data (buffer size: {}, magic: {})",
                     offset, media_buffer_.size() - offset, magic_str);
            break;
        }

        const auto& [frame, consumed] = *result;
        offset += consumed;
        stats_.frames_received++;
        stats_.bytes_received += consumed;

        // Process frame based on type
        std::visit([this](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr (std::is_same_v<T, BcMediaInfo>) {
                LOG_INFO("Stream info: {}x{} @ {} fps",
                        arg.video_width, arg.video_height, arg.fps);
                stream_info_ = arg;
                stream_info_received_ = true;
                if (stream_info_callback_) {
                    stream_info_callback_(arg);
                }
            }
            else if constexpr (std::is_same_v<T, BcMediaIFrame>) {
                stats_.i_frames++;
                LOG_INFO("IFrame received: {} bytes, {} codec",
                         arg.data.size(),
                         arg.codec == VideoCodec::H264 ? "H264" : "H265");
            }
            else if constexpr (std::is_same_v<T, BcMediaPFrame>) {
                stats_.p_frames++;
                if (stats_.p_frames <= 3) {
                    LOG_DEBUG("PFrame received: {} bytes", arg.data.size());
                }
            }
            else if constexpr (std::is_same_v<T, BcMediaAac>) {
                // Audio frames are frequent, only log at trace level
            }
            else if constexpr (std::is_same_v<T, BcMediaAdpcm>) {
                // Audio frames are frequent, only log at trace level
            }
        }, frame);

        // Call frame callback
        if (frame_callback_) {
            frame_callback_(frame);
        }
    }

    // Remove consumed data from buffer
    if (offset > 0) {
        media_buffer_.erase(media_buffer_.begin(), media_buffer_.begin() + offset);
    }
}

} // namespace bachuan
