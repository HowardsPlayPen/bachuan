# Baichuan

A C++ client for viewing IP camera streams. Supports the Reolink proprietary Baichuan (BC) protocol, standard RTSP, and HTTP MJPEG streams.

## Features

- **Baichuan Protocol** - Native support for Reolink cameras with encryption (BCEncrypt, AES)
- **RTSP Protocol** - Connect to any RTSP-compatible camera (via FFmpeg)
- **MJPEG Protocol** - Connect to HTTP MJPEG streams (via libjpeg)
- **Live Display** - Real-time video in GTK window
- **Snapshot Capture** - Save JPEG images
- **Video Recording** - Record to MP4/MPEG files
- **Multi-Camera Dashboard** - View multiple cameras in a grid layout (mixed protocols supported)

## Applications

This project builds two applications:

## Why?

I own several reolink cameras and do NOT want to use the Windows native application 
- I mainly use (Arch) Linux and the Windows app did not run
- I do not trust the Windows app + it does not do everything I would like

I searched and the best option seemed to be to use the Rust application below to act as a reolink -> rtsp bridge then use a video client like VLC. All this did was create a configuration headache and a REALLY laggy video (several seconds lag).

I therefore decided to use Claude to port the Rust to a language I understand and give me straight out of the box a video display -so NOW I can connect to my reolinks on linux and have a real time display. I am now much happier

### baichuan

Single camera viewer with GTK display. Connect to one camera and view its live video stream.

**Baichuan Protocol (Reolink native):**
```bash
./baichuan -h <camera_ip> -u <username> -P <password> [options]
```

**RTSP Protocol:**
```bash
./baichuan --rtsp rtsp://user:pass@camera_ip:554/stream [options]
```

**MJPEG Protocol:**
```bash
./baichuan --mjpeg http://user:pass@camera_ip/mjpeg [options]
```

Baichuan Options:
- `-h, --host <ip>` - Camera IP address
- `-p, --port <port>` - Camera port (default: 9000)
- `-u, --user <name>` - Username (default: admin)
- `-P, --password <pw>` - Password
- `-c, --channel <id>` - Channel ID (default: 0)
- `-s, --stream <type>` - Stream type: main, sub, extern (default: main)
- `-e, --encryption <t>` - Encryption: none, bc, aes (default: aes)

