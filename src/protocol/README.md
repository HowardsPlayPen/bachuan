# Protocol Layer

Low-level implementation of the Baichuan (BC) protocol used by Reolink cameras.

## Files

| File | Purpose |
|------|---------|
| `bc_header.cpp/h` | BC message header parsing and serialization |
| `bc_crypto.cpp/h` | Encryption: BCEncrypt (XOR) and AES-128-CFB128 |
| `bc_xml.cpp/h` | XML message creation and parsing |
| `bc_media.cpp/h` | BcMedia frame parsing (video/audio) |

## Responsibilities

### BcHeader
- Parse 20-byte and 24-byte BC message headers
- Serialize headers for outgoing messages
- Message ID, class, and response code handling
- `BcMessage` structure for complete messages with header + payload

### BcCrypto
- **BCEncrypt**: XOR-based encryption with 8-byte fixed key
- **AES-128-CFB128**: OpenSSL-based AES encryption
- Key derivation: `MD5(nonce + "-" + password)` → uppercase hex → first 16 bytes
- Separate encrypt/decrypt contexts with IV reset per message

### BcXml
- XML builders for login, preview requests
- XML parsers for encryption response, device info, extension
- Uses libxml2 for parsing, string streams for serialization
- RAII wrappers for libxml2 resources

### BcMedia
- Parse BcMedia frames from binary stream:
  - Info (stream metadata): resolution, FPS, timestamps
  - IFrame (keyframes): H264/H265 video
  - PFrame (delta frames): H264/H265 video
  - AAC audio
  - ADPCM audio
- Magic number detection for frame type identification
- Padding handling (8-byte alignment)

## Dependencies

### Internal
- `utils/md5` - MD5 hashing for key derivation

### External
- **OpenSSL** (`libssl`, `libcrypto`) - AES-128-CFB128 encryption
  - `EVP_EncryptInit_ex`, `EVP_DecryptInit_ex`
  - `EVP_aes_128_cfb128()`
- **libxml2** - XML parsing
  - `xmlReadMemory`, `xmlDocGetRootElement`
  - `xmlGetProp`, `xmlNodeGetContent`

## Protocol Constants

```cpp
// Message magic
MAGIC_HEADER = 0x0abcdef0

// Message IDs
MSG_ID_LOGIN = 1
MSG_ID_VIDEO = 3
MSG_ID_VIDEO_STOP = 4

// Message classes
MSG_CLASS_LEGACY = 0x6514      // 20-byte header
MSG_CLASS_MODERN_20 = 0x6614   // 20-byte header
MSG_CLASS_MODERN_24 = 0x6414   // 24-byte header

// Encryption responses
0xdd00 = Unencrypted
0xdd01 = BCEncrypt
0xdd02 = AES
0xdd12 = FullAES

// BcMedia magic values
0x31303031 = Info V1 ("1001")
0x32303031 = Info V2 ("2001")
0x63643030-39 = IFrame ("00dc"-"09dc")
0x63643130-39 = PFrame ("01dc"-"19dc")
0x62773530 = AAC ("05wb")
0x62773130 = ADPCM ("01wb")
```

## References

### External Resources
- [Hacking Reolink Cameras for Fun and Profit](https://www.thirtythreeforty.net/posts/2020/05/hacking-reolink-cameras-for-fun-and-profit/) - Original reverse engineering blog post by thirtythreeforty
- [Neolink GitHub](https://github.com/QuantumEntangledAndy/neolink) - Rust implementation of the Baichuan protocol (reference implementation)

### Protocol Documentation
The Baichuan protocol is not officially documented. The implementation in this folder is based on:
1. Reverse engineering of network traffic between Reolink apps and cameras
2. The Neolink Rust implementation (linked above)
3. Wireshark packet captures and analysis

## AES Key Derivation

```
Input: password, nonce (from camera)
1. key_phrase = nonce + "-" + password
2. digest = MD5(key_phrase)
3. hex_str = uppercase_hex(digest)  // 32 chars
4. key = first 16 ASCII bytes of hex_str
```

Example:
```
nonce = "ABC123"
password = "admin"
key_phrase = "ABC123-admin"
MD5 = "1a2b3c4d..."
key = ['1', 'A', '2', 'B', '3', 'C', '4', 'D', ...] (16 bytes)
```
