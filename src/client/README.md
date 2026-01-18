# Client Layer

High-level client functionality for connecting to and communicating with Reolink cameras.

## Files

| File | Purpose |
|------|---------|
| `connection.cpp/h` | TCP socket connection, message send/receive, encryption handling |
| `auth.cpp/h` | Login flow, credential hashing, encryption negotiation |
| `stream.cpp/h` | Video stream requests, BcMedia frame accumulation and parsing |

## Responsibilities

### Connection
- TCP socket management (connect, disconnect, send, receive)
- Message serialization/deserialization
- Encryption/decryption of message payloads
- Binary mode tracking per `msg_num` (for FullAES)
- Thread-safe send/receive with mutexes

### Authenticator
- Three-step login flow:
  1. Legacy login (encryption negotiation)
  2. Parse nonce from camera response
  3. Modern login with hashed credentials
- Credential hashing: `MD5(username + nonce)`, `MD5(password + nonce)`
- AES key derivation from password and nonce
- Switching encryption after successful login

### VideoStream
- Send preview start/stop requests
- Receive and accumulate video message payloads
- Parse BcMedia frames from accumulated data
- Callback system for frame delivery
- Statistics tracking (frames received, I/P frame counts)

## Dependencies

### Internal
- `protocol/` - BC protocol types, encryption, XML, media parsing

### External
- **None** - This layer uses only standard library and internal components

## Usage Flow

```
1. Connection::connect(host, port)
2. Authenticator::login(username, password, max_encryption)
   - Negotiates encryption
   - Sets up Connection's crypto
3. VideoStream::start(config)
   - Sends preview request
   - Starts receive loop
4. VideoStream callbacks deliver frames
5. VideoStream::stop()
6. Connection::disconnect()
```
