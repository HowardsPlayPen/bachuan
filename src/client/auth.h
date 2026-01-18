#pragma once

#include "client/connection.h"
#include "protocol/bc_xml.h"
#include <string>
#include <optional>

namespace baichuan {

// Requested encryption level for login
enum class MaxEncryption {
    None,       // Request no encryption
    BCEncrypt,  // Request BCEncrypt (simple XOR)
    Aes         // Request AES (camera may fall back to lower)
};

// Result of login attempt
struct LoginResult {
    bool success = false;
    std::string error_message;
    std::optional<DeviceInfoXml> device_info;
    EncryptionType encryption_type = EncryptionType::Unencrypted;
};

class Authenticator {
public:
    explicit Authenticator(Connection& conn);

    // Perform the 3-step login process
    // max_encryption specifies the maximum encryption level to request
    // Returns LoginResult with success status and device info
    LoginResult login(const std::string& username, const std::string& password,
                      MaxEncryption max_encryption = MaxEncryption::Aes);

private:
    Connection& conn_;
    uint16_t login_msg_num_ = 0;
    MaxEncryption max_encryption_ = MaxEncryption::Aes;

    // Step 1: Send legacy login request to negotiate encryption
    bool send_legacy_login();

    // Step 2: Receive encryption negotiation and extract nonce
    struct EncryptionNegotiation {
        EncryptionType type = EncryptionType::Unencrypted;
        std::string nonce;
    };
    std::optional<EncryptionNegotiation> receive_encryption_negotiation();

    // Step 3: Send modern login with hashed credentials
    bool send_modern_login(const std::string& username,
                          const std::string& password,
                          const std::string& nonce);

    // Receive and parse login response
    std::optional<DeviceInfoXml> receive_login_response();
};

} // namespace baichuan
