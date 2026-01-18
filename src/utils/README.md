# Utils Layer

Utility functions and helpers used throughout the application.

## Files

| File | Purpose |
|------|---------|
| `logger.cpp/h` | Thread-safe logging with levels and timestamps |
| `md5.cpp/h` | MD5 hash implementation |

## Responsibilities

### Logger
- Log levels: Debug, Info, Warning, Error
- Thread-safe output with mutex
- Timestamp formatting (HH:MM:SS.mmm)
- Simple format string support with `{}` placeholders
- Hex dump utility for binary data

Usage:
```cpp
LOG_DEBUG("Message with value: {}", value);
LOG_INFO("Connection established");
LOG_WARN("Unexpected response: {}", code);
LOG_ERROR("Failed to connect: {}", error);
```

### MD5
- Pure C++ MD5 implementation (no external dependencies)
- Returns 16-byte digest as `std::array<uint8_t, 16>`
- Hex conversion utilities:
  - `to_hex()` - lowercase hex string
  - `to_hex_upper()` - uppercase hex string
  - `to_hex_upper_truncated()` - uppercase, truncated to 31 chars (for credentials)

Usage:
```cpp
auto digest = MD5::hash("input string");
std::string hex = MD5::to_hex(digest);
std::string upper = MD5::to_hex_upper_truncated(digest);
```

## Dependencies

### Internal
- None

### External
- **None** - Pure C++ standard library only

## Design Notes

- Logger uses `{}` placeholder syntax (similar to fmt/spdlog) but with simple implementation
- Logger does NOT support format specifiers like `{:08x}` - use snprintf for formatted values
- MD5 implementation is self-contained to avoid OpenSSL dependency in utils layer
- All utilities are designed to be header-includable with minimal compilation overhead
