#pragma once

#include <cstdint>
#include <vector>
#include <optional>
#include <string>

namespace baichuan {

// Magic header values
constexpr uint32_t MAGIC_HEADER = 0x0abcdef0;
constexpr uint32_t MAGIC_HEADER_REV = 0x0fedcba0;  // Big-endian variant

// Message IDs
constexpr uint32_t MSG_ID_LOGIN = 1;
constexpr uint32_t MSG_ID_LOGOUT = 2;
constexpr uint32_t MSG_ID_VIDEO = 3;
constexpr uint32_t MSG_ID_VIDEO_STOP = 4;
constexpr uint32_t MSG_ID_TALKABILITY = 10;
constexpr uint32_t MSG_ID_TALKRESET = 11;
constexpr uint32_t MSG_ID_PTZ_CONTROL = 18;
constexpr uint32_t MSG_ID_REBOOT = 23;
constexpr uint32_t MSG_ID_MOTION_REQUEST = 31;
constexpr uint32_t MSG_ID_MOTION = 33;
constexpr uint32_t MSG_ID_VERSION = 80;
constexpr uint32_t MSG_ID_PING = 93;
constexpr uint32_t MSG_ID_GET_GENERAL = 104;
constexpr uint32_t MSG_ID_SNAP = 109;
constexpr uint32_t MSG_ID_UID = 114;
constexpr uint32_t MSG_ID_STREAM_INFO_LIST = 146;
constexpr uint32_t MSG_ID_ABILITY_INFO = 151;
constexpr uint32_t MSG_ID_GET_SUPPORT = 199;

// Message classes
constexpr uint16_t MSG_CLASS_LEGACY = 0x6514;    // Legacy 20-byte header
constexpr uint16_t MSG_CLASS_MODERN_20 = 0x6614; // Modern 20-byte header
constexpr uint16_t MSG_CLASS_MODERN_24 = 0x6414; // Modern 24-byte header
constexpr uint16_t MSG_CLASS_MODERN_24_ALT = 0x0000; // Modern 24-byte header (alternative)

// Header sizes
constexpr size_t HEADER_SIZE_20 = 20;
constexpr size_t HEADER_SIZE_24 = 24;

// Response codes
constexpr uint16_t RESPONSE_CODE_OK = 200;
constexpr uint16_t RESPONSE_CODE_BAD_REQUEST = 400;

// Encryption negotiation codes (request)
constexpr uint16_t ENC_REQ_NONE = 0xdc00;
constexpr uint16_t ENC_REQ_BC = 0xdc01;
constexpr uint16_t ENC_REQ_AES = 0xdc12;

// Encryption negotiation codes (response)
constexpr uint16_t ENC_RESP_NONE = 0xdd00;
constexpr uint16_t ENC_RESP_BC = 0xdd01;
constexpr uint16_t ENC_RESP_AES = 0xdd02;
constexpr uint16_t ENC_RESP_FULL_AES = 0xdd12;

// Stream types
enum class StreamType : uint8_t {
    MainStream = 0,
    SubStream = 1,
    ExternStream = 2
};

// Stream handles (used in video request)
constexpr uint32_t STREAM_HANDLE_MAIN = 0;
constexpr uint32_t STREAM_HANDLE_SUB = 256;
constexpr uint32_t STREAM_HANDLE_EXTERN = 1024;

struct BcHeader {
    uint32_t magic = MAGIC_HEADER;
    uint32_t msg_id = 0;
    uint32_t body_len = 0;
    uint8_t channel_id = 0;
    uint8_t stream_type = 0;
    uint16_t msg_num = 0;
    uint16_t response_code = 0;
    uint16_t msg_class = MSG_CLASS_MODERN_24;
    std::optional<uint32_t> payload_offset;

    // Determine header size based on message class
    size_t header_size() const {
        if (msg_class == MSG_CLASS_MODERN_24 || msg_class == MSG_CLASS_MODERN_24_ALT) {
            return HEADER_SIZE_24;
        }
        return HEADER_SIZE_20;
    }

    // Check if this is a modern message (has payload offset field)
    bool is_modern_with_offset() const {
        return msg_class == MSG_CLASS_MODERN_24 || msg_class == MSG_CLASS_MODERN_24_ALT;
    }

    // Serialize header to bytes
    std::vector<uint8_t> serialize() const;

    // Deserialize header from bytes (returns bytes consumed or 0 on error)
    static size_t deserialize(const uint8_t* data, size_t len, BcHeader& header);

    // Get a descriptive name for the message ID
    static const char* msg_id_name(uint32_t msg_id);
};

// Top-level message structure
struct BcMessage {
    BcHeader header;
    std::vector<uint8_t> extension_data;  // Data before payload_offset (XML extension)
    std::vector<uint8_t> payload_data;    // Data after payload_offset (XML or binary)

    // Create a header-only message
    static BcMessage create_header_only(uint32_t msg_id, uint16_t msg_num,
                                        uint16_t msg_class = MSG_CLASS_MODERN_24);

    // Create a message with XML payload
    static BcMessage create_with_payload(uint32_t msg_id, uint16_t msg_num,
                                         const std::string& xml_payload,
                                         uint16_t msg_class = MSG_CLASS_MODERN_24);

    // Create a message with extension and payload
    static BcMessage create_with_extension(uint32_t msg_id, uint16_t msg_num,
                                           const std::string& extension_xml,
                                           const std::vector<uint8_t>& payload,
                                           uint16_t msg_class = MSG_CLASS_MODERN_24);

    // Serialize the complete message
    std::vector<uint8_t> serialize() const;
};

} // namespace baichuan
