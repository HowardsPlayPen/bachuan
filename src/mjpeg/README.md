# MJPEG Source

This module provides HTTP MJPEG (Motion JPEG) stream support for viewing camera streams.

## Implementation Approach

This implementation uses raw sockets for HTTP and libjpeg for JPEG decoding, providing a lightweight solution with no FFmpeg dependency for the MJPEG path.

### Why Native Implementation?

Unlike RTSP which required FFmpeg's libavformat due to protocol complexity, MJPEG over HTTP is simple enough to implement directly:

**MJPEG is simple because:**
- Standard HTTP GET request to start
- Multipart MIME boundaries separate frames
- Each frame is a complete, standalone JPEG image
- No session negotiation, RTP, or RTCP
- No temporal compression or frame dependencies

**Benefits of native implementation:**
- **Minimal dependencies** - Only libjpeg, no FFmpeg needed for MJPEG
- **Lower latency** - Direct socket reads with no intermediate buffering
- **Full control** - Easy to tune timeouts, handle reconnection, etc.
- **Simpler debugging** - Straightforward HTTP + JPEG flow

**Trade-offs:**
- More code to maintain vs using libavformat
- Need to handle HTTP edge cases ourselves
- Basic authentication only (no Digest auth currently)

## How MJPEG Streaming Works

The server sends a continuous HTTP response with multipart MIME content:

```
HTTP/1.1 200 OK
Content-Type: multipart/x-mixed-replace; boundary=--myboundary

--myboundary
Content-Type: image/jpeg
Content-Length: 12345

<JPEG binary data>
--myboundary
Content-Type: image/jpeg
Content-Length: 12346

<JPEG binary data>
...
```

The client:
1. Connects via TCP and sends HTTP GET request
2. Parses response headers to extract boundary string
3. Loops: find boundary → read headers → read JPEG → decode → display

## Usage

### Single Camera (baichuan)

```bash
# Basic MJPEG connection
./baichuan --mjpeg http://admin:password@192.168.1.100/mjpeg

# Without authentication
./baichuan --mjpeg http://camera:8080/video.mjpg

# Capture snapshot from MJPEG
./baichuan --mjpeg http://admin:pass@camera/mjpeg --img snapshot.jpg

# Record video from MJPEG
./baichuan --mjpeg http://admin:pass@camera/mjpeg --video recording.mp4 -t 30
```

### Dashboard (Multi-Camera)

Add MJPEG cameras to your JSON configuration:

```json
{
  "columns": 2,
  "cameras": [
    {
      "name": "Garage",
      "type": "mjpeg",
      "url": "http://admin:password@192.168.1.100/mjpeg"
    },
    {
      "name": "Front Door",
      "type": "baichuan",
      "host": "192.168.1.101",
      "username": "admin",
      "password": "password123"
    }
  ]
}
```

## Common MJPEG URL Formats

Different devices use different URL paths:

### Generic
```
http://user:pass@<ip>/mjpeg
http://user:pass@<ip>/video.mjpg
http://user:pass@<ip>/cgi-bin/mjpeg
http://<ip>:8080/video
```

### ESP32-CAM
```
http://<ip>:81/stream
```

### Raspberry Pi Camera
```
http://<ip>:8080/?action=stream
```

### Many IP Cameras
```
http://user:pass@<ip>/axis-cgi/mjpg/video.cgi
http://user:pass@<ip>/mjpg/video.mjpg
```

## Architecture

```
MjpegSource
    |
    +-- HTTP Layer (raw sockets)
    |       |
    |       +-- TCP connect
    |       +-- Send GET request with Basic Auth
    |       +-- Parse response headers
    |       +-- Extract multipart boundary
    |
    +-- Frame Parser
    |       |
    |       +-- Find boundary marker
    |       +-- Read part headers (Content-Length)
    |       +-- Read JPEG data
    |
    +-- JPEG Decoder (libjpeg)
    |       |
    |       +-- jpeg_mem_src() - read from memory
    |       +-- jpeg_read_header()
    |       +-- jpeg_start_decompress()
    |       +-- jpeg_read_scanlines() -> RGB
    |
    +-- DecodedFrame callback
            |
            +-- Direct to display (no VideoDecoder needed)
```

## Key Differences from RTSP/Baichuan

| Aspect | MJPEG | RTSP | Baichuan |
|--------|-------|------|----------|
| Protocol | HTTP | RTSP+RTP | Proprietary |
| Codec | JPEG | H.264/H.265 | H.264/H.265 |
| Frame type | All keyframes | I/P/B frames | I/P frames |
| Compression | Spatial only | Temporal+Spatial | Temporal+Spatial |
| Bandwidth | Higher | Lower | Lower |
| Latency | Low | Low | Low |
| Complexity | Simple | Complex | Medium |
| Decoder | libjpeg | FFmpeg | FFmpeg |

## Files

- `mjpeg_source.h` - MjpegSource class declaration
- `mjpeg_source.cpp` - HTTP client, multipart parser, JPEG decoder

## Dependencies

- libjpeg (or libjpeg-turbo) - JPEG decoding
- POSIX sockets - HTTP networking
