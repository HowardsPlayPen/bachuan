#include "utils/md5.h"
#include <openssl/evp.h>
#include <openssl/md5.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace baichuan {

MD5::Digest MD5::hash(const std::string& data) {
    return hash(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

MD5::Digest MD5::hash(const uint8_t* data, size_t len) {
    Digest digest;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("Failed to create EVP_MD_CTX");
    }

    if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("Failed to initialize MD5 digest");
    }

    if (EVP_DigestUpdate(ctx, data, len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("Failed to update MD5 digest");
    }

    unsigned int digest_len = 0;
    if (EVP_DigestFinal_ex(ctx, digest.data(), &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("Failed to finalize MD5 digest");
    }

    EVP_MD_CTX_free(ctx);
    return digest;
}

MD5::Digest MD5::hash(const std::vector<uint8_t>& data) {
    return hash(data.data(), data.size());
}

std::string MD5::to_hex(const Digest& digest) {
    std::ostringstream oss;
    for (uint8_t byte : digest) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::string MD5::to_hex_upper_truncated(const Digest& digest) {
    // Baichuan protocol uses uppercase hex, truncated to 31 characters
    std::ostringstream oss;
    for (uint8_t byte : digest) {
        oss << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
    }
    std::string result = oss.str();
    // Truncate to 31 characters (removes last hex digit)
    if (result.size() > 31) {
        result.resize(31);
    }
    return result;
}

std::string MD5::hash_hex(const std::string& data) {
    return to_hex(hash(data));
}

std::string MD5::hash_hex(const uint8_t* data, size_t len) {
    return to_hex(hash(data, len));
}

} // namespace baichuan
