#include "client/connection.h"
#include "client/auth.h"
#include "client/stream.h"
#include "video/decoder.h"
#include "video/dashboard_display.h"
#include "rtsp/rtsp_source.h"
#include "utils/logger.h"
#include "utils/json_config.h"

#include <iostream>
#include <string>
#include <atomic>
#include <memory>
#include <vector>
#include <thread>
#include <csignal>
#include <getopt.h>

using namespace baichuan;

// Global flag for signal handling
static std::atomic<bool> g_quit{false};

void signal_handler(int signum) {
    (void)signum;
    LOG_INFO("Received signal, shutting down...");
    g_quit.store(true);
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " -c <config.json> [options]\n"
              << "\n"
              << "Options:\n"
              << "  -c, --config <file>   JSON configuration file (required)\n"
              << "  -d, --debug           Enable debug logging\n"
              << "  --help                Show this help message\n"
              << "\n"
              << "Configuration file format (Baichuan camera):\n"
              << "  {\n"
              << "    \"columns\": 2,\n"
              << "    \"cameras\": [\n"
              << "      {\n"
              << "        \"name\": \"Front Door\",\n"
              << "        \"type\": \"baichuan\",\n"
              << "        \"host\": \"192.168.1.100\",\n"
              << "        \"port\": 9000,\n"
              << "        \"username\": \"admin\",\n"
              << "        \"password\": \"password123\",\n"
              << "        \"encryption\": \"aes\",\n"
              << "        \"stream\": \"main\",\n"
              << "        \"channel\": 0\n"
              << "      }\n"
              << "    ]\n"
              << "  }\n"
              << "\n"
              << "Configuration file format (RTSP camera):\n"
              << "  {\n"
              << "    \"cameras\": [\n"
              << "      {\n"
              << "        \"name\": \"Back Yard\",\n"
              << "        \"type\": \"rtsp\",\n"
              << "        \"url\": \"rtsp://admin:password@192.168.1.101:554/stream\",\n"
              << "        \"transport\": \"tcp\"\n"
              << "      }\n"
              << "    ]\n"
              << "  }\n"
              << "\n"
              << "Example:\n"
              << "  " << program << " -c cameras.json\n";
}

// Per-camera context
struct CameraContext {
    size_t index;
    CameraConfig config;
    // Baichuan-specific
    std::unique_ptr<Connection> connection;
    std::unique_ptr<VideoStream> stream;
    // RTSP-specific
    std::unique_ptr<RtspSource> rtsp_source;
    // Shared
    std::unique_ptr<VideoDecoder> decoder;
    std::thread worker_thread;
    std::atomic<bool> running{false};
};

MaxEncryption string_to_encryption(const std::string& enc) {
    if (enc == "none") return MaxEncryption::None;
    if (enc == "bc") return MaxEncryption::BCEncrypt;
    return MaxEncryption::Aes;
}

