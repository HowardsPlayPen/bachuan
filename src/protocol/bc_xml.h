#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include <vector>

namespace bachuan {

// Default XML version used in messages
constexpr const char* XML_VERSION = "1.1";

// XML structure for Encryption (received during login negotiation)
struct EncryptionXml {
    std::string version = XML_VERSION;
    std::string type;    // Usually "md5"
    std::string nonce;   // Random nonce from camera

    static std::optional<EncryptionXml> parse(const std::string& xml);
};

// XML structure for LoginUser (sent during modern login)
struct LoginUserXml {
    std::string version = XML_VERSION;
    std::string user_name;  // MD5(username + nonce) as hex
    std::string password;   // MD5(password + nonce) as hex
    uint32_t user_ver = 1;

    std::string serialize() const;
};

// XML structure for LoginNet (sent during modern login)
struct LoginNetXml {
    std::string version = XML_VERSION;
    std::string type = "LAN";
    uint16_t udp_port = 0;

    std::string serialize() const;
};

// XML structure for DeviceInfo (received after successful login)
struct DeviceInfoXml {
    std::optional<std::string> version;
    std::optional<uint32_t> resolution_width;
    std::optional<uint32_t> resolution_height;

    static std::optional<DeviceInfoXml> parse(const std::string& xml);
};

// XML structure for Preview (video stream request)
struct PreviewXml {
    std::string version = XML_VERSION;
    uint8_t channel_id = 0;
    uint32_t handle = 0;  // 0=main, 256=sub, 1024=extern
    std::string stream_type = "mainStream";  // mainStream, subStream, externStream

    std::string serialize() const;
};

// XML structure for Extension (metadata for payload)
struct ExtensionXml {
    std::string version = XML_VERSION;
    std::optional<uint32_t> binary_data;    // 1 if payload is binary
    std::optional<std::string> user_name;
    std::optional<std::string> token;
    std::optional<uint8_t> channel_id;
    std::optional<uint32_t> encrypt_len;    // Length of encrypted portion

    std::string serialize() const;
    static std::optional<ExtensionXml> parse(const std::string& xml);
};

// Combined login request body
struct LoginRequestXml {
    LoginUserXml login_user;
    LoginNetXml login_net;

    std::string serialize() const;
};

// Helper class for building and parsing XML
class BcXmlBuilder {
public:
    // Create login request XML
    static std::string create_login_request(const std::string& hashed_username,
                                            const std::string& hashed_password);

    // Create video stream start request XML
    static std::string create_preview_request(uint8_t channel_id,
                                              uint32_t handle,
                                              const std::string& stream_type);

    // Create extension XML for binary data
    static std::string create_binary_extension(uint8_t channel_id);

    // Parse encryption response to get nonce
    static std::optional<EncryptionXml> parse_encryption(const std::string& xml);

    // Parse device info response
    static std::optional<DeviceInfoXml> parse_device_info(const std::string& xml);

    // Parse extension from response
    static std::optional<ExtensionXml> parse_extension(const std::string& xml);

    // Generic XML parsing helper - extract text content of a tag
    static std::optional<std::string> extract_tag(const std::string& xml, const std::string& tag);
};

} // namespace bachuan
