# Bachuan Architecture Overview

A C++ client for Reolink cameras using the Baichuan (BC) protocol.

## Protocol Overview

The Baichuan protocol is a proprietary binary protocol used by Reolink cameras over TCP port 9000. It supports multiple encryption modes and carries both control messages (XML) and media streams (binary).

### Message Structure

```
┌─────────────────────────────────────────────────────────────┐
│                      BC Message                              │
├─────────────────────────────────────────────────────────────┤
│ Header (20 or 24 bytes)                                     │
│   ├─ Magic (4 bytes): 0x0abcdef0                           │
│   ├─ Message ID (4 bytes)                                   │
│   ├─ Body Length (4 bytes)                                  │
│   ├─ Channel ID (1 byte)                                    │
│   ├─ Stream Type (1 byte)                                   │
│   ├─ Message Number (2 bytes)                               │
│   ├─ Response Code (2 bytes)                                │
│   ├─ Message Class (2 bytes)                                │
│   └─ Payload Offset (4 bytes) [only for 24-byte headers]    │
├─────────────────────────────────────────────────────────────┤
│ Body (body_len bytes)                                       │
│   ├─ Extension (payload_offset bytes) - XML metadata        │
│   └─ Payload (remaining bytes) - XML or binary data         │
└─────────────────────────────────────────────────────────────┘
```

### Message Classes

- `0x6514` - Legacy (20-byte header, used for login negotiation)
- `0x6614` - Modern with 20-byte header
- `0x6414` - Modern with 24-byte header (most common)

### Key Message IDs

- `1` - Login
- `3` - Video streaming
- `4` - Video stop

## Encryption Modes

The protocol supports four encryption modes, negotiated during login.

### Encryption Negotiation

**Important**: The client can only advertise its *maximum* supported encryption level. The camera then decides what to actually use. The client cannot choose between AES and FullAES - the camera makes this decision.

Client request values (sent in legacy login):
- `0x0000` - No encryption supported
- `0x0001` - BCEncrypt max
- `0x0002` - AES max (camera may choose AES or FullAES)

Camera response values:
- `0xdd00` - Using no encryption
- `0xdd01` - Using BCEncrypt
- `0xdd02` - Using AES
- `0xdd12` - Using FullAES

When the user selects `-e aes`, they are requesting "AES-capable encryption". The camera then decides whether to use regular AES (0xdd02) or FullAES (0xdd12). Newer Reolink cameras typically require FullAES, while older models may use regular AES.

If a user wants to avoid FullAES, their only option is to request BCEncrypt (`-e bc`).

### 1. Unencrypted (0xdd00)
No encryption. Rarely used.

### 2. BCEncrypt (0xdd01)
Simple XOR-based encryption with an 8-byte fixed key:
```
key = [0x1F, 0x2D, 0x3C, 0x4B, 0x5A, 0x69, 0x78, 0xFF]
encrypted[i] = data[i] ^ key[(offset + i) % 8] ^ (offset & 0xFF)
```
- Used for: XML payloads
- Binary video data is NOT encrypted

### 3. AES (0xdd02)
AES-128-CFB128 encryption:
- Key derivation: `MD5(nonce + "-" + password)` → uppercase hex → first 16 ASCII bytes
- IV: `"0123456789abcdef"` (16 bytes)
- Used for: XML payloads only
- Binary video data is NOT encrypted

### 4. FullAES (0xdd12)
Same AES algorithm as above, but with partial binary encryption:
- XML payloads: Fully encrypted
- Binary video data: Only first `encryptLen` bytes are encrypted
- `encryptLen` is specified in the Extension XML for each message

## Login Flow

