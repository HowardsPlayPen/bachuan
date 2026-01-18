#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <array>

namespace baichuan {

class MD5 {
public:
    static constexpr size_t DIGEST_SIZE = 16;
    using Digest = std::array<uint8_t, DIGEST_SIZE>;

    // Compute MD5 hash of string data
    static Digest hash(const std::string& data);

    // Compute MD5 hash of binary data
    static Digest hash(const uint8_t* data, size_t len);
    static Digest hash(const std::vector<uint8_t>& data);

    // Convert digest to hex string (lowercase)
    static std::string to_hex(const Digest& digest);

    // Convert digest to uppercase hex string, truncated to 31 chars (Baichuan protocol)
    static std::string to_hex_upper_truncated(const Digest& digest);

    // Compute MD5 and return hex string directly
    static std::string hash_hex(const std::string& data);
    static std::string hash_hex(const uint8_t* data, size_t len);
};

} // namespace baichuan
