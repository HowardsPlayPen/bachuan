# RTSP Support Implementation Plan

## Overview

Add RTSP (Real Time Streaming Protocol) support to enable connecting to any RTSP-compatible camera, not just Reolink cameras using the proprietary Baichuan protocol.

## Goals

1. Support standard RTSP URLs: `rtsp://username:password@host:port/path`
2. Handle RTSP authentication (Basic and Digest)
3. Receive H.264/H.265 video streams via RTP
4. Feed decoded frames into the existing display pipeline
5. Support both single-camera (baichuan) and multi-camera (dashboard) applications

## Architecture

### Current Flow (Baichuan)
```
Connection → Authenticator → VideoStream → BcMedia Parser → VideoDecoder → Display
     ↓              ↓              ↓              ↓              ↓
   TCP:9000    BC Protocol    BC Messages    Frame Extract    FFmpeg
```

### Proposed RTSP Flow
```
RtspClient → RtspSession → RtpReceiver → VideoDecoder → Display
     ↓            ↓            ↓              ↓
  TCP:554     RTSP/SDP      RTP/H264       FFmpeg
```

### Unified Interface
```
┌─────────────────────────────────────────────────────────┐
│                    IVideoSource                          │
│  - connect(url/config)                                   │
│  - start()                                               │
│  - stop()                                                │
│  - on_frame(callback)                                    │
│  - on_error(callback)                                    │
└─────────────────────────────────────────────────────────┘
           ▲                              ▲
           │                              │
┌──────────┴──────────┐      ┌───────────┴───────────┐
│  BaichuanSource     │      │     RtspSource        │
│  (existing code)    │      │     (new code)        │
└─────────────────────┘      └───────────────────────┘
```

## Implementation Phases

### Phase 1: RTSP Protocol Layer

**Files to create:**
```
src/rtsp/
├── rtsp_client.h/.cpp      # RTSP protocol handling
├── rtsp_session.h/.cpp     # Session management (DESCRIBE, SETUP, PLAY)
├── rtp_receiver.h/.cpp     # RTP packet reception and depacketization
├── sdp_parser.h/.cpp       # SDP (Session Description Protocol) parsing
└── README.md               # Documentation
```

**Key components:**

1. **RtspClient** - TCP connection and RTSP message exchange
   - Parse RTSP URLs
   - Send RTSP requests (OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN)
   - Handle RTSP responses
   - Support Basic and Digest authentication
   - CSeq sequence number tracking

2. **SdpParser** - Parse SDP from DESCRIBE response
   - Extract media streams (video, audio)
   - Get codec parameters (H.264/H.265 profile, SPS/PPS)
   - Get RTP payload types and ports

3. **RtpReceiver** - Receive and depacketize RTP
   - UDP socket for RTP data
   - Handle RTP header parsing
   - H.264 NAL unit depacketization (RFC 6184)
   - H.265 NAL unit depacketization (RFC 7798)
   - Sequence number tracking and reordering
   - RTCP handling (optional, for keep-alive)

### Phase 2: H.264/H.265 Depacketization

**RTP to NAL unit conversion:**

```
RTP Packet → Depacketizer → NAL Units → VideoDecoder
                  │
                  ├─ Single NAL Unit Mode
                  ├─ Aggregation Packets (STAP-A)
                  └─ Fragmentation Units (FU-A)
```

**H.264 RTP depacketization (RFC 6184):**
- NAL unit type 1-23: Single NAL unit packet
- NAL unit type 24 (STAP-A): Aggregated NAL units
- NAL unit type 28 (FU-A): Fragmented NAL unit

**H.265 RTP depacketization (RFC 7798):**
- Similar structure but different NAL unit types
- AP (Aggregation Packets)
- FU (Fragmentation Units)

### Phase 3: Video Source Abstraction

**Create unified interface:**

```cpp
// src/source/video_source.h
class IVideoSource {
public:
    virtual ~IVideoSource() = default;

    virtual bool connect() = 0;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool is_connected() const = 0;

    using FrameCallback = std::function<void(const uint8_t* data, size_t len, VideoCodec codec)>;
    using ErrorCallback = std::function<void(const std::string& error)>;

    virtual void on_frame(FrameCallback cb) = 0;
    virtual void on_error(ErrorCallback cb) = 0;
};
```

