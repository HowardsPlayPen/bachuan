#include "client/connection.h"
#include "client/auth.h"
#include "client/stream.h"
#include "video/decoder.h"
#include "video/display.h"
#include "video/writer.h"
#include "rtsp/rtsp_source.h"
#include "utils/logger.h"

#include <iostream>
#include <string>
#include <atomic>
#include <memory>
#include <csignal>
#include <getopt.h>
#include <chrono>
#include <thread>

using namespace baichuan;

// Source type enum
enum class SourceType {
    Baichuan,   // Reolink proprietary protocol
    Rtsp        // Standard RTSP
};

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
              << "Baichuan Protocol Options:\n"
              << "  -h, --host <ip>       Camera IP address (default: 10.0.1.10)\n"
              << "  -p, --port <port>     Camera port (default: 9000)\n"
              << "  -u, --user <name>     Username (default: admin)\n"
              << "  -P, --password <pw>   Password (default: empty)\n"
              << "  -c, --channel <id>    Channel ID (default: 0)\n"
              << "  -s, --stream <type>   Stream type: main, sub, extern (default: main)\n"
              << "  -e, --encryption <t>  Encryption: none, bc, aes (default: aes)\n"
              << "\n"
              << "RTSP Protocol Options:\n"
              << "  -r, --rtsp <url>      RTSP URL (rtsp://[user:pass@]host[:port]/path)\n"
              << "  --transport <tcp|udp> RTSP transport protocol (default: tcp)\n"
              << "\n"
              << "Common Options:\n"
              << "  -i, --img <file>      Capture single snapshot to JPEG file and exit\n"
              << "  -v, --video <file>    Record video to file (mp4/mpg/avi)\n"
              << "  -t, --time <seconds>  Recording duration in seconds (default: 10, 0=until Ctrl+C)\n"
              << "  -d, --debug           Enable debug logging\n"
              << "  --help                Show this help message\n"
              << "\n"
              << "Modes:\n"
              << "  Default:  Display live video in GTK window\n"
              << "  --img:    Capture one frame, save as JPEG, exit\n"
              << "  --video:  Record video for specified duration\n"
              << "\n"
              << "Baichuan Examples:\n"
              << "  " << program << " -h 10.0.1.29 -u admin -P mypassword\n"
              << "  " << program << " -h 10.0.1.29 -P mypassword --img snapshot.jpg\n"
              << "  " << program << " -h 10.0.1.29 -P mypassword --video recording.mp4 -t 30\n"
              << "\n"
              << "RTSP Examples:\n"
              << "  " << program << " --rtsp rtsp://admin:password@10.0.1.29:554/h264Preview_01_main\n"
              << "  " << program << " --rtsp rtsp://10.0.1.29/stream --transport udp\n";
}

// Capture mode enum
enum class CaptureMode {
    Display,    // Live display in GTK window
    Image,      // Single snapshot to JPEG
    Video       // Record video to file
};

