# Video Layer

Video decoding and display using FFmpeg and GTK3.

## Files

| File | Purpose |
|------|---------|
| `decoder.cpp/h` | FFmpeg-based H264/H265 video decoding |
| `display.cpp/h` | GTK3 window with Cairo rendering |

## Responsibilities

### VideoDecoder
- Initialize FFmpeg decoder for H264 or H265 (auto-detected from first IFrame)
- Decode video frames (IFrame and PFrame)
- Provide decoded frames via callback
- Statistics tracking (frames decoded, errors)
- Resource cleanup (codec contexts, frames, packets)

Features:
- Lazy initialization on first frame
- Codec auto-detection from BcMedia frame type
- YUV output (conversion to RGB done in display layer)
- Error recovery and logging

### VideoDisplay
- GTK3 window creation and management
- Cairo-based frame rendering
- YUV to RGB conversion using libswscale
- Window resize handling
- Close callback for graceful shutdown
- GTK main loop integration

Features:
- Aspect ratio preservation
- Efficient surface reuse
- Thread-safe frame updates
- Window title with camera info

## Dependencies

### Internal
- `protocol/bc_media` - BcMediaIFrame, BcMediaPFrame types

### External

#### FFmpeg Libraries
- **libavcodec** - Video decoding
  - `avcodec_find_decoder()` - Find H264/H265 decoder
  - `avcodec_alloc_context3()` - Create decoder context
  - `avcodec_send_packet()` - Send encoded data
  - `avcodec_receive_frame()` - Get decoded frame
- **libavutil** - Frame and memory management
  - `av_frame_alloc()`, `av_frame_free()`
  - `av_packet_alloc()`, `av_packet_free()`
- **libswscale** - Color space conversion
  - `sws_getContext()` - Create YUV→RGB converter
  - `sws_scale()` - Perform conversion

#### GTK3 / Cairo
- **GTK3** (`libgtk-3`)
  - `gtk_init()` - Initialize GTK
  - `gtk_window_new()` - Create window
  - `gtk_drawing_area_new()` - Create drawing area
  - `gtk_main()` - Run event loop
- **Cairo** (`libcairo`)
  - `cairo_image_surface_create()` - Create RGB surface
  - `cairo_set_source_surface()` - Set surface for drawing
  - `cairo_paint()` - Render to window

## Decoded Frame Structure

```cpp
struct DecodedFrame {
    std::vector<uint8_t> data;  // RGB24 pixel data
    int width;
    int height;
    int64_t pts;                // Presentation timestamp
};
```

## Usage Flow

```cpp
// Initialize
VideoDecoder decoder;
VideoDisplay display;
display.create("Window Title", 1280, 720);

// On first IFrame
decoder.init(iframe.codec);  // H264 or H265

// Decode frames
decoder.decode(iframe, [&display](const DecodedFrame& frame) {
    display.update_frame(frame);
});

// Run display loop
display.run();  // Blocks until window closed
```

## Build Requirements

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED libavcodec libavformat libavutil libswscale)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
```