RTSP Options:
- `-r, --rtsp <url>` - RTSP URL (rtsp://[user:pass@]host[:port]/path)
- `--transport <tcp|udp>` - RTSP transport protocol (default: tcp)

MJPEG Options:
- `-m, --mjpeg <url>` - MJPEG URL (http://[user:pass@]host[:port]/path)

Common Options:
- `-i, --img <file>` - Capture single snapshot to JPEG file
- `-v, --video <file>` - Record video to file (mp4/mpg/avi)
- `-t, --time <seconds>` - Recording duration (default: 10)
- `-d, --debug` - Enable debug logging

### dashboard

Multi-camera viewer. Display multiple camera streams in a grid layout, configured via JSON file. Supports runtime control via Unix domain sockets and/or TCP sockets.

```bash
./dashboard -c <config.json> [options]
```

Options:
- `-c, --config <file>` - JSON configuration file (required)
- `-d, --debug` - Enable debug logging
- `-H, --hidden` - Start with the window hidden (headless mode, control via socket)

#### Configuration

```json
{
  "columns": 2,
  "control": {
    "unix": "/tmp/dash.sock",
    "tcp_port": 9100
  },
  "cameras": [
    {
      "name": "Front Door",
      "type": "baichuan",
      "host": "192.168.1.100",
      "port": 9000,
      "username": "admin",
      "password": "password123",
      "encryption": "aes",
      "stream": "main",
      "channel": 0
    },
    {
      "name": "Back Yard",
      "type": "rtsp",
      "url": "rtsp://admin:password@192.168.1.101:554/h264Preview_01_main",
      "transport": "tcp"
    },
    {
      "name": "Garage",
      "type": "mjpeg",
      "url": "http://admin:password@192.168.1.102/mjpeg"
    }
  ]
}
```

| Field | Description |
|-------|-------------|
| `columns` | Number of grid columns (default: 2) |
| `control.unix` | Unix domain socket path for runtime commands (optional) |
| `control.tcp_port` | TCP port for runtime commands (optional) |
| `cameras[].name` | Display name |
| `cameras[].type` | `baichuan`, `rtsp`, or `mjpeg` |
| `cameras[].url` | Stream URL (RTSP/MJPEG only) |
| `cameras[].transport` | `tcp` or `udp` (RTSP only, default: tcp) |
| `cameras[].host` | Camera IP (Baichuan only) |
| `cameras[].port` | Camera port (Baichuan only, default: 9000) |
| `cameras[].username` | Username (Baichuan only) |
| `cameras[].password` | Password (Baichuan only) |
| `cameras[].encryption` | `none`, `bc`, or `aes` (Baichuan only) |
| `cameras[].stream` | `main`, `sub`, or `extern` (Baichuan only) |
| `cameras[].channel` | Channel ID (Baichuan only, default: 0) |

#### Runtime Control Commands

When `control` is configured, the dashboard accepts newline-delimited JSON commands over Unix socket or TCP. All commands return `{"ok": true}` on success or `{"error": "message"}` on failure.

**View control:**
```bash
# Show only specific feeds (by index)
echo '{"show": 0}' | socat - UNIX-CONNECT:/tmp/dash.sock
echo '{"show": [0, 2]}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Show only specific feeds and disconnect hidden cameras to save resources
echo '{"show": 0, "disconnect": true}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Restore all feeds (reconnects any disconnected cameras)
echo '{"show_all": true}' | socat - UNIX-CONNECT:/tmp/dash.sock
```

**Connection control:**
```bash
# Disconnect specific cameras (stops stream, frees resources, pane stays)
echo '{"disconnect": [1, 2]}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Disconnect all cameras
echo '{"disconnect": true}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Reconnect specific cameras
echo '{"connect": [1, 2]}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Reconnect all cameras
echo '{"connect": true}' | socat - UNIX-CONNECT:/tmp/dash.sock
```

**Window control:**
```bash
# Hide the window (keeps streams running)
echo '{"hide_ui": true}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Show the window
echo '{"show_ui": true}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Enter fullscreen
echo '{"fullscreen": true}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Exit fullscreen
echo '{"fullscreen": false}' | socat - UNIX-CONNECT:/tmp/dash.sock
```

**Add a camera at runtime:**
```bash
# Add a new camera to the grid
echo '{"add": {"name":"New Cam", "type":"rtsp", "url":"rtsp://..."}}' | socat - UNIX-CONNECT:/tmp/dash.sock

# Add a camera and replace all existing feeds with it
echo '{"add": {"name":"Solo", "type":"rtsp", "url":"rtsp://...", "replace": true}}' | socat - UNIX-CONNECT:/tmp/dash.sock
```

**Query status:**
```bash
# List all feeds with visibility and connection state
echo '{"list": true}' | socat - UNIX-CONNECT:/tmp/dash.sock
# Returns: {"ok": true, "feeds": [{"index": 0, "name": "Front", "visible": true, "connected": true}, ...]}
```

All commands also work via TCP: `echo '{"list": true}' | nc localhost 9100`

## Building

```bash
mkdir build && cd build
cmake ..
make -j4
```

## Dependencies

- OpenSSL - AES encryption
- libxml2 - XML parsing
- FFmpeg (libavcodec, libavformat, libswscale) - Video decoding
- GTK3 - Display window
- Cairo - 2D rendering

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) - Detailed protocol documentation, encryption modes, message formats, and component architecture
- [src/rtsp/README.md](src/rtsp/README.md) - RTSP module documentation and common camera URL formats

## References

- [Neolink](https://github.com/QuantumEntangledAndy/neolink) - Rust implementation (reference)
- [Hacking Reolink Cameras](https://www.thirtythreeforty.net/posts/2020/05/hacking-reolink-cameras-for-fun-and-profit/) - Protocol reverse engineering