**Implementations:**
- `BaichuanSource` - Wraps existing Connection + VideoStream
- `RtspSource` - Wraps new RtspClient + RtpReceiver

### Phase 4: Application Integration

**Command line changes:**

```bash
# Baichuan protocol (existing)
./baichuan -h 192.168.1.100 -u admin -P password

# RTSP protocol (new)
./baichuan --rtsp rtsp://admin:password@192.168.1.100:554/stream1

# Dashboard with mixed sources
./dashboard -c config.json
```

**JSON config extension:**
```json
{
  "cameras": [
    {
      "name": "Front Door",
      "type": "baichuan",
      "host": "192.168.1.100",
      "username": "admin",
      "password": "password"
    },
    {
      "name": "Back Yard",
      "type": "rtsp",
      "url": "rtsp://admin:password@192.168.1.101:554/h264"
    }
  ]
}
```

### Phase 5: Testing and Refinement

1. Test with various RTSP servers:
   - VLC as RTSP server
   - FFmpeg as RTSP server
   - Real IP cameras (Hikvision, Dahua, generic)

2. Handle edge cases:
   - Connection timeouts
   - Stream interruptions
   - Codec negotiation failures
   - NAL unit assembly errors

## Dependencies

**Existing (already in project):**
- FFmpeg (libavcodec) - Video decoding
- OpenSSL - For Digest authentication MD5

**No new dependencies required** - RTSP can be implemented with:
- Standard TCP sockets (already used)
- UDP sockets (new, but standard POSIX)
- Existing MD5 implementation (for Digest auth)

## File Changes Summary

**New files:**
```
src/rtsp/
├── rtsp_client.h
├── rtsp_client.cpp
├── rtsp_session.h
├── rtsp_session.cpp
├── rtp_receiver.h
├── rtp_receiver.cpp
├── rtp_depacketizer.h
├── rtp_depacketizer.cpp
├── sdp_parser.h
├── sdp_parser.cpp
└── README.md

src/source/
├── video_source.h
├── baichuan_source.h
├── baichuan_source.cpp
├── rtsp_source.h
└── rtsp_source.cpp
```

**Modified files:**
- `CMakeLists.txt` - Add new source files
- `src/main.cpp` - Add --rtsp option
- `src/dashboard_main.cpp` - Support RTSP in config
- `src/utils/json_config.h` - Add RTSP config parsing

## Estimated Complexity

| Phase | Description | Complexity |
|-------|-------------|------------|
| 1 | RTSP Protocol Layer | Medium |
| 2 | RTP Depacketization | Medium-High |
| 3 | Video Source Abstraction | Low |
| 4 | Application Integration | Low |
| 5 | Testing | Medium |

## Alternative: FFmpeg RTSP

Instead of implementing RTSP from scratch, we could use FFmpeg's libavformat which has built-in RTSP support:

```cpp
AVFormatContext* fmt_ctx = avformat_alloc_context();
avformat_open_input(&fmt_ctx, "rtsp://...", NULL, NULL);
avformat_find_stream_info(fmt_ctx, NULL);
// Read packets with av_read_frame()
```

**Pros:**
- Much simpler implementation
- Handles all RTSP/RTP complexity
- Supports many codecs automatically
- Battle-tested code

**Cons:**
- Less control over connection handling
- Harder to integrate with existing callback model
- May have different threading requirements

**Recommendation:** Start with FFmpeg's RTSP support for faster implementation, then optionally implement native RTSP later if more control is needed.

## References

- [RFC 2326](https://tools.ietf.org/html/rfc2326) - RTSP 1.0
- [RFC 6184](https://tools.ietf.org/html/rfc6184) - RTP Payload Format for H.264
- [RFC 7798](https://tools.ietf.org/html/rfc7798) - RTP Payload Format for H.265
- [RFC 4566](https://tools.ietf.org/html/rfc4566) - SDP: Session Description Protocol
- [FFmpeg libavformat](https://ffmpeg.org/doxygen/trunk/group__lavf__decoding.html) - Demuxing API
