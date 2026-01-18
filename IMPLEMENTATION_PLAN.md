# Baichuan Protocol C++ Implementation Plan

## Overview

A C++ application to connect to Reolink cameras using the proprietary Baichuan protocol, receive video streams, and display them in a GTK window.

**Target Camera:** 10.0.1.29 (admin user)
**Protocol Port:** 9000 (TCP)

---

## Phase 1: Project Structure and Build System

### 1.1 Directory Structure
```
bachuan/
├── CMakeLists.txt
├── src/
│   ├── main.cpp                 # Application entry point
│   ├── protocol/
│   │   ├── bc_header.h/cpp      # Message header structures
│   │   ├── bc_message.h/cpp     # Full message handling
│   │   ├── bc_crypto.h/cpp      # Encryption (BCEncrypt, AES)
│   │   ├── bc_xml.h/cpp         # XML payload generation/parsing
│   │   └── bc_media.h/cpp       # Video/audio frame parsing
│   ├── client/
│   │   ├── connection.h/cpp     # TCP connection management
│   │   ├── auth.h/cpp           # 3-step login flow
│   │   └── stream.h/cpp         # Video stream handling
│   ├── video/
│   │   ├── decoder.h/cpp        # FFmpeg H264/H265 decoding
│   │   └── display.h/cpp        # GTK window display
│   └── utils/
│       ├── md5.h/cpp            # MD5 hashing
│       └── logger.h/cpp         # Logging utility
└── tests/
    └── test_protocol.cpp        # Unit tests
```

### 1.2 CMake Configuration
- Minimum CMake 3.16
- C++17 standard
- Dependencies: OpenSSL, FFmpeg (libavcodec, libavformat, libswscale), GTK3, libxml2

---

## Phase 2: Core Protocol Implementation

### 2.1 Message Header (bc_header.h/cpp)

**Structure (20-24 bytes, little-endian):**
```cpp
struct BcHeader {
    uint32_t magic;          // 0x0abcdef0 or 0x0fedcba0
    uint32_t msg_id;         // Message type (1=login, 3=video, etc.)
    uint32_t body_len;       // Payload length
    uint8_t  channel_id;     // Camera channel
    uint8_t  stream_type;    // 0=main, 1=sub, 2=extern
    uint16_t msg_num;        // Sequence number
    uint16_t response_code;  // 0=request, 200=success
    uint16_t msg_class;      // 0x6514=legacy, 0x6414/0x6614=modern
    uint32_t payload_offset; // Only for class 0x6414 or 0x0000
};
```

**Key Message IDs:**
- MSG_ID_LOGIN = 1
- MSG_ID_VIDEO = 3
- MSG_ID_VIDEO_STOP = 4
- MSG_ID_PING = 93
- MSG_ID_VERSION = 80

### 2.2 Encryption (bc_crypto.h/cpp)

**BCEncrypt (XOR-based):**
```cpp
// Fixed key: {0x1F, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0xFF}
void bc_encrypt(uint8_t* data, size_t len, uint32_t offset) {
    static const uint8_t key[8] = {0x1F, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0xFF};
    for (size_t i = 0; i < len; i++) {
        data[i] ^= key[(offset + i) % 8] ^ ((offset + i) & 0xFF);
    }
}
```

**AES-128-CFB:**
- Key: MD5(password + nonce) = 16 bytes
- IV: "0123456789abcdef" (fixed)
- Use OpenSSL EVP interface

### 2.3 XML Payloads (bc_xml.h/cpp)

Using libxml2 for XML generation/parsing:

**Login Request:**
```xml
<body>
  <LoginUser version="1.1">
    <userName>MD5_HASH_HEX</userName>
    <password>MD5_HASH_HEX</password>
  </LoginUser>
  <LoginNet version="1.1">
    <type>LAN</type>
    <udpPort>0</udpPort>
  </LoginNet>
</body>
```

**Video Stream Request:**
```xml
<body>
  <Preview version="1.1">
    <channelId>0</channelId>
    <handle>0</handle>
    <streamType>mainStream</streamType>
  </Preview>
</body>
```

### 2.4 BcMedia Parsing (bc_media.h/cpp)

**Magic Headers:**
- InfoV1: 0x31303031
- InfoV2: 0x32303031
- IFrame: 0x63643030-0x63643039
- PFrame: 0x63643130-0x63643139
- AAC Audio: 0x62773530
- ADPCM Audio: 0x62773130

**Frame Structure:**
```cpp
struct BcMediaFrame {
    uint32_t magic;
    uint8_t  video_type;    // 0x04=H264, 0x05=H265
    uint32_t payload_size;
    uint32_t microseconds;
    uint32_t posix_time;    // Only IFrame
    std::vector<uint8_t> data;
};
```

---

## Phase 3: Client Implementation

### 3.1 TCP Connection (connection.h/cpp)

- Async socket using select() or poll()
- Message framing: read header first, then body
- Timeout handling for keepalive
- Reconnection logic

### 3.2 Authentication Flow (auth.h/cpp)

**Three-Step Login:**