// RTSP camera worker
void rtsp_camera_worker(CameraContext* ctx, DashboardDisplay* display) {
    LOG_INFO("Camera {} (RTSP: {}) starting...", ctx->index, ctx->config.name);

    display->set_status(ctx->index, "Connecting RTSP...");

    // Create RTSP source
    ctx->rtsp_source = std::make_unique<RtspSource>();
    ctx->rtsp_source->set_url(ctx->config.url);
    ctx->rtsp_source->set_transport(ctx->config.transport);

    if (!ctx->rtsp_source->connect()) {
        LOG_ERROR("Camera {}: RTSP connection failed", ctx->index);
        display->set_status(ctx->index, "RTSP failed");
        return;
    }

    display->set_status(ctx->index, "Starting stream...");

    // Create decoder
    ctx->decoder = std::make_unique<VideoDecoder>();

    // Handle stream info
    ctx->rtsp_source->on_info([ctx](int width, int height, int fps) {
        LOG_INFO("Camera {} (RTSP): Stream {}x{} @ {} fps", ctx->index, width, height, fps);
    });

    // Handle video frames
    ctx->rtsp_source->on_frame([ctx, display](const uint8_t* data, size_t len, VideoCodec codec) {
        if (!ctx->running.load()) return;

        // Initialize decoder on first frame
        if (!ctx->decoder->is_initialized()) {
            if (!ctx->decoder->init(codec)) {
                LOG_ERROR("Camera {}: Failed to initialize decoder", ctx->index);
                return;
            }
        }

        // Decode and display
        ctx->decoder->decode(data, len, [ctx, display](const DecodedFrame& decoded) {
            display->update_frame(ctx->index, decoded);
        });
    });

    // Handle errors
    ctx->rtsp_source->on_error([ctx, display](const std::string& error) {
        LOG_ERROR("Camera {} (RTSP): Error: {}", ctx->index, error);
        display->set_status(ctx->index, "Error: " + error);
    });

    // Start streaming
    ctx->running.store(true);
    if (!ctx->rtsp_source->start()) {
        LOG_ERROR("Camera {}: Failed to start RTSP stream", ctx->index);
        display->set_status(ctx->index, "Stream failed");
        ctx->running.store(false);
        return;
    }

    // Wait until quit requested
    while (ctx->running.load() && !g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    ctx->rtsp_source->stop();
    LOG_INFO("Camera {} (RTSP): Stopped", ctx->index);
}

// Baichuan camera worker
void baichuan_camera_worker(CameraContext* ctx, DashboardDisplay* display) {
    LOG_INFO("Camera {} ({}) starting...", ctx->index, ctx->config.host);

    display->set_status(ctx->index, "Connecting...");

    // Create connection
    ctx->connection = std::make_unique<Connection>();
    if (!ctx->connection->connect(ctx->config.host, ctx->config.port)) {
        LOG_ERROR("Camera {}: Failed to connect", ctx->index);
        display->set_status(ctx->index, "Connection failed");
        return;
    }

    display->set_status(ctx->index, "Authenticating...");

    // Authenticate
    Authenticator auth(*ctx->connection);
    auto login_result = auth.login(ctx->config.username, ctx->config.password,
                                   string_to_encryption(ctx->config.encryption));
    if (!login_result.success) {
        LOG_ERROR("Camera {}: Login failed: {}", ctx->index, login_result.error_message);
        display->set_status(ctx->index, "Login failed");
        return;
    }

    LOG_INFO("Camera {}: Login successful", ctx->index);
    display->set_status(ctx->index, "Starting stream...");

    // Create decoder
    ctx->decoder = std::make_unique<VideoDecoder>();

    // Configure stream
    StreamConfig stream_config;
    stream_config.channel_id = ctx->config.channel;

    if (ctx->config.stream == "sub") {
        stream_config.handle = STREAM_HANDLE_SUB;
        stream_config.stream_type = "subStream";
    } else if (ctx->config.stream == "extern") {
        stream_config.handle = STREAM_HANDLE_EXTERN;
        stream_config.stream_type = "externStream";
    } else {
        stream_config.handle = STREAM_HANDLE_MAIN;
        stream_config.stream_type = "mainStream";
    }

    // Create video stream
    ctx->stream = std::make_unique<VideoStream>(*ctx->connection);

    // Handle stream info
    ctx->stream->on_stream_info([ctx](const BcMediaInfo& info) {
        LOG_INFO("Camera {}: Stream {}x{} @ {} fps",
                 ctx->index, info.video_width, info.video_height, info.fps);
    });

    // Handle video frames
    ctx->stream->on_frame([ctx, display](const BcMediaFrame& frame) {
        if (!ctx->running.load()) return;

        const BcMediaIFrame* iframe = std::get_if<BcMediaIFrame>(&frame);
        const BcMediaPFrame* pframe = std::get_if<BcMediaPFrame>(&frame);

        if (!iframe && !pframe) return;

        // Initialize decoder on first IFrame
        if (iframe && !ctx->decoder->is_initialized()) {
            if (!ctx->decoder->init(iframe->codec)) {
                LOG_ERROR("Camera {}: Failed to initialize decoder", ctx->index);
                return;
            }
        }

        if (!ctx->decoder->is_initialized()) return;

        // Decode and display
        auto decode_callback = [ctx, display](const DecodedFrame& decoded) {
            display->update_frame(ctx->index, decoded);
        };

        if (iframe) {
            ctx->decoder->decode(*iframe, decode_callback);
        } else if (pframe) {
            ctx->decoder->decode(*pframe, decode_callback);
        }
    });

    // Handle errors
    ctx->stream->on_error([ctx, display](const std::string& error) {
        LOG_ERROR("Camera {}: Stream error: {}", ctx->index, error);
        display->set_status(ctx->index, "Error: " + error);
    });

    // Start stream
    ctx->running.store(true);
    if (!ctx->stream->start(stream_config)) {
        LOG_ERROR("Camera {}: Failed to start stream", ctx->index);
        display->set_status(ctx->index, "Stream failed");
        ctx->running.store(false);
        return;
    }

    // Wait until quit requested
    while (ctx->running.load() && !g_quit.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    ctx->stream->stop();
    ctx->connection->disconnect();
    LOG_INFO("Camera {}: Stopped", ctx->index);
}

// Dispatch to appropriate worker based on camera type
void camera_worker(CameraContext* ctx, DashboardDisplay* display) {
    if (ctx->config.type == CameraType::Rtsp) {
        rtsp_camera_worker(ctx, display);
    } else {
        baichuan_camera_worker(ctx, display);
    }
}

int main(int argc, char* argv[]) {
    std::string config_file;
    bool debug = false;

    // Parse command line arguments
    static struct option long_options[] = {
        {"config",  required_argument, nullptr, 'c'},
        {"debug",   no_argument,       nullptr, 'd'},
        {"help",    no_argument,       nullptr, '?'},
        {nullptr,   0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:d", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
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

    if (config_file.empty()) {
        std::cerr << "Error: Configuration file required\n\n";
        print_usage(argv[0]);
        return 1;
    }

    // Configure logging
    if (debug) {
        Logger::instance().set_level(LogLevel::Debug);
    }

    LOG_INFO("Baichuan Dashboard");

    // Parse configuration
    DashboardConfig config;
    try {
        config = JsonConfigParser::parse(config_file);
        LOG_INFO("Loaded {} cameras from config", config.cameras.size());
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse config: {}", e.what());
        return 1;
    }

    if (config.cameras.empty()) {
        LOG_ERROR("No cameras defined in config");
        return 1;
    }

    // Initialize GTK
    if (!DashboardDisplay::init_gtk(&argc, &argv)) {
        LOG_ERROR("Failed to initialize GTK");
        return 1;
    }

    // Install signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Create dashboard display
    DashboardDisplay display;
    if (!display.create("Baichuan Dashboard", config.cameras, config.columns)) {
        LOG_ERROR("Failed to create dashboard");
        return 1;
    }

    // Create camera contexts and start workers
    std::vector<std::unique_ptr<CameraContext>> cameras;
    for (size_t i = 0; i < config.cameras.size(); i++) {
        auto ctx = std::make_unique<CameraContext>();
        ctx->index = i;
        ctx->config = config.cameras[i];
        cameras.push_back(std::move(ctx));
    }

    // Start camera worker threads
    for (auto& ctx : cameras) {
        ctx->worker_thread = std::thread(camera_worker, ctx.get(), &display);
    }

    // Handle quit
    display.on_quit([&cameras]() {
        g_quit.store(true);
        for (auto& ctx : cameras) {
            ctx->running.store(false);
        }
    });

    // Run GTK main loop
    display.run();

    // Signal all cameras to stop
    g_quit.store(true);
    for (auto& ctx : cameras) {
        ctx->running.store(false);
    }

    // Wait for all threads to finish
    for (auto& ctx : cameras) {
        if (ctx->worker_thread.joinable()) {
            ctx->worker_thread.join();
        }
    }

    LOG_INFO("Dashboard shutdown complete");
    return 0;
}