int main(int argc, char* argv[]) {
    // Default configuration - Baichuan
    std::string host = "10.0.1.10";
    uint16_t port = 9000;
    std::string username = "admin";
    std::string password;
    uint8_t channel_id = 0;
    std::string stream_type = "main";
    std::string encryption = "bc";
    bool debug = false;

    // RTSP configuration
    SourceType source_type = SourceType::Baichuan;
    std::string rtsp_url;
    std::string rtsp_transport = "tcp";

    // Capture options
    CaptureMode mode = CaptureMode::Display;
    std::string image_file;
    std::string video_file;
    int record_seconds = 10;

    // Parse command line arguments
    static struct option long_options[] = {
        {"host",       required_argument, nullptr, 'h'},
        {"port",       required_argument, nullptr, 'p'},
        {"user",       required_argument, nullptr, 'u'},
        {"password",   required_argument, nullptr, 'P'},
        {"channel",    required_argument, nullptr, 'c'},
        {"stream",     required_argument, nullptr, 's'},
        {"encryption", required_argument, nullptr, 'e'},
        {"rtsp",       required_argument, nullptr, 'r'},
        {"transport",  required_argument, nullptr, 'T'},
        {"img",        required_argument, nullptr, 'i'},
        {"video",      required_argument, nullptr, 'v'},
        {"time",       required_argument, nullptr, 't'},
        {"debug",      no_argument,       nullptr, 'd'},
        {"help",       no_argument,       nullptr, '?'},
        {nullptr,      0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "h:p:u:P:c:s:e:r:T:i:v:t:d", long_options, nullptr)) != -1) {
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
            case 'r':
                rtsp_url = optarg;
                source_type = SourceType::Rtsp;
                break;
            case 'T':
                rtsp_transport = optarg;
                break;
            case 'i':
                image_file = optarg;
                mode = CaptureMode::Image;
                break;
            case 'v':
                video_file = optarg;
                mode = CaptureMode::Video;
                break;
            case 't':
                record_seconds = std::stoi(optarg);
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

    // Initialize GTK only for display mode
    if (mode == CaptureMode::Display) {
        if (!VideoDisplay::init_gtk(&argc, &argv)) {
            LOG_ERROR("Failed to initialize GTK");
            return 1;
        }
    }

    // Install signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Convert encryption string to enum (Baichuan only)
    MaxEncryption max_encryption = MaxEncryption::Aes;
    if (source_type == SourceType::Baichuan) {
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
    }

    LOG_INFO("Baichuan Camera Client");
    const char* mode_str = (mode == CaptureMode::Display) ? "display" :
                           (mode == CaptureMode::Image) ? "snapshot" : "recording";
    LOG_INFO("Mode: {}", mode_str);

    // Get display name based on source type
    std::string source_name;
    if (source_type == SourceType::Rtsp) {
        // Extract host from RTSP URL for display name
        size_t at_pos = rtsp_url.find('@');
        size_t host_start = (at_pos != std::string::npos) ? at_pos + 1 : 7; // after "rtsp://"
        size_t host_end = rtsp_url.find('/', host_start);
        if (host_end == std::string::npos) host_end = rtsp_url.length();
        size_t port_pos = rtsp_url.find(':', host_start);
        if (port_pos != std::string::npos && port_pos < host_end) {
            host_end = port_pos;
        }
        source_name = rtsp_url.substr(host_start, host_end - host_start);
        LOG_INFO("RTSP source: {} (transport: {})", rtsp_url, rtsp_transport);
    } else {
        source_name = host;
        LOG_INFO("Baichuan source: {}:{} user '{}' (encryption: {})", host, port, username, encryption);
    }

    // Mode-specific setup - create display FIRST so we can show status during connection
    std::unique_ptr<VideoDisplay> display;
    std::unique_ptr<VideoWriter> video_writer;
    std::atomic<bool> capture_done{false};
    auto start_time = std::chrono::steady_clock::now();

    if (mode == CaptureMode::Display) {
        // Create display for live viewing
        display = std::make_unique<VideoDisplay>();
        std::string window_title = (source_type == SourceType::Rtsp) ?
            "RTSP - " + source_name : "Baichuan - " + source_name;
        if (!display->create(window_title, 1280, 720)) {
            LOG_ERROR("Failed to create display window");
            return 1;
        }
        // Show initial connection status in window
        std::string status_msg = (source_type == SourceType::Rtsp) ?
            "Connecting to RTSP...\n" + source_name :
            "Connecting to " + host + "...\nUser: " + username;
        display->set_status(status_msg);
        // Process pending GTK events to show the status
        while (gtk_events_pending()) {
            gtk_main_iteration();
        }
    } else if (mode == CaptureMode::Video) {
        // Video writer will be initialized on first frame (we need dimensions)
        video_writer = std::make_unique<VideoWriter>();
        if (record_seconds > 0) {
            LOG_INFO("Recording {} seconds of video to: {}", record_seconds, video_file);
        } else {
            LOG_INFO("Recording video to: {} (press Ctrl+C to stop)", video_file);
        }
    } else if (mode == CaptureMode::Image) {
        LOG_INFO("Capturing snapshot to: {}", image_file);
    }

    // Create video decoder (shared between both protocols)
    VideoDecoder decoder;

    // Shared decode callback for processed frames
    auto decoded_frame_callback = [&](const DecodedFrame& decoded) {
        if (g_quit.load() || capture_done.load()) {
            return;
        }

        switch (mode) {
            case CaptureMode::Display:
                if (display) {
                    display->update_frame(decoded);
                }
                break;

            case CaptureMode::Image:
                // Save single snapshot and exit
                if (ImageWriter::save_jpeg(decoded, image_file)) {
                    LOG_INFO("Snapshot saved successfully");
                } else {
                    LOG_ERROR("Failed to save snapshot");
                }
                capture_done.store(true);
                break;

            case CaptureMode::Video:
                // Initialize video writer on first frame
                if (video_writer && !video_writer->is_open()) {
                    if (!video_writer->open(video_file, decoded.width, decoded.height, 25)) {
                        LOG_ERROR("Failed to open video file: {}", video_file);
                        capture_done.store(true);
                        return;
                    }
                }

                // Write frame
                if (video_writer && video_writer->is_open()) {
                    video_writer->write_frame(decoded);

                    // Check if recording time elapsed
                    if (record_seconds > 0) {
                        auto elapsed = std::chrono::steady_clock::now() - start_time;
                        auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
                        if (elapsed_sec >= record_seconds) {
                            LOG_INFO("Recording time reached ({} seconds)", record_seconds);
                            capture_done.store(true);
                        }
                    }
                }
                break;
        }
    };

    // ========== RTSP Protocol Path ==========
    if (source_type == SourceType::Rtsp) {
        RtspSource rtsp_source;
        rtsp_source.set_url(rtsp_url);
        rtsp_source.set_transport(rtsp_transport);

        // Connect to RTSP source
        if (!rtsp_source.connect()) {
            LOG_ERROR("Failed to connect to RTSP source");
            if (display) {
                display->set_status("RTSP connection failed!\n" + source_name);
            }
            return 1;
        }

        // Update status
        if (display) {
            display->set_status("RTSP connected\nStarting stream...");
            while (gtk_events_pending()) {
                gtk_main_iteration();
            }
        }

        // Handle stream info
        rtsp_source.on_info([](int width, int height, int fps) {
            LOG_INFO("RTSP stream info: {}x{} @ {} fps", width, height, fps);
        });

        // Handle video frames
        rtsp_source.on_frame([&](const uint8_t* data, size_t len, VideoCodec codec) {
            if (g_quit.load() || capture_done.load()) {
                return;
            }

            // Initialize decoder on first frame
            if (!decoder.is_initialized()) {
                if (!decoder.init(codec)) {
                    LOG_ERROR("Failed to initialize decoder");
                    return;
                }
            }

            // Decode frame
            decoder.decode(data, len, decoded_frame_callback);
        });

        // Handle errors
        rtsp_source.on_error([](const std::string& error) {
            LOG_ERROR("RTSP error: {}", error);
        });

        // Handle window close
        if (display) {
            display->on_close([&rtsp_source]() {
                LOG_INFO("Window closed");
                rtsp_source.stop();
            });
        }

        // Start streaming
        if (!rtsp_source.start()) {
            LOG_ERROR("Failed to start RTSP stream");
            return 1;
        }

        // Main loop
        if (mode == CaptureMode::Display) {
            display->run();
        } else {
            while (!g_quit.load() && !capture_done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // Cleanup RTSP
        rtsp_source.stop();

        if (video_writer && video_writer->is_open()) {
            video_writer->close();
        }

        // Print RTSP statistics
        auto decoder_stats = decoder.stats();
        LOG_INFO("RTSP statistics:");
        LOG_INFO("  Frames decoded: {}", decoder_stats.frames_decoded);
        LOG_INFO("  Decode errors: {}", decoder_stats.decode_errors);

        if (video_writer) {
            LOG_INFO("  Video frames written: {}", video_writer->frames_written());
        }
    }
    // ========== Baichuan Protocol Path ==========
    else {
        // Create connection
        Connection conn;
        if (!conn.connect(host, port)) {
            LOG_ERROR("Failed to connect to camera");
            if (display) {
                display->set_status("Connection failed!\n" + host);
            }
            return 1;
        }

        // Update status before authentication
        if (display) {
            display->set_status("Connected to " + host + "\nAuthenticating as " + username + "...");
            while (gtk_events_pending()) {
                gtk_main_iteration();
            }
        }

        // Authenticate
        Authenticator auth(conn);
        auto login_result = auth.login(username, password, max_encryption);
        if (!login_result.success) {
            LOG_ERROR("Login failed: {}", login_result.error_message);
            if (display) {
                display->set_status("Login failed!\n" + login_result.error_message);
            }
            return 1;
        }

        LOG_INFO("Login successful, encryption type: {}", static_cast<int>(login_result.encryption_type));

        // Update status before starting stream
        if (display) {
            display->set_status("Logged in as " + username + "\nStarting video stream...");
            while (gtk_events_pending()) {
                gtk_main_iteration();
            }
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
        stream.on_stream_info([](const BcMediaInfo& info) {
            LOG_INFO("Stream info received: {}x{} @ {} fps",
                     info.video_width, info.video_height, info.fps);
        });

        // Handle video frames
        stream.on_frame([&](const BcMediaFrame& frame) {
            if (g_quit.load() || capture_done.load()) {
                return;
            }

            const BcMediaIFrame* iframe = std::get_if<BcMediaIFrame>(&frame);
            const BcMediaPFrame* pframe = std::get_if<BcMediaPFrame>(&frame);

            if (!iframe && !pframe) {
                return;
            }

            // Initialize decoder on first IFrame
            if (iframe && !decoder.is_initialized()) {
                if (!decoder.init(iframe->codec)) {
                    LOG_ERROR("Failed to initialize decoder");
                    return;
                }
            }

            if (!decoder.is_initialized()) {
                return;
            }

            if (iframe) {
                decoder.decode(*iframe, decoded_frame_callback);
            } else if (pframe) {
                decoder.decode(*pframe, decoded_frame_callback);
            }
        });

        // Handle stream errors
        stream.on_error([](const std::string& error) {
            LOG_ERROR("Stream error: {}", error);
        });

        // Handle window close
        if (display) {
            display->on_close([&stream]() {
                LOG_INFO("Window closed");
                stream.stop();
            });
        }

        // Start stream
        if (!stream.start(stream_config)) {
            LOG_ERROR("Failed to start video stream");
            return 1;
        }

        // Main loop
        if (mode == CaptureMode::Display) {
            display->run();
        } else {
            while (!g_quit.load() && !capture_done.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        // Cleanup Baichuan
        stream.stop();

        if (video_writer && video_writer->is_open()) {
            video_writer->close();
        }

        conn.disconnect();

        // Print Baichuan statistics
        auto stats = stream.stats();
        auto decoder_stats = decoder.stats();
        LOG_INFO("Stream statistics:");
        LOG_INFO("  Frames received: {}", stats.frames_received);
        LOG_INFO("  Bytes received: {}", stats.bytes_received);
        LOG_INFO("  I-Frames: {}", stats.i_frames);
        LOG_INFO("  P-Frames: {}", stats.p_frames);
        LOG_INFO("  Frames decoded: {}", decoder_stats.frames_decoded);
        LOG_INFO("  Decode errors: {}", decoder_stats.decode_errors);

        if (video_writer) {
            LOG_INFO("  Video frames written: {}", video_writer->frames_written());
        }
    }

    LOG_INFO("Shutdown complete");
    return 0;
}
