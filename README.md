# Bachuan

A C++ client for viewing Reolink camera streams using the Baichuan (BC) protocol.

## Applications

This project builds two applications:

### baichuan

Single camera viewer with GTK display. Connect to one Reolink camera and view its live video stream.

```bash
./baichuan -h <camera_ip> -u <username> -P <password> [options]
```

Options:
- `-h, --host <ip>` - Camera IP address
- `-p, --port <port>` - Camera port (default: 9000)
- `-u, --user <name>` - Username (default: admin)
- `-P, --password <pw>` - Password
- `-c, --channel <id>` - Channel ID (default: 0)
- `-s, --stream <type>` - Stream type: main, sub, extern (default: main)
- `-e, --encryption <t>` - Encryption: none, bc, aes (default: aes)
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

Configuration file format:
```json
{
  "columns": 2,
  "cameras": [
    {
      "name": "Front Door",
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
      "host": "192.168.1.101",
      "username": "admin",
      "password": "password123"
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

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed protocol documentation, encryption modes, message formats, and component architecture.

## References

- [Neolink](https://github.com/QuantumEntangledAndy/neolink) - Rust implementation (reference)
- [Hacking Reolink Cameras](https://www.thirtythreeforty.net/posts/2020/05/hacking-reolink-cameras-for-fun-and-profit/) - Protocol reverse engineering
