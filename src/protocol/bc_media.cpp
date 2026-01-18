#include "protocol/bc_media.h"
#include "utils/logger.h"
#include <cstring>

namespace baichuan {

namespace {

uint32_t read_u32_le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t read_u16_le(const uint8_t* data) {
    return static_cast<uint16_t>(data[0]) |
           (static_cast<uint16_t>(data[1]) << 8);
}

VideoCodec parse_video_type(const uint8_t* data) {
    // Video type is a 4-byte ASCII string: "H264" or "H265"
    if (data[0] == 'H' && data[1] == '2' && data[2] == '6') {
        if (data[3] == '4') return VideoCodec::H264;
        if (data[3] == '5') return VideoCodec::H265;
    }
    // Default to H264 if unknown
    return VideoCodec::H264;
}

uint32_t calculate_padding(uint32_t size) {
    uint32_t remainder = size % BCMEDIA_PAD_SIZE;
    return remainder == 0 ? 0 : BCMEDIA_PAD_SIZE - remainder;
}

} // anonymous namespace

// BcMediaAac methods
std::optional<uint32_t> BcMediaAac::duration() const {
    if (data.size() < 8) return std::nullopt;

    // Check ADTS syncword
    if (data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return std::nullopt;
    }

    // Extract sample frequency index from ADTS header
    uint8_t freq_index = (data[2] & 0x3C) >> 2;

    static const uint32_t sample_rates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
        16000, 12000, 11025, 8000, 7350, 0, 0, 0
    };

    if (freq_index >= 13) return std::nullopt;
    uint32_t sample_rate = sample_rates[freq_index];
    if (sample_rate == 0) return std::nullopt;

    // Number of AAC frames (usually 1)
    uint8_t frames = (data[6] & 0x03) + 1;
    uint32_t samples = static_cast<uint32_t>(frames) * 1024;

    return (samples * 1000000) / sample_rate;
}

// BcMediaAdpcm methods
uint32_t BcMediaAdpcm::duration() const {
    uint32_t samples = block_size() * 2;
    constexpr uint32_t SAMPLE_RATE = 8000;
    return (samples * 1000000) / SAMPLE_RATE;
}

// BcMediaParser methods
bool BcMediaParser::is_bcmedia_magic(uint32_t magic) {
    return magic == MAGIC_BCMEDIA_INFO_V1 ||
           magic == MAGIC_BCMEDIA_INFO_V2 ||
           (magic >= MAGIC_BCMEDIA_IFRAME && magic <= MAGIC_BCMEDIA_IFRAME_LAST) ||
           (magic >= MAGIC_BCMEDIA_PFRAME && magic <= MAGIC_BCMEDIA_PFRAME_LAST) ||
           magic == MAGIC_BCMEDIA_AAC ||
           magic == MAGIC_BCMEDIA_ADPCM;
}

std::optional<std::pair<BcMediaFrame, size_t>> BcMediaParser::parse(const uint8_t* data, size_t len) {
    if (len < 4) return std::nullopt;

    uint32_t magic = read_u32_le(data);

    if (magic == MAGIC_BCMEDIA_INFO_V1 || magic == MAGIC_BCMEDIA_INFO_V2) {
        auto result = parse_info(data + 4, len - 4);
        if (result) {
            return std::make_pair(BcMediaFrame{result->first}, result->second + 4);
        }
    } else if (magic >= MAGIC_BCMEDIA_IFRAME && magic <= MAGIC_BCMEDIA_IFRAME_LAST) {
        auto result = parse_iframe(data + 4, len - 4);
        if (result) {
            return std::make_pair(BcMediaFrame{result->first}, result->second + 4);
        }
    } else if (magic >= MAGIC_BCMEDIA_PFRAME && magic <= MAGIC_BCMEDIA_PFRAME_LAST) {
        auto result = parse_pframe(data + 4, len - 4);
        if (result) {
            return std::make_pair(BcMediaFrame{result->first}, result->second + 4);
        }
    } else if (magic == MAGIC_BCMEDIA_AAC) {
        auto result = parse_aac(data + 4, len - 4);
        if (result) {
            return std::make_pair(BcMediaFrame{result->first}, result->second + 4);
        }
    } else if (magic == MAGIC_BCMEDIA_ADPCM) {
        auto result = parse_adpcm(data + 4, len - 4);
        if (result) {
            return std::make_pair(BcMediaFrame{result->first}, result->second + 4);
        }
    }

    return std::nullopt;
}