```
┌────────┐                              ┌────────┐
│ Client │                              │ Camera │
└───┬────┘                              └───┬────┘
    │                                       │
    │ 1. Legacy Login Request (class 0x6514)│
    │   (encryption capability in resp_code)│
    │ ─────────────────────────────────────>│
    │                                       │
    │ 2. Encryption Negotiation Response    │
    │   (0xddXX in resp_code, nonce in XML) │
    │ <─────────────────────────────────────│
    │                                       │
    │   [Set up encryption based on 0xddXX] │
    │                                       │
    │ 3. Modern Login Request (class 0x6414)│
    │   (hashed credentials in XML)         │
    │   [Encrypted with BCEncrypt, not AES] │
    │ ─────────────────────────────────────>│
    │                                       │
    │ 4. Login Response                     │
    │   (200 OK or 400 error)               │
    │ <─────────────────────────────────────│
    │                                       │
    │   [Switch to AES for subsequent msgs] │
```

**Important**: During login (msg_id=1), BCEncrypt is used even when AES is negotiated. AES is only used for messages AFTER login succeeds.

### Credential Hashing

```
hashed_username = MD5(username + nonce) → uppercase hex, truncated to 31 chars
hashed_password = MD5(password + nonce) → uppercase hex, truncated to 31 chars
```

## Video Streaming

### Stream Request Flow

```
┌────────┐                              ┌────────┐
│ Client │                              │ Camera │
└───┬────┘                              └───┬────┘
    │                                       │
    │ Preview Request (msg_id=3)            │
    │   <Preview>                           │
    │     <channelId>0</channelId>          │
    │     <handle>0</handle>                │
    │     <streamType>mainStream</streamType>│
    │   </Preview>                          │
    │ ─────────────────────────────────────>│
    │                                       │
    │ Response with Extension               │
    │   <Extension>                         │
    │     <binaryData>1</binaryData>        │
    │     <encryptLen>264</encryptLen>      │
    │   </Extension>                        │
    │ <─────────────────────────────────────│
    │                                       │
    │ BcMedia frames (video/audio data)     │
    │ <─────────────────────────────────────│
```

### Binary Mode Tracking

Once a `msg_num` receives a message with `<binaryData>1</binaryData>`, all subsequent messages with that `msg_num` are treated as binary, even if they don't have the tag.

### BcMedia Frame Format

Video and audio data is encapsulated in BcMedia frames:

```
┌─────────────────────────────────────────┐
│ BcMedia Frame                           │
├─────────────────────────────────────────┤
│ Magic (4 bytes)                         │
│   - Info V1: 0x31303031 ("1001")        │
│   - Info V2: 0x32303031 ("2001")        │
│   - IFrame:  0x63643030-39 ("00dc"-"09dc")│
│   - PFrame:  0x63643130-39 ("01dc"-"19dc")│
│   - AAC:     0x62773530 ("05wb")        │
│   - ADPCM:   0x62773130 ("01wb")        │
├─────────────────────────────────────────┤
│ Frame-specific header                   │
├─────────────────────────────────────────┤
│ Payload data                            │
├─────────────────────────────────────────┤
│ Padding (to 8-byte alignment)           │
└─────────────────────────────────────────┘
```

### Video Frame Structure (IFrame/PFrame)

```
Offset  Size  Field
0       4     Video type ("H264" or "H265")
4       4     Payload size (bytes)
8       4     Additional header size
12      4     Microseconds timestamp
16      4     Unknown
20      N     Additional header (if size > 0)
20+N    M     Video data (NAL units)
```

## Component Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         main.cpp                             │
│                    (CLI & GTK setup)                         │
└─────────────────────────┬───────────────────────────────────┘
                          │
         ┌────────────────┼────────────────┐
         ▼                ▼                ▼
┌─────────────┐   ┌─────────────┐   ┌─────────────┐
│ Connection  │   │Authenticator│   │ VideoStream │
│             │   │             │   │             │
│ - Socket I/O│   │ - Login flow│   │ - Stream req│
│ - Encryption│   │ - Credential│   │ - BcMedia   │
│ - Msg send/ │   │   hashing   │   │   parsing   │
│   receive   │   │             │   │             │
└──────┬──────┘   └─────────────┘   └──────┬──────┘
       │                                    │
       ▼                                    ▼
