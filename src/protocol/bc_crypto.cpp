#include "protocol/bc_crypto.h"
#include "utils/md5.h"
#include "utils/logger.h"

#include <openssl/evp.h>
#include <openssl/err.h>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <iomanip>

namespace bachuan {

struct BcCrypto::AesContext {
    std::array<uint8_t, 16> key;
    EVP_CIPHER_CTX* enc_ctx = nullptr;
    EVP_CIPHER_CTX* dec_ctx = nullptr;

    AesContext() = default;

    ~AesContext() {
        if (enc_ctx) EVP_CIPHER_CTX_free(enc_ctx);
        if (dec_ctx) EVP_CIPHER_CTX_free(dec_ctx);
    }

    void init(const std::array<uint8_t, 16>& k) {
        key = k;

        // Create encryption context
        enc_ctx = EVP_CIPHER_CTX_new();
        if (!enc_ctx) {
            throw std::runtime_error("Failed to create AES encryption context");
        }

        // Create decryption context
        dec_ctx = EVP_CIPHER_CTX_new();
        if (!dec_ctx) {
            EVP_CIPHER_CTX_free(enc_ctx);
            enc_ctx = nullptr;
            throw std::runtime_error("Failed to create AES decryption context");
        }

        // Initialize cipher state with key and IV once
        // For streaming AES-CFB, we maintain state across messages
        reset_enc();
        reset_dec();
    }

    void reset_enc() {
        // Use CFB128 (full block feedback) - matches Rust cfb_mode crate and protocol docs
        if (EVP_EncryptInit_ex(enc_ctx, EVP_aes_128_cfb128(), nullptr,
                               key.data(), reinterpret_cast<const uint8_t*>(AES_IV)) != 1) {
            throw std::runtime_error("Failed to init AES encryption");
        }
        EVP_CIPHER_CTX_set_padding(enc_ctx, 0);
    }

    void reset_dec() {
        if (EVP_DecryptInit_ex(dec_ctx, EVP_aes_128_cfb128(), nullptr,
                               key.data(), reinterpret_cast<const uint8_t*>(AES_IV)) != 1) {
            throw std::runtime_error("Failed to init AES decryption");
        }
        EVP_CIPHER_CTX_set_padding(dec_ctx, 0);
    }