std::optional<std::pair<BcMediaInfo, size_t>> BcMediaParser::parse_info(const uint8_t* data, size_t len) {
    // Info header: 4 (header_size) + 4 (width) + 4 (height) + 1 (unknown) + 1 (fps)
    //            + 7 (start time) + 7 (end time) + 2 (unknown) = 32 bytes total after magic
    constexpr size_t INFO_SIZE = 32;

    if (len < INFO_SIZE) return std::nullopt;

    uint32_t header_size = read_u32_le(data);
    if (header_size != 32) {
        LOG_WARN("Unexpected info header size: {}", header_size);
    }

    BcMediaInfo info;
    info.video_width = read_u32_le(data + 4);
    info.video_height = read_u32_le(data + 8);
    // data[12] is unknown
    info.fps = data[13];
    info.start_year = data[14];
    info.start_month = data[15];
    info.start_day = data[16];
    info.start_hour = data[17];
    info.start_min = data[18];
    info.start_seconds = data[19];
    info.end_year = data[20];
    info.end_month = data[21];
    info.end_day = data[22];
    info.end_hour = data[23];
    info.end_min = data[24];
    info.end_seconds = data[25];
    // data[26-27] is unknown

    LOG_DEBUG("Parsed media info: {}x{} @ {} fps", info.video_width, info.video_height, info.fps);

    return std::make_pair(info, INFO_SIZE);
}

std::optional<std::pair<BcMediaIFrame, size_t>> BcMediaParser::parse_iframe(const uint8_t* data, size_t len) {
    // IFrame header: 4 (video_type) + 4 (payload_size) + 4 (additional_header_size)
    //              + 4 (microseconds) + 4 (unknown) + [additional header] + [data] + [padding]
    constexpr size_t MIN_HEADER = 20;

    if (len < MIN_HEADER) return std::nullopt;

    BcMediaIFrame frame;
    frame.codec = parse_video_type(data);

    uint32_t payload_size = read_u32_le(data + 4);
    uint32_t additional_header = read_u32_le(data + 8);
    frame.microseconds = read_u32_le(data + 12);
    // data[16-19] is unknown

    size_t header_consumed = 20;

    // Parse additional header (contains POSIX time if present)
    if (additional_header >= 4) {
        if (len < header_consumed + 4) return std::nullopt;
        frame.posix_time = read_u32_le(data + header_consumed);
        header_consumed += additional_header;
    }

    // Calculate total frame size
    uint32_t padding = calculate_padding(payload_size);
    size_t total_size = header_consumed + payload_size + padding;

    if (len < total_size) return std::nullopt;

    // Copy frame data
    frame.data.assign(data + header_consumed, data + header_consumed + payload_size);

    LOG_DEBUG("Parsed IFrame: {} bytes, codec={}", payload_size,
              frame.codec == VideoCodec::H264 ? "H264" : "H265");

    return std::make_pair(std::move(frame), total_size);
}

std::optional<std::pair<BcMediaPFrame, size_t>> BcMediaParser::parse_pframe(const uint8_t* data, size_t len) {
    // PFrame is similar to IFrame but without POSIX time
    constexpr size_t MIN_HEADER = 20;

    if (len < MIN_HEADER) return std::nullopt;

    BcMediaPFrame frame;
    frame.codec = parse_video_type(data);

    uint32_t payload_size = read_u32_le(data + 4);
    uint32_t additional_header = read_u32_le(data + 8);
    frame.microseconds = read_u32_le(data + 12);
    // data[16-19] is unknown

    size_t header_consumed = 20 + additional_header;

    // Calculate total frame size
    uint32_t padding = calculate_padding(payload_size);
    size_t total_size = header_consumed + payload_size + padding;

    if (len < total_size) return std::nullopt;

    // Copy frame data
    frame.data.assign(data + header_consumed, data + header_consumed + payload_size);

    return std::make_pair(std::move(frame), total_size);
}