┌─────────────────────────────────────────────────────────────┐
│                      Protocol Layer                          │
├─────────────┬─────────────┬─────────────┬───────────────────┤
│  BcHeader   │  BcCrypto   │   BcXml     │    BcMedia        │
│             │             │             │                   │
│ - Header    │ - BCEncrypt │ - XML       │ - Frame parsing   │
│   parse/    │ - AES-128   │   serialize │ - Video/audio     │
│   serialize │ - Key derive│ - XML parse │   extraction      │
└─────────────┴─────────────┴─────────────┴───────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────┐
│                       Video Layer                            │
├─────────────────────────┬───────────────────────────────────┤
│      VideoDecoder       │         VideoDisplay              │
│                         │                                   │
│ - FFmpeg H264/H265      │ - GTK3 window                     │
│ - Frame decoding        │ - Cairo rendering                 │
│                         │ - YUV→RGB conversion              │
└─────────────────────────┴───────────────────────────────────┘
```

## Key Files

```
bachuan/
├── src/
│   ├── main.cpp              # Entry point, CLI parsing
│   ├── client/
│   │   ├── connection.cpp    # TCP connection, encryption
│   │   ├── auth.cpp          # Login flow
│   │   └── stream.cpp        # Video stream handling
│   ├── protocol/
│   │   ├── bc_header.cpp     # BC message header
│   │   ├── bc_crypto.cpp     # Encryption (BCEncrypt, AES)
│   │   ├── bc_xml.cpp        # XML serialization/parsing
│   │   └── bc_media.cpp      # BcMedia frame parsing
│   ├── video/
│   │   ├── decoder.cpp       # FFmpeg video decoding
│   │   └── display.cpp       # GTK display
│   └── utils/
│       ├── logger.h          # Logging utilities
│       └── md5.cpp           # MD5 hashing
└── ARCHITECTURE.md           # This file
```

## FullAES Binary Encryption Details

For FullAES (0xdd12), binary video data is partially encrypted:

1. **Messages with Extension containing `encryptLen`**:
   - First `encryptLen` bytes of payload are AES encrypted
   - Remaining bytes are cleartext
   - Decrypt first part, concatenate with cleartext

2. **Messages without Extension** (continuation packets):
   - Check if `msg_num` is in binary mode (from previous message)
   - If in binary mode: payload is cleartext, don't decrypt
   - If not in binary mode: treat as XML, decrypt

3. **Extension XML example**:
   ```xml
   <Extension version="1.1">
     <encryptLen>264</encryptLen>
     <binaryData>1</binaryData>
     <checkPos>0</checkPos>
     <checkValue>1651979568</checkValue>
   </Extension>
   ```

## Dependencies

- **OpenSSL** - AES encryption
- **libxml2** - XML parsing
- **FFmpeg** (libavcodec, libavformat, libswscale) - Video decoding
- **GTK3** - Display window
- **Cairo** - 2D rendering

## Building

```bash
mkdir build && cd build
cmake ..
make -j4
```

## Usage

```bash
./bachuan -h <camera_ip> -u <username> -P <password> [-e bc|aes] [-d]
```

Options:
- `-h, --host` - Camera IP address
- `-p, --port` - Camera port (default: 9000)
- `-u, --user` - Username (default: admin)
- `-P, --password` - Password
- `-c, --channel` - Channel ID (default: 0)
- `-s, --stream` - Stream type: main, sub, extern (default: main)
- `-e, --encryption` - Encryption: none, bc, aes (default: aes)
- `-d, --debug` - Enable debug logging

## References

- [Neolink](https://github.com/thirtythreeforty/neolink) - Rust implementation (reference)
- Reolink camera protocol reverse engineering
