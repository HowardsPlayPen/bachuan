#include "client/auth.h"
#include "utils/md5.h"
#include "utils/logger.h"

namespace baichuan {

Authenticator::Authenticator(Connection& conn) : conn_(conn) {}

LoginResult Authenticator::login(const std::string& username, const std::string& password,
                                  MaxEncryption max_encryption) {
    LoginResult result;
    max_encryption_ = max_encryption;

    LOG_INFO("Starting login for user: {} (max encryption: {})", username,
             max_encryption == MaxEncryption::None ? "none" :
             max_encryption == MaxEncryption::BCEncrypt ? "bc" : "aes");

    // Get a message number to use for the entire login sequence
    login_msg_num_ = conn_.next_msg_num();

    // Step 1: Send legacy login request
    if (!send_legacy_login()) {
        result.error_message = "Failed to send legacy login request";
        LOG_ERROR("{}", result.error_message);
        return result;
    }

    // Step 2: Receive encryption negotiation
    auto negotiation = receive_encryption_negotiation();
    if (!negotiation) {
        result.error_message = "Failed to receive encryption negotiation";
        LOG_ERROR("{}", result.error_message);
        return result;
    }

    LOG_INFO("Encryption negotiated: type={}, nonce={}",
             static_cast<int>(negotiation->type), negotiation->nonce);

    result.encryption_type = negotiation->type;

    // IMPORTANT: During login (msg_id == 1), the protocol uses BCEncrypt even when AES is negotiated.
    // The AES key is derived here but only applied AFTER the login succeeds.
    // This matches the Rust neolink behavior in codex.rs lines 38-47.

    std::array<uint8_t, 16> aes_key{};
    bool use_aes_after_login = false;
    bool use_full_aes = false;

    if (negotiation->type == EncryptionType::BCEncrypt) {
        // BCEncrypt - use it for login and all subsequent messages
        BcCrypto crypto;
        crypto.set_bc_encrypt();
        conn_.set_encryption(std::move(crypto));
    } else if (negotiation->type == EncryptionType::Aes ||
               negotiation->type == EncryptionType::FullAes) {
        // AES negotiated - but use BCEncrypt for the login message itself
        // Save the AES key to apply after login succeeds
        aes_key = BcCrypto::derive_aes_key(password, negotiation->nonce);
        use_aes_after_login = true;
        use_full_aes = (negotiation->type == EncryptionType::FullAes);

        // Use BCEncrypt for the login message
        BcCrypto crypto;
        crypto.set_bc_encrypt();
        conn_.set_encryption(std::move(crypto));
        LOG_INFO("Using BCEncrypt for login message, will switch to AES after login");
    }

    // Reset encryption offsets after setting up encryption
    conn_.reset_encryption_offsets();

    // Step 3: Send modern login with hashed credentials
    if (!send_modern_login(username, password, negotiation->nonce)) {
        result.error_message = "Failed to send modern login request";
        LOG_ERROR("{}", result.error_message);
        return result;
    }

    // Receive login response
    auto device_info = receive_login_response();
    if (!device_info) {
        result.error_message = "Login failed - invalid credentials or connection error";
        LOG_ERROR("{}", result.error_message);
        return result;
    }

    // Now switch to AES if that's what was negotiated
    if (use_aes_after_login) {
        BcCrypto crypto;
        if (use_full_aes) {
            crypto.set_full_aes(aes_key);
            LOG_INFO("Switched to Full AES encryption for subsequent messages");
        } else {
            crypto.set_aes(aes_key);
            LOG_INFO("Switched to AES encryption for subsequent messages");
        }
        conn_.set_encryption(std::move(crypto));
        conn_.reset_encryption_offsets();
    }

    result.success = true;
    result.device_info = device_info;
    LOG_INFO("Login successful!");

    return result;
}

bool Authenticator::send_legacy_login() {
    // Create legacy login message to negotiate encryption
    // This is a minimal message with class 0x6514 (legacy)
    BcMessage msg;
    msg.header.magic = MAGIC_HEADER;
    msg.header.msg_id = MSG_ID_LOGIN;
    msg.header.msg_class = MSG_CLASS_LEGACY;
    msg.header.msg_num = login_msg_num_;

    // Set encryption request based on max_encryption_
    switch (max_encryption_) {
        case MaxEncryption::None:
            msg.header.response_code = ENC_REQ_NONE;
            break;
        case MaxEncryption::BCEncrypt:
            msg.header.response_code = ENC_REQ_BC;
            break;
        case MaxEncryption::Aes:
            msg.header.response_code = ENC_REQ_AES;
            break;
    }

    msg.header.body_len = 0;

    return conn_.send_message(msg);
}

std::optional<Authenticator::EncryptionNegotiation> Authenticator::receive_encryption_negotiation() {
    // Some cameras send unsolicited messages; skip them and wait for login response
    std::optional<BcMessage> msg;
    for (int attempts = 0; attempts < 5; attempts++) {
        msg = conn_.receive_message(10000);
        if (!msg) {
            LOG_ERROR("No response to legacy login");
            return std::nullopt;
        }

        if (msg->header.msg_id == MSG_ID_LOGIN) {
            break;
        }
        LOG_DEBUG("Skipping unexpected message ID {} during negotiation", msg->header.msg_id);
        msg.reset();
    }

    if (!msg) {
        LOG_ERROR("Did not receive negotiation response after retries");
        return std::nullopt;
    }

    EncryptionNegotiation result;

    // Determine encryption type from response code
    uint16_t resp = msg->header.response_code;
    uint8_t resp_high = (resp >> 8) & 0xFF;
    uint8_t resp_low = resp & 0xFF;

    if (resp_high == 0xdd) {
        switch (resp_low) {
            case 0x00:
                result.type = EncryptionType::Unencrypted;
                LOG_DEBUG("Camera requires no encryption");
                break;
            case 0x01:
                result.type = EncryptionType::BCEncrypt;
                LOG_DEBUG("Camera requires BCEncrypt");
                break;
            case 0x02:
                result.type = EncryptionType::Aes;
                LOG_DEBUG("Camera requires AES encryption");
                break;
            case 0x12:
                result.type = EncryptionType::FullAes;
                LOG_DEBUG("Camera requires Full AES encryption");
                break;
            default:
                LOG_WARN("Unknown encryption response: 0x{:04x}", resp);
                result.type = EncryptionType::Unencrypted;
        }
    } else {
        LOG_WARN("Unexpected response code format: 0x{:04x}", resp);
    }

    // The payload is ALWAYS encrypted with BCEncrypt during negotiation
    // (BCEncrypt uses a fixed key, so no key exchange needed)
    // Even when AES is negotiated, the nonce payload is BCEncrypt encrypted
    // because we need the nonce to derive the AES key
    std::vector<uint8_t> payload_data = msg->payload_data;

    // Always decrypt with BCEncrypt (except for truly unencrypted mode)
    if (result.type != EncryptionType::Unencrypted && !payload_data.empty()) {
        BcCrypto temp_crypto;
        temp_crypto.set_bc_encrypt();
        payload_data = temp_crypto.decrypt(0, payload_data);
        LOG_DEBUG("Decrypted {} bytes of encryption response with BCEncrypt", payload_data.size());
    }

    // Parse XML payload to get nonce
    if (!payload_data.empty()) {
        std::string xml(payload_data.begin(), payload_data.end());
        LOG_INFO("Encryption XML response: {}", xml);

        auto enc = BcXmlBuilder::parse_encryption(xml);
        if (enc) {
            result.nonce = enc->nonce;
            LOG_INFO("Parsed nonce: {}", result.nonce);
        } else {
            LOG_WARN("Failed to parse encryption XML, using empty nonce");
        }
    } else {
        LOG_WARN("No payload data in encryption response");
    }

    // Also check extension data
    if (!msg->extension_data.empty()) {
        std::string ext(msg->extension_data.begin(), msg->extension_data.end());
        LOG_DEBUG("Extension data: {}", ext);
    }

    return result;
}

bool Authenticator::send_modern_login(const std::string& username,
                                      const std::string& password,
                                      const std::string& nonce) {
    // Hash credentials with nonce (uppercase hex, truncated to 31 chars)
    std::string hashed_username = MD5::to_hex_upper_truncated(MD5::hash(username + nonce));
    std::string hashed_password = MD5::to_hex_upper_truncated(MD5::hash(password + nonce));

    LOG_INFO("Username + nonce: {}", username + nonce);
    LOG_INFO("Password + nonce: {}", password + nonce);
    LOG_INFO("Hashed username: {}", hashed_username);
    LOG_INFO("Hashed password: {}", hashed_password);

    // Create login XML
    std::string xml = BcXmlBuilder::create_login_request(hashed_username, hashed_password);
    LOG_INFO("Login XML: {}", xml);

    // Create modern login message - use same msg_num as legacy login
    BcMessage msg = BcMessage::create_with_payload(
        MSG_ID_LOGIN,
        login_msg_num_,
        xml,
        MSG_CLASS_MODERN_24
    );

    return conn_.send_message(msg);
}

std::optional<DeviceInfoXml> Authenticator::receive_login_response() {
    // Some cameras send unsolicited messages during login; skip them
    std::optional<BcMessage> msg;
    for (int attempts = 0; attempts < 5; attempts++) {
        msg = conn_.receive_message(10000);
        if (!msg) {
            LOG_ERROR("No response to modern login");
            return std::nullopt;
        }

        if (msg->header.msg_id == MSG_ID_LOGIN) {
            break;
        }
        LOG_DEBUG("Skipping unexpected message ID {} during login", msg->header.msg_id);
        msg.reset();
    }

    if (!msg) {
        LOG_ERROR("Did not receive login response after retries");
        return std::nullopt;
    }

    LOG_INFO("Login response: msg_id={}, response_code={}, class={}",
             msg->header.msg_id, msg->header.response_code, msg->header.msg_class);

    // Log payload for debugging
    if (!msg->payload_data.empty()) {
        // Try to decrypt if we're using BCEncrypt
        std::vector<uint8_t> decrypted = msg->payload_data;
        if (conn_.encryption().type() == EncryptionType::BCEncrypt) {
            decrypted = conn_.encryption().decrypt(0, decrypted);
        }
        std::string payload(decrypted.begin(), decrypted.end());
        LOG_INFO("Login response payload: {}", payload);
    }

    if (msg->header.response_code != RESPONSE_CODE_OK) {
        LOG_ERROR("Login rejected with code: {}", msg->header.response_code);
        return std::nullopt;
    }

    // Parse device info from response
    if (!msg->payload_data.empty()) {
        std::string xml(msg->payload_data.begin(), msg->payload_data.end());
        LOG_DEBUG("Login response XML: {}", xml);
        return BcXmlBuilder::parse_device_info(xml);
    }

    // Return empty device info if no XML payload
    return DeviceInfoXml{};
}

} // namespace baichuan
