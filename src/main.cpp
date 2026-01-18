#include "client/connection.h"
#include "client/auth.h"
#include "client/stream.h"
#include "video/decoder.h"
#include "video/display.h"
#include "utils/logger.h"

#include <iostream>
#include <string>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <getopt.h>

using namespace bachuan;

// Global flag for signal handling
static std::atomic<bool> g_quit{false};

void signal_handler(int signum) {
    (void)signum;
    LOG_INFO("Received signal, shutting down...");
    g_quit.store(true);
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  -h, --host <ip>       Camera IP address (default: 10.0.1.29)\n"
              << "  -p, --port <port>     Camera port (default: 9000)\n"
              << "  -u, --user <name>     Username (default: admin)\n"
              << "  -P, --password <pw>   Password (default: empty)\n"
              << "  -c, --channel <id>    Channel ID (default: 0)\n"
              << "  -s, --stream <type>   Stream type: main, sub, extern (default: main)\n"
              << "  -e, --encryption <t>  Encryption: none, bc, aes (default: aes)\n"
              << "  -d, --debug           Enable debug logging\n"
              << "  --help                Show this help message\n"
              << "\n"
              << "Example:\n"
              << "  " << program << " -h 10.0.1.29 -u admin -P mypassword -e bc\n";
}

int main(int argc, char* argv[]) {
    // Default configuration
    std::string host = "10.0.1.10";
    uint16_t port = 9000;
    std::string username = "admin";
    std::string password;
    uint8_t channel_id = 0;
    std::string stream_type = "main";
    std::string encryption = "bc";
    bool debug = false;

    // Parse command line arguments
    static struct option long_options[] = {
        {"host",       required_argument, nullptr, 'h'},
        {"port",       required_argument, nullptr, 'p'},
        {"user",       required_argument, nullptr, 'u'},
        {"password",   required_argument, nullptr, 'P'},
        {"channel",    required_argument, nullptr, 'c'},
        {"stream",     required_argument, nullptr, 's'},
        {"encryption", required_argument, nullptr, 'e'},
        {"debug",      no_argument,       nullptr, 'd'},
        {"help",       no_argument,       nullptr, '?'},
        {nullptr,      0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:u:P:c:s:e:d", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'h':
                host = optarg;
                break;
            case 'p':
                port = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'u':
                username = optarg;
                break;
            case 'P':
                password = optarg;
                break;
            case 'c':
                channel_id = static_cast<uint8_t>(std::stoi(optarg));
                break;
            case 's':
                stream_type = optarg;
                break;
            case 'e':
                encryption = optarg;
                break;
            case 'd':
                debug = true;
                break;
            case '?':
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    // Configure logging
    if (debug) {
        Logger::instance().set_level(LogLevel::Debug);
    }

    // Initialize GTK
    if (!VideoDisplay::init_gtk(&argc, &argv)) {
        LOG_ERROR("Failed to initialize GTK");
        return 1;
    }

    // Install signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Convert encryption string to enum
    MaxEncryption max_encryption = MaxEncryption::Aes;
    if (encryption == "none") {
        max_encryption = MaxEncryption::None;
    } else if (encryption == "bc") {
        max_encryption = MaxEncryption::BCEncrypt;
    } else if (encryption == "aes") {
        max_encryption = MaxEncryption::Aes;
    } else {
        std::cerr << "Unknown encryption type: " << encryption << "\n";
        print_usage(argv[0]);
        return 1;
    }

    LOG_INFO("Bachuan Camera Client");
    LOG_INFO("Connecting to {}:{} as user '{}' (encryption: {})", host, port, username, encryption);

    // Create connection
    Connection conn;
    if (!conn.connect(host, port)) {
        LOG_ERROR("Failed to connect to camera");
        return 1;
    }

    // Authenticate
    Authenticator auth(conn);
    auto login_result = auth.login(username, password, max_encryption);
    if (!login_result.success) {
        LOG_ERROR("Login failed: {}", login_result.error_message);
        return 1;
    }

    LOG_INFO("Login successful, encryption type: {}", static_cast<int>(login_result.encryption_type));

    // Create video decoder
    VideoDecoder decoder;

    // Create display
    VideoDisplay display;
    if (!display.create("Bachuan Camera - " + host, 1280, 720)) {
        LOG_ERROR("Failed to create display window");
        return 1;
    }

    // Configure stream
    StreamConfig stream_config;
    stream_config.channel_id = channel_id;

    if (stream_type == "main") {
        stream_config.handle = STREAM_HANDLE_MAIN;
        stream_config.stream_type = "mainStream";
    } else if (stream_type == "sub") {
        stream_config.handle = STREAM_HANDLE_SUB;
        stream_config.stream_type = "subStream";
    } else if (stream_type == "extern") {
        stream_config.handle = STREAM_HANDLE_EXTERN;
        stream_config.stream_type = "externStream";
    }

    // Create video stream
    VideoStream stream(conn);

    // Handle stream info
    stream.on_stream_info([&decoder](const BcMediaInfo& info) {
        LOG_INFO("Stream info received: {}x{} @ {} fps",
                 info.video_width, info.video_height, info.fps);
    });

    // Handle video frames
    stream.on_frame([&decoder, &display](const BcMediaFrame& frame) {
        // Check if it's a video frame
        if (auto* iframe = std::get_if<BcMediaIFrame>(&frame)) {
            // Initialize decoder on first IFrame if not already done
            if (!decoder.is_initialized()) {
                if (!decoder.init(iframe->codec)) {
                    LOG_ERROR("Failed to initialize decoder");
                    return;
                }
            }

            // Decode and display
            decoder.decode(*iframe, [&display](const DecodedFrame& decoded) {
                display.update_frame(decoded);
            });
        }
        else if (auto* pframe = std::get_if<BcMediaPFrame>(&frame)) {
            if (decoder.is_initialized()) {
                decoder.decode(*pframe, [&display](const DecodedFrame& decoded) {
                    display.update_frame(decoded);
                });
            }
        }
    });

    // Handle stream errors
    stream.on_error([](const std::string& error) {
        LOG_ERROR("Stream error: {}", error);
    });

    // Handle window close
    display.on_close([&stream]() {
        LOG_INFO("Window closed");
        stream.stop();
    });

    // Start stream
    if (!stream.start(stream_config)) {
        LOG_ERROR("Failed to start video stream");
        return 1;
    }

    // Run GTK main loop
    display.run();

    // Cleanup
    stream.stop();
    conn.disconnect();

    // Print statistics
    auto stats = stream.stats();
    auto decoder_stats = decoder.stats();
    LOG_INFO("Stream statistics:");
    LOG_INFO("  Frames received: {}", stats.frames_received);
    LOG_INFO("  Bytes received: {}", stats.bytes_received);
    LOG_INFO("  I-Frames: {}", stats.i_frames);
    LOG_INFO("  P-Frames: {}", stats.p_frames);
    LOG_INFO("  Frames decoded: {}", decoder_stats.frames_decoded);
    LOG_INFO("  Decode errors: {}", decoder_stats.decode_errors);

    LOG_INFO("Shutdown complete");
    return 0;
}