    // Debug: print the current key
    void debug_key() {
        std::string hex;
        for (int i = 0; i < 16; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02x ", key[i]);
            hex += buf;
        }
        // LOG via stderr for debugging
        fprintf(stderr, "AES key bytes: %s\n", hex.c_str());
    }
};

BcCrypto::BcCrypto() : type_(EncryptionType::Unencrypted) {}

BcCrypto::~BcCrypto() = default;

BcCrypto::BcCrypto(BcCrypto&& other) noexcept
    : type_(other.type_), aes_ctx_(std::move(other.aes_ctx_)) {
    other.type_ = EncryptionType::Unencrypted;
}

BcCrypto& BcCrypto::operator=(BcCrypto&& other) noexcept {
    if (this != &other) {
        type_ = other.type_;
        aes_ctx_ = std::move(other.aes_ctx_);
        other.type_ = EncryptionType::Unencrypted;
    }
    return *this;
}

void BcCrypto::set_unencrypted() {
    type_ = EncryptionType::Unencrypted;
    aes_ctx_.reset();
}

void BcCrypto::set_bc_encrypt() {
    type_ = EncryptionType::BCEncrypt;
    aes_ctx_.reset();
}

void BcCrypto::set_aes(const std::array<uint8_t, 16>& key) {
    type_ = EncryptionType::Aes;
    aes_ctx_ = std::make_unique<AesContext>();
    aes_ctx_->init(key);
    LOG_DEBUG("AES encryption initialized");
}

void BcCrypto::set_full_aes(const std::array<uint8_t, 16>& key) {
    type_ = EncryptionType::FullAes;
    aes_ctx_ = std::make_unique<AesContext>();
    aes_ctx_->init(key);
    LOG_DEBUG("Full AES encryption initialized");
}

std::array<uint8_t, 16> BcCrypto::derive_aes_key(const std::string& password,
                                                  const std::string& nonce) {
    // Key derivation: MD5("{nonce}-{password}") -> uppercase hex -> first 16 ASCII bytes
    std::string key_phrase = nonce + "-" + password;
    auto digest = MD5::hash(key_phrase);

    // Convert to uppercase hex string
    std::ostringstream oss;
    for (uint8_t byte : digest) {
        oss << std::uppercase << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(byte);
    }
    std::string hex_str = oss.str();

    LOG_DEBUG("AES key derivation: phrase='{}', hex='{}'", key_phrase, hex_str);

    // Take first 16 ASCII bytes of the hex string
    std::array<uint8_t, 16> key;
    for (size_t i = 0; i < 16 && i < hex_str.size(); ++i) {
        key[i] = static_cast<uint8_t>(hex_str[i]);
    }

    // Log the actual key bytes
    std::ostringstream key_oss;
    for (size_t i = 0; i < 16; ++i) {
        key_oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(key[i]);
        if (i < 15) key_oss << " ";
    }
    LOG_DEBUG("AES key bytes: {}", key_oss.str());

    return key;
}

std::vector<uint8_t> BcCrypto::encrypt(uint32_t offset, const uint8_t* data, size_t len) {
    switch (type_) {
        case EncryptionType::Unencrypted:
            return std::vector<uint8_t>(data, data + len);

        case EncryptionType::BCEncrypt:
            return bc_encrypt_decrypt(offset, data, len);

        case EncryptionType::Aes:
        case EncryptionType::FullAes:
            return aes_encrypt(data, len);
    }
    return std::vector<uint8_t>(data, data + len);
}

std::vector<uint8_t> BcCrypto::decrypt(uint32_t offset, const uint8_t* data, size_t len) {
    switch (type_) {
        case EncryptionType::Unencrypted:
            return std::vector<uint8_t>(data, data + len);

        case EncryptionType::BCEncrypt:
            return bc_encrypt_decrypt(offset, data, len);

        case EncryptionType::Aes:
        case EncryptionType::FullAes:
            return aes_decrypt(data, len);
    }
    return std::vector<uint8_t>(data, data + len);
}

std::vector<uint8_t> BcCrypto::bc_encrypt_decrypt(uint32_t offset, const uint8_t* data, size_t len) {
    // BCEncrypt: XOR each byte with key[(offset+i) % 8] ^ (offset as u8)
    // Note: offset is XORed as-is (not offset+i), only key index advances
    // This is symmetric - encrypt and decrypt are the same operation
    std::vector<uint8_t> result(len);

    uint8_t offset_byte = static_cast<uint8_t>(offset & 0xFF);
    for (size_t i = 0; i < len; ++i) {
        size_t key_idx = (offset + i) % 8;
        uint8_t key_byte = BC_ENCRYPT_KEY[key_idx];
        result[i] = data[i] ^ key_byte ^ offset_byte;
    }

    return result;
}

std::vector<uint8_t> BcCrypto::aes_encrypt(const uint8_t* data, size_t len) {
    if (!aes_ctx_) {
        throw std::runtime_error("AES context not initialized");
    }

    // Reset cipher state for each message - Rust code clones the encryptor
    // for each operation, which effectively resets to IV state
    aes_ctx_->reset_enc();

    std::vector<uint8_t> result(len + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;

    if (EVP_EncryptUpdate(aes_ctx_->enc_ctx, result.data(), &out_len,
                          data, static_cast<int>(len)) != 1) {
        throw std::runtime_error("AES encryption failed");
    }

    int final_len = 0;
    if (EVP_EncryptFinal_ex(aes_ctx_->enc_ctx, result.data() + out_len, &final_len) != 1) {
        throw std::runtime_error("AES encryption finalization failed");
    }

    result.resize(out_len + final_len);
    return result;
}

std::vector<uint8_t> BcCrypto::aes_decrypt(const uint8_t* data, size_t len) {
    if (!aes_ctx_) {
        throw std::runtime_error("AES context not initialized");
    }

    // Reset cipher state for each message - Rust code clones the decryptor
    // for each operation, which effectively resets to IV state
    aes_ctx_->reset_dec();

    std::vector<uint8_t> result(len + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;

    if (EVP_DecryptUpdate(aes_ctx_->dec_ctx, result.data(), &out_len,
                          data, static_cast<int>(len)) != 1) {
        throw std::runtime_error("AES decryption failed");
    }

    int final_len = 0;
    if (EVP_DecryptFinal_ex(aes_ctx_->dec_ctx, result.data() + out_len, &final_len) != 1) {
        throw std::runtime_error("AES decryption finalization failed");
    }

    result.resize(out_len + final_len);
    return result;
}

} // namespace bachuan