1. **Send Legacy Login Request**
   - msg_id = 1, class = 0x6514
   - response_code = 0xdc01 (prefer BCEncrypt)
   - Body: Empty or minimal XML

2. **Receive Encryption Negotiation**
   - Parse response_code (0xdd00/01/02/12)
   - Extract nonce from `<Encryption>` XML
   - Determine encryption type

3. **Send Modern Login**
   - msg_id = 1, class = 0x6414
   - Credentials: MD5(username + nonce), MD5(password + nonce)
   - Await 200 OK response

### 3.3 Video Stream (stream.h/cpp)

**Start Stream:**
1. Send MSG_ID_VIDEO with Preview XML
2. Receive 200 OK
3. Enter receive loop for video frames

**Receive Loop:**
1. Read Bc header
2. If msg_id == 3, parse BcMedia frame
3. Extract H264/H265 NAL units
4. Queue for decoder

**Stop Stream:**
1. Send MSG_ID_VIDEO_STOP
2. Exit receive loop

---

## Phase 4: Video Decoding

### 4.1 FFmpeg Decoder (decoder.h/cpp)

```cpp
class VideoDecoder {
public:
    bool init(AVCodecID codec_id);  // AV_CODEC_ID_H264 or AV_CODEC_ID_HEVC
    AVFrame* decode(const uint8_t* data, size_t size);
    void shutdown();
private:
    AVCodecContext* ctx_;
    AVPacket* pkt_;
    AVFrame* frame_;
};
```

### 4.2 Frame Conversion

- Use swscale for YUV420P to RGB24 conversion
- Output suitable for GTK display

---

## Phase 5: GTK Display

### 5.1 Window Setup (display.h/cpp)

```cpp
class VideoDisplay {
public:
    bool init(int width, int height);
    void update_frame(AVFrame* frame);
    void run_main_loop();
private:
    GtkWidget* window_;
    GtkWidget* drawing_area_;
    cairo_surface_t* surface_;
};
```

### 5.2 Rendering Pipeline

1. Convert AVFrame to cairo_surface_t
2. Draw surface in GTK drawing area
3. Use g_idle_add() for thread-safe UI updates

---

## Phase 6: Integration and Main Application

### 6.1 Application Flow (main.cpp)

```cpp
int main() {
    // 1. Parse command line (IP, username, password)
    // 2. Initialize GTK
    // 3. Create connection to camera
    // 4. Perform authentication
    // 5. Start video stream
    // 6. Decode and display in loop
    // 7. Handle user close → stop stream and cleanup
}
```

### 6.2 Threading Model

- **Main Thread:** GTK event loop, rendering
- **Network Thread:** TCP receive, message parsing
- **Decoder Thread:** FFmpeg decode queue

Use mutexes/condition variables for frame handoff.

---

## Implementation Order

| Step | Component | Files | Dependencies |
|------|-----------|-------|--------------|
| 1 | CMake setup | CMakeLists.txt | - |
| 2 | Logger utility | utils/logger.h/cpp | - |
| 3 | MD5 hashing | utils/md5.h/cpp | OpenSSL |
| 4 | Message header | protocol/bc_header.h/cpp | - |
| 5 | Encryption | protocol/bc_crypto.h/cpp | OpenSSL |
| 6 | XML handling | protocol/bc_xml.h/cpp | libxml2 |
| 7 | TCP connection | client/connection.h/cpp | Step 4 |
| 8 | Authentication | client/auth.h/cpp | Steps 4-7 |
| 9 | BcMedia parsing | protocol/bc_media.h/cpp | Step 4 |
| 10 | Video stream | client/stream.h/cpp | Steps 8-9 |
| 11 | FFmpeg decoder | video/decoder.h/cpp | FFmpeg |
| 12 | GTK display | video/display.h/cpp | GTK3 |
| 13 | Main integration | main.cpp | All above |
| 14 | Testing | tests/* | All above |

---

## Dependencies (Package Names - Arch Linux)

```bash
pacman -S cmake openssl libxml2 ffmpeg gtk3
```

**Libraries for linking:**
- `-lssl -lcrypto` (OpenSSL)
- `-lxml2` (libxml2)
- `-lavcodec -lavformat -lavutil -lswscale` (FFmpeg)
- `pkg-config --cflags --libs gtk+-3.0` (GTK3)

---

## Testing Strategy

1. **Unit Tests:** Message serialization/deserialization, encryption
2. **Integration Test:** Connect to 10.0.1.29, authenticate, receive 10 frames
3. **Manual Test:** Full application with video display

---

## Risk Areas

1. **Encryption negotiation:** Camera may require specific encryption type
2. **Frame parsing:** BcMedia format variations between firmware versions
3. **Thread safety:** Frame handoff between network and display threads
4. **NAL unit extraction:** H264/H265 framing within BcMedia packets

---

## Estimated Complexity

- Core protocol: ~1500 lines
- Client logic: ~800 lines
- Video/display: ~600 lines
- Utilities: ~300 lines
- **Total: ~3200 lines of C++**
