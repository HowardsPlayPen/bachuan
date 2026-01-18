#pragma once

#include <cstdint>
#include <vector>
#include <variant>
#include <optional>
#include <string>

namespace baichuan {

// BcMedia magic headers
constexpr uint32_t MAGIC_BCMEDIA_INFO_V1 = 0x31303031;    // "1001"
constexpr uint32_t MAGIC_BCMEDIA_INFO_V2 = 0x32303031;    // "2001"
constexpr uint32_t MAGIC_BCMEDIA_IFRAME = 0x63643030;     // "cd00" base
constexpr uint32_t MAGIC_BCMEDIA_IFRAME_LAST = 0x63643039; // "cd09"
constexpr uint32_t MAGIC_BCMEDIA_PFRAME = 0x63643130;     // "cd10" base
constexpr uint32_t MAGIC_BCMEDIA_PFRAME_LAST = 0x63643139; // "cd19"
constexpr uint32_t MAGIC_BCMEDIA_AAC = 0x62773530;        // "bw50"
constexpr uint32_t MAGIC_BCMEDIA_ADPCM = 0x62773130;      // "bw10"

// Padding size for media packets
constexpr uint32_t BCMEDIA_PAD_SIZE = 8;

enum class VideoCodec {
    H264,
    H265
};

// Stream info sent at start of video stream
struct BcMediaInfo {
    uint32_t video_width = 0;
    uint32_t video_height = 0;
    uint8_t fps = 0;
    uint8_t start_year = 0;
    uint8_t start_month = 0;
    uint8_t start_day = 0;
    uint8_t start_hour = 0;
    uint8_t start_min = 0;
    uint8_t start_seconds = 0;
    uint8_t end_year = 0;
    uint8_t end_month = 0;
    uint8_t end_day = 0;
    uint8_t end_hour = 0;
    uint8_t end_min = 0;
    uint8_t end_seconds = 0;
};

// Video I-Frame (keyframe)
struct BcMediaIFrame {
    VideoCodec codec = VideoCodec::H264;
    uint32_t microseconds = 0;
    std::optional<uint32_t> posix_time;  // Seconds since epoch
    std::vector<uint8_t> data;
};

// Video P-Frame (delta frame)
struct BcMediaPFrame {
    VideoCodec codec = VideoCodec::H264;
    uint32_t microseconds = 0;
    std::vector<uint8_t> data;
};

// AAC audio frame
struct BcMediaAac {
    std::vector<uint8_t> data;

    // Get audio duration in microseconds from ADTS header
    std::optional<uint32_t> duration() const;
};

// ADPCM audio frame
struct BcMediaAdpcm {
    std::vector<uint8_t> data;

    // Block size (data length minus 4-byte header)
    uint32_t block_size() const { return data.size() > 4 ? static_cast<uint32_t>(data.size()) - 4 : 0; }

    // Get audio duration in microseconds (8000Hz sample rate)
    uint32_t duration() const;
};

// Variant type for all BcMedia frame types
using BcMediaFrame = std::variant<
    BcMediaInfo,
    BcMediaIFrame,
    BcMediaPFrame,
    BcMediaAac,
    BcMediaAdpcm
>;

// BcMedia frame type enumeration
enum class BcMediaType {
    Info,
    IFrame,
    PFrame,
    Aac,
    Adpcm
};

class BcMediaParser {
public:
    // Parse a BcMedia frame from data
    // Returns the parsed frame and the number of bytes consumed
    // Returns nullopt if not enough data available
    static std::optional<std::pair<BcMediaFrame, size_t>> parse(const uint8_t* data, size_t len);

    // Check if magic value is a valid BcMedia header
    static bool is_bcmedia_magic(uint32_t magic);

    // Get frame type from BcMediaFrame variant
    static BcMediaType get_type(const BcMediaFrame& frame);

    // Get frame type name as string
    static const char* type_name(BcMediaType type);

    // Check if frame is a video frame (IFrame or PFrame)
    static bool is_video_frame(const BcMediaFrame& frame);

    // Get video data from frame (returns nullptr for non-video frames)
    static const std::vector<uint8_t>* get_video_data(const BcMediaFrame& frame);

    // Get video codec from frame (returns nullopt for non-video frames)
    static std::optional<VideoCodec> get_video_codec(const BcMediaFrame& frame);

private:
    static std::optional<std::pair<BcMediaInfo, size_t>> parse_info(const uint8_t* data, size_t len);
    static std::optional<std::pair<BcMediaIFrame, size_t>> parse_iframe(const uint8_t* data, size_t len);
    static std::optional<std::pair<BcMediaPFrame, size_t>> parse_pframe(const uint8_t* data, size_t len);
    static std::optional<std::pair<BcMediaAac, size_t>> parse_aac(const uint8_t* data, size_t len);
    static std::optional<std::pair<BcMediaAdpcm, size_t>> parse_adpcm(const uint8_t* data, size_t len);
};

} // namespace baichuan