std::optional<std::pair<BcMediaAac, size_t>> BcMediaParser::parse_aac(const uint8_t* data, size_t len) {
    // AAC header: 2 (payload_size) + 2 (payload_size_b) + [data] + [padding]
    constexpr size_t MIN_HEADER = 4;

    if (len < MIN_HEADER) return std::nullopt;

    uint16_t payload_size = read_u16_le(data);
    // data[2-3] is payload_size_b (same as payload_size)

    // Calculate padding - Rust code calculates on payload_size alone
    uint32_t padding = calculate_padding(payload_size);
    size_t total_size = 4 + payload_size + padding;  // header + data + padding

    if (len < total_size) return std::nullopt;

    BcMediaAac frame;
    frame.data.assign(data + 4, data + 4 + payload_size);

    return std::make_pair(std::move(frame), total_size);
}

std::optional<std::pair<BcMediaAdpcm, size_t>> BcMediaParser::parse_adpcm(const uint8_t* data, size_t len) {
    // ADPCM header: 2 (payload_size) + 2 (payload_size_b) + 2 (more_magic) + 2 (block_size) + [data]
    constexpr size_t MIN_HEADER = 8;

    if (len < MIN_HEADER) return std::nullopt;

    uint16_t payload_size = read_u16_le(data);
    // payload_size includes the 4 bytes of more_magic and block_size

    if (len < 4 + payload_size) return std::nullopt;

    BcMediaAdpcm frame;
    // Skip the inner header (more_magic + block_size = 4 bytes)
    // The actual ADPCM data starts after that
    frame.data.assign(data + 8, data + 4 + payload_size);

    return std::make_pair(std::move(frame), static_cast<size_t>(4 + payload_size));
}

BcMediaType BcMediaParser::get_type(const BcMediaFrame& frame) {
    return std::visit([](auto&& arg) -> BcMediaType {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, BcMediaInfo>) return BcMediaType::Info;
        else if constexpr (std::is_same_v<T, BcMediaIFrame>) return BcMediaType::IFrame;
        else if constexpr (std::is_same_v<T, BcMediaPFrame>) return BcMediaType::PFrame;
        else if constexpr (std::is_same_v<T, BcMediaAac>) return BcMediaType::Aac;
        else if constexpr (std::is_same_v<T, BcMediaAdpcm>) return BcMediaType::Adpcm;
        else return BcMediaType::Info;
    }, frame);
}

const char* BcMediaParser::type_name(BcMediaType type) {
    switch (type) {
        case BcMediaType::Info: return "Info";
        case BcMediaType::IFrame: return "IFrame";
        case BcMediaType::PFrame: return "PFrame";
        case BcMediaType::Aac: return "AAC";
        case BcMediaType::Adpcm: return "ADPCM";
        default: return "Unknown";
    }
}

bool BcMediaParser::is_video_frame(const BcMediaFrame& frame) {
    return std::holds_alternative<BcMediaIFrame>(frame) ||
           std::holds_alternative<BcMediaPFrame>(frame);
}

const std::vector<uint8_t>* BcMediaParser::get_video_data(const BcMediaFrame& frame) {
    if (auto* iframe = std::get_if<BcMediaIFrame>(&frame)) {
        return &iframe->data;
    }
    if (auto* pframe = std::get_if<BcMediaPFrame>(&frame)) {
        return &pframe->data;
    }
    return nullptr;
}

std::optional<VideoCodec> BcMediaParser::get_video_codec(const BcMediaFrame& frame) {
    if (auto* iframe = std::get_if<BcMediaIFrame>(&frame)) {
        return iframe->codec;
    }
    if (auto* pframe = std::get_if<BcMediaPFrame>(&frame)) {
        return pframe->codec;
    }
    return std::nullopt;
}

} // namespace baichuan
