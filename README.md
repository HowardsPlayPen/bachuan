# Baichuan

A C++ client for viewing IP camera streams. Supports both the Reolink proprietary Baichuan (BC) protocol and standard RTSP.

## Features

- **Baichuan Protocol** - Native support for Reolink cameras with encryption (BCEncrypt, AES)
- **RTSP Protocol** - Connect to any RTSP-compatible camera
- **Live Display** - Real-time video in GTK window
- **Snapshot Capture** - Save JPEG images
- **Video Recording** - Record to MP4/MPEG files
- **Multi-Camera Dashboard** - View multiple cameras in a grid layout

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

Common Options:
- `-i, --img <file>` - Capture single snapshot to JPEG file
- `-v, --video <file>` - Record video to file (mp4/mpg/avi)
- `-t, --time <seconds>` - Recording duration (default: 10)
- `-d, --debug` - Enable debug logging

### dashboard

Multi-camera viewer. Display multiple camera streams in a grid layout, configured via JSON file.

```bash
./dashboard -c <config.json> [options]
```

Options:
- `-c, --config <file>` - JSON configuration file (required)
- `-d, --debug` - Enable debug logging

Configuration file format (supports mixed Baichuan and RTSP cameras):
```json
{
  "columns": 2,
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
    }
  ]
}
```

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
