# RTSP Source

This module provides RTSP (Real Time Streaming Protocol) support for viewing camera streams from any RTSP-compatible source.

## Implementation Approach

This implementation uses FFmpeg's libavformat library rather than implementing the RTSP protocol directly. Here's why:

### Why FFmpeg?

**Speed of implementation** - FFmpeg provides a complete, battle-tested RTSP client implementation. Using libavformat allowed adding RTSP support in a fraction of the time it would take to implement the protocol from scratch.

**What FFmpeg handles for us:**
- RTSP session negotiation (OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN)
- SDP (Session Description Protocol) parsing
- RTP packet reassembly and reordering
- RTCP (Real-time Transport Control Protocol) handling
- TCP interleaved and UDP transport modes
- Authentication (Basic and Digest)
- Codec parameter extraction (SPS/PPS)

### Trade-offs

**Not optimal because:**
- **Larger dependency** - Pulls in all of libavformat even though we only need RTSP
- **Less control** - Limited ability to tune RTSP-specific parameters or handle edge cases
- **Buffering** - FFmpeg's internal buffering adds some latency compared to a minimal custom implementation
- **Error handling** - Generic error codes rather than RTSP-specific diagnostics

**A native implementation would offer:**
- Smaller binary size (no libavformat dependency for RTSP)
- Fine-grained control over session management and timing
- Ability to implement camera-specific RTSP quirks
- Potentially lower latency with minimal buffering
- Better integration with the existing Baichuan connection model

**However, the FFmpeg approach is pragmatic:**
- RTSP is complex (RFC 2326 + RTP/RTCP + SDP + various extensions)
- A robust implementation would take significant development time
- FFmpeg's implementation handles countless edge cases discovered over years
- The latency difference is minimal for most use cases
- libavformat is already a dependency for video decoding

For this project, the FFmpeg approach provides good-enough RTSP support with minimal development effort, allowing focus on the core Baichuan protocol features.

## Overview

The `RtspSource` class implements the `IVideoSource` interface using FFmpeg's libavformat library to connect to and receive video from RTSP streams. This allows the application to display video from:

- Generic IP cameras with RTSP support
- Reolink cameras via their RTSP interface (alternative to Baichuan protocol)
- Network video recorders (NVRs)
- Any device providing an RTSP stream

## Features

- **H.264 and H.265 codec support** - Automatically detects codec from stream
- **TCP and UDP transport** - Configurable transport protocol (TCP recommended for reliability)
- **Keyframe synchronization** - Waits for first keyframe before decoding to avoid artifacts
- **Extradata handling** - Properly handles SPS/PPS (H.264) and VPS/SPS/PPS (H.265) parameter sets
- **Low latency** - Configured for minimal buffering

## Usage

### Single Camera (baichuan)

```bash
# Basic RTSP connection
./baichuan --rtsp rtsp://admin:password@192.168.1.100:554/h264Preview_01_main

# With UDP transport
./baichuan --rtsp rtsp://192.168.1.100/stream --transport udp

# Capture snapshot from RTSP
./baichuan --rtsp rtsp://admin:pass@camera/stream --img snapshot.jpg

# Record video from RTSP
./baichuan --rtsp rtsp://admin:pass@camera/stream --video recording.mp4 -t 30
```

### Dashboard (Multi-Camera)

Add RTSP cameras to your JSON configuration:

```json
{
  "columns": 2,
  "cameras": [
    {
      "name": "Front Door",
      "type": "rtsp",
      "url": "rtsp://admin:password@192.168.1.100:554/h264Preview_01_main",
      "transport": "tcp"
    },
    {
      "name": "Back Yard",
      "type": "baichuan",
      "host": "192.168.1.101",
      "username": "admin",
      "password": "password123"
    }
  ]
}
```

## Common RTSP URL Formats

Different camera manufacturers use different RTSP URL formats:

### Reolink
```
rtsp://admin:password@<ip>:554/h264Preview_01_main    # Main stream
rtsp://admin:password@<ip>:554/h264Preview_01_sub     # Sub stream
```

### Hikvision
```
rtsp://admin:password@<ip>:554/Streaming/Channels/101  # Main stream
rtsp://admin:password@<ip>:554/Streaming/Channels/102  # Sub stream
```

### Dahua
```
rtsp://admin:password@<ip>:554/cam/realmonitor?channel=1&subtype=0  # Main
rtsp://admin:password@<ip>:554/cam/realmonitor?channel=1&subtype=1  # Sub
```

### Generic
```
rtsp://user:pass@<ip>:<port>/stream
rtsp://user:pass@<ip>:<port>/live
```

## Implementation Details

### Architecture

```
RtspSource
    |
    +-- FFmpeg libavformat (RTSP demuxing)
    |       |
    |       +-- avformat_open_input()    - Connect to RTSP URL
    |       +-- avformat_find_stream_info() - Detect streams
    |       +-- av_read_frame()          - Receive packets
    |
    +-- IVideoSource interface
            |
            +-- FrameCallback(data, len, codec) - Deliver to decoder
```

### Keyframe Handling

The module waits for the first keyframe (I-frame) before delivering frames to the decoder. This prevents decoder errors that occur when P-frames are received without prior reference frames.

For keyframes, the codec extradata (SPS/PPS parameter sets) is prepended to ensure the decoder has all necessary information.

## Files

- `rtsp_source.h` - RtspSource class declaration
- `rtsp_source.cpp` - Implementation using FFmpeg libavformat

## Dependencies

- FFmpeg libavformat - RTSP protocol handling
- FFmpeg libavcodec - Codec detection
- FFmpeg libavutil - Utility functions
