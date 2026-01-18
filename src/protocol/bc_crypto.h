#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <string>

namespace baichuan {

// Fixed XOR key for BCEncrypt
constexpr uint8_t BC_ENCRYPT_KEY[8] = {0x1F, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0xFF};

// Fixed IV for AES-128-CFB
constexpr char AES_IV[17] = "0123456789abcdef";

enum class EncryptionType {
    Unencrypted,
    BCEncrypt,
    Aes,
    FullAes
};

class BcCrypto {
public:
    BcCrypto();
    ~BcCrypto();

    // Move operations
    BcCrypto(BcCrypto&& other) noexcept;
    BcCrypto& operator=(BcCrypto&& other) noexcept;

    // Disable copy
    BcCrypto(const BcCrypto&) = delete;
    BcCrypto& operator=(const BcCrypto&) = delete;

    // Set encryption type (Unencrypted or BCEncrypt)
    void set_unencrypted();
    void set_bc_encrypt();

    // Set AES encryption with key (16 bytes)
    void set_aes(const std::array<uint8_t, 16>& key);
    void set_full_aes(const std::array<uint8_t, 16>& key);

    // Derive AES key from password and nonce
    static std::array<uint8_t, 16> derive_aes_key(const std::string& password,
                                                   const std::string& nonce);

    // Encrypt/decrypt data
    // offset is used for BCEncrypt (comes from packet header offset)
    std::vector<uint8_t> encrypt(uint32_t offset, const uint8_t* data, size_t len);
    std::vector<uint8_t> decrypt(uint32_t offset, const uint8_t* data, size_t len);

    std::vector<uint8_t> encrypt(uint32_t offset, const std::vector<uint8_t>& data) {
        return encrypt(offset, data.data(), data.size());
    }

    std::vector<uint8_t> decrypt(uint32_t offset, const std::vector<uint8_t>& data) {
        return decrypt(offset, data.data(), data.size());
    }

    EncryptionType type() const { return type_; }

    // Check if video stream should be encrypted (only FullAes)
    bool encrypts_video() const { return type_ == EncryptionType::FullAes; }

private:
    EncryptionType type_;

    // AES context (opaque pointer to hide OpenSSL details)
    struct AesContext;
    std::unique_ptr<AesContext> aes_ctx_;

    std::vector<uint8_t> bc_encrypt_decrypt(uint32_t offset, const uint8_t* data, size_t len);
    std::vector<uint8_t> aes_encrypt(const uint8_t* data, size_t len);
    std::vector<uint8_t> aes_decrypt(const uint8_t* data, size_t len);
};

} // namespace baichuan
