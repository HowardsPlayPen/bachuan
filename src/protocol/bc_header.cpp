#include "protocol/bc_header.h"
#include "utils/logger.h"
#include <cstring>

namespace bachuan {

namespace {

// Helper functions for little-endian serialization
void write_u32_le(std::vector<uint8_t>& buf, uint32_t value) {
    buf.push_back(static_cast<uint8_t>(value & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void write_u16_le(std::vector<uint8_t>& buf, uint16_t value) {
    buf.push_back(static_cast<uint8_t>(value & 0xFF));
    buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

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

} // anonymous namespace

std::vector<uint8_t> BcHeader::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(header_size());

    write_u32_le(buf, magic);
    write_u32_le(buf, msg_id);
    write_u32_le(buf, body_len);
    buf.push_back(channel_id);
    buf.push_back(stream_type);
    write_u16_le(buf, msg_num);
    write_u16_le(buf, response_code);
    write_u16_le(buf, msg_class);

    if (is_modern_with_offset()) {
        write_u32_le(buf, payload_offset.value_or(0));
    }

    return buf;
}

size_t BcHeader::deserialize(const uint8_t* data, size_t len, BcHeader& header) {
    if (len < HEADER_SIZE_20) {
        LOG_ERROR("Not enough data for header: {} bytes", len);
        return 0;
    }

    header.magic = read_u32_le(data);
    if (header.magic != MAGIC_HEADER && header.magic != MAGIC_HEADER_REV) {
        LOG_ERROR("Invalid magic header: 0x{:08x}", header.magic);
        return 0;
    }

    header.msg_id = read_u32_le(data + 4);
    header.body_len = read_u32_le(data + 8);
    header.channel_id = data[12];
    header.stream_type = data[13];
    header.msg_num = read_u16_le(data + 14);
    header.response_code = read_u16_le(data + 16);
    header.msg_class = read_u16_le(data + 18);

    // Check if this is a 24-byte header
    if (header.is_modern_with_offset()) {
        if (len < HEADER_SIZE_24) {
            LOG_ERROR("Not enough data for 24-byte header: {} bytes", len);
            return 0;
        }
        header.payload_offset = read_u32_le(data + 20);
        return HEADER_SIZE_24;
    }

    header.payload_offset = std::nullopt;
    return HEADER_SIZE_20;
}

const char* BcHeader::msg_id_name(uint32_t msg_id) {
    switch (msg_id) {
        case MSG_ID_LOGIN: return "Login";
        case MSG_ID_LOGOUT: return "Logout";
        case MSG_ID_VIDEO: return "Video";
        case MSG_ID_VIDEO_STOP: return "VideoStop";
        case MSG_ID_TALKABILITY: return "TalkAbility";
        case MSG_ID_TALKRESET: return "TalkReset";
        case MSG_ID_PTZ_CONTROL: return "PtzControl";
        case MSG_ID_REBOOT: return "Reboot";
        case MSG_ID_MOTION_REQUEST: return "MotionRequest";
        case MSG_ID_MOTION: return "Motion";
        case MSG_ID_VERSION: return "Version";
        case MSG_ID_PING: return "Ping";
        case MSG_ID_GET_GENERAL: return "GetGeneral";
        case MSG_ID_SNAP: return "Snap";
        case MSG_ID_UID: return "Uid";
        case MSG_ID_STREAM_INFO_LIST: return "StreamInfoList";
        case MSG_ID_ABILITY_INFO: return "AbilityInfo";
        case MSG_ID_GET_SUPPORT: return "GetSupport";
        default: return "Unknown";
    }
}

BcMessage BcMessage::create_header_only(uint32_t msg_id, uint16_t msg_num, uint16_t msg_class) {
    BcMessage msg;
    msg.header.msg_id = msg_id;
    msg.header.msg_num = msg_num;
    msg.header.msg_class = msg_class;
    msg.header.body_len = 0;
    if (msg.header.is_modern_with_offset()) {
        msg.header.payload_offset = 0;
    }
    return msg;
}

BcMessage BcMessage::create_with_payload(uint32_t msg_id, uint16_t msg_num,
                                         const std::string& xml_payload,
                                         uint16_t msg_class) {
    BcMessage msg;
    msg.header.msg_id = msg_id;
    msg.header.msg_num = msg_num;
    msg.header.msg_class = msg_class;

    // Payload data is the XML string
    msg.payload_data.assign(xml_payload.begin(), xml_payload.end());
    msg.header.body_len = static_cast<uint32_t>(msg.payload_data.size());

    if (msg.header.is_modern_with_offset()) {
        // No extension, payload starts at offset 0
        msg.header.payload_offset = 0;
    }

    return msg;
}

BcMessage BcMessage::create_with_extension(uint32_t msg_id, uint16_t msg_num,
                                           const std::string& extension_xml,
                                           const std::vector<uint8_t>& payload,
                                           uint16_t msg_class) {
    BcMessage msg;
    msg.header.msg_id = msg_id;
    msg.header.msg_num = msg_num;
    msg.header.msg_class = msg_class;

    // Extension data
    msg.extension_data.assign(extension_xml.begin(), extension_xml.end());

    // Payload data
    msg.payload_data = payload;

    // Total body length
    msg.header.body_len = static_cast<uint32_t>(msg.extension_data.size() + msg.payload_data.size());

    if (msg.header.is_modern_with_offset()) {
        msg.header.payload_offset = static_cast<uint32_t>(msg.extension_data.size());
    }

    return msg;
}

std::vector<uint8_t> BcMessage::serialize() const {
    std::vector<uint8_t> buf = header.serialize();

    // Append extension data
    buf.insert(buf.end(), extension_data.begin(), extension_data.end());

    // Append payload data
    buf.insert(buf.end(), payload_data.begin(), payload_data.end());

    return buf;
}

} // namespace bachuan
