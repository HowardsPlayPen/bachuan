#include "client/connection.h"
#include "client/auth.h"
#include "client/stream.h"
#include "video/decoder.h"
#include "video/dashboard_display.h"
#include "rtsp/rtsp_source.h"
#include "mjpeg/mjpeg_source.h"
#include "control/command_server.h"
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
              << "  -H, --hidden          Start with window hidden (headless mode)\n"
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
              << "Configuration file format (MJPEG camera):\n"
              << "  {\n"
              << "    \"cameras\": [\n"
              << "      {\n"
              << "        \"name\": \"Garage\",\n"
              << "        \"type\": \"mjpeg\",\n"
              << "        \"url\": \"http://admin:password@192.168.1.102/mjpeg\"\n"
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
    // MJPEG-specific
    std::unique_ptr<MjpegSource> mjpeg_source;
    // Shared
    std::unique_ptr<VideoDecoder> decoder;
    std::thread worker_thread;
    std::atomic<bool> running{false};
    std::atomic<bool> paused{false};   // When true, worker disconnects and waits
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

    // Wait until quit or pause requested
    while (ctx->running.load() && !g_quit.load() && !ctx->paused.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    ctx->running.store(false);
    ctx->rtsp_source->stop();
    ctx->rtsp_source.reset();
    ctx->decoder.reset();
    LOG_INFO("Camera {} (RTSP): Stopped", ctx->index);
}

// MJPEG camera worker
void mjpeg_camera_worker(CameraContext* ctx, DashboardDisplay* display) {
    LOG_INFO("Camera {} (MJPEG: {}) starting...", ctx->index, ctx->config.name);

    display->set_status(ctx->index, "Connecting MJPEG...");

    // Create MJPEG source
    ctx->mjpeg_source = std::make_unique<MjpegSource>();
    ctx->mjpeg_source->set_url(ctx->config.url);

    if (!ctx->mjpeg_source->connect()) {
        LOG_ERROR("Camera {}: MJPEG connection failed", ctx->index);
        display->set_status(ctx->index, "MJPEG failed");
        return;
    }

    display->set_status(ctx->index, "Starting stream...");

    // Handle stream info
    ctx->mjpeg_source->on_info([ctx](int width, int height, int fps) {
        (void)fps;
        LOG_INFO("Camera {} (MJPEG): Stream {}x{}", ctx->index, width, height);
    });

    // Handle decoded frames directly (MJPEG decodes internally)
    ctx->mjpeg_source->on_frame([ctx, display](const DecodedFrame& decoded) {
        if (!ctx->running.load()) return;
        display->update_frame(ctx->index, decoded);
    });

    // Handle errors
    ctx->mjpeg_source->on_error([ctx, display](const std::string& error) {
        LOG_ERROR("Camera {} (MJPEG): Error: {}", ctx->index, error);
        display->set_status(ctx->index, "Error: " + error);
    });

    // Start streaming
    ctx->running.store(true);
    if (!ctx->mjpeg_source->start()) {
        LOG_ERROR("Camera {}: Failed to start MJPEG stream", ctx->index);
        display->set_status(ctx->index, "Stream failed");
        ctx->running.store(false);
        return;
    }

    // Wait until quit or pause requested
    while (ctx->running.load() && !g_quit.load() && !ctx->paused.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    ctx->running.store(false);
    ctx->mjpeg_source->stop();
    ctx->mjpeg_source.reset();
    LOG_INFO("Camera {} (MJPEG): Stopped", ctx->index);
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

    // Wait until quit or pause requested
    while (ctx->running.load() && !g_quit.load() && !ctx->paused.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    ctx->running.store(false);
    ctx->stream->stop();
    ctx->connection->disconnect();
    ctx->stream.reset();
    ctx->connection.reset();
    ctx->decoder.reset();
    LOG_INFO("Camera {}: Stopped", ctx->index);
}

// Run one connection cycle for the appropriate camera type
void camera_worker_once(CameraContext* ctx, DashboardDisplay* display) {
    if (ctx->config.type == CameraType::Rtsp) {
        rtsp_camera_worker(ctx, display);
    } else if (ctx->config.type == CameraType::Mjpeg) {
        mjpeg_camera_worker(ctx, display);
    } else {
        baichuan_camera_worker(ctx, display);
    }
}

// Top-level camera worker with pause/resume support
void camera_worker(CameraContext* ctx, DashboardDisplay* display) {
    while (!g_quit.load()) {
        // Wait while paused
        if (ctx->paused.load()) {
            display->set_status(ctx->index, "Disconnected");
            while (ctx->paused.load() && !g_quit.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (g_quit.load()) break;
        }

        // Run one connection cycle
        camera_worker_once(ctx, display);

        // If we exited due to pause, loop back to wait for unpause
        if (ctx->paused.load() && !g_quit.load()) {
            continue;
        }

        // Otherwise (quit or permanent stop), exit
        break;
    }
}

int main(int argc, char* argv[]) {
    std::string config_file;
    bool debug = false;
    bool start_hidden = false;

    // Parse command line arguments
    static struct option long_options[] = {
        {"config",  required_argument, nullptr, 'c'},
        {"debug",   no_argument,       nullptr, 'd'},
        {"hidden",  no_argument,       nullptr, 'H'},
        {"help",    no_argument,       nullptr, '?'},
        {nullptr,   0,                 nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:dH", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'c':
                config_file = optarg;
                break;
            case 'd':
                debug = true;
                break;
            case 'H':
                start_hidden = true;
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

    // Hide window if --hidden flag was passed
    if (start_hidden) {
        display.hide_window();
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

    // Set up command server if control config is present
    std::unique_ptr<CommandServer> cmd_server;
    if (!config.control.unix_path.empty() || config.control.tcp_port > 0) {
        cmd_server = std::make_unique<CommandServer>(config.control.unix_path,
                                                      config.control.tcp_port);

        // Helper: parse an int or array of ints from JSON value after a key
        auto parse_indices = [](const std::string& json, const std::string& key) -> std::vector<size_t> {
            std::vector<size_t> indices;
            std::string search = "\"" + key + "\"";
            size_t pos = json.find(search);
            if (pos == std::string::npos) return indices;

            size_t colon = json.find(':', pos);
            if (colon == std::string::npos) return indices;

            size_t val_start = colon + 1;
            while (val_start < json.size() && json[val_start] == ' ') val_start++;

            if (json[val_start] == '[') {
                size_t arr_end = JsonConfigParser::find_matching_bracket_pub(json, val_start);
                if (arr_end == std::string::npos) return indices;
                std::string arr_str = json.substr(val_start + 1, arr_end - val_start - 1);
                std::string num;
                for (char c : arr_str) {
                    if (c >= '0' && c <= '9') {
                        num += c;
                    } else if (c == ',' || c == ' ') {
                        if (!num.empty()) {
                            indices.push_back(static_cast<size_t>(std::stoi(num)));
                            num.clear();
                        }
                    }
                }
                if (!num.empty()) {
                    indices.push_back(static_cast<size_t>(std::stoi(num)));
                }
            } else if (json[val_start] >= '0' && json[val_start] <= '9') {
                std::string num;
                for (size_t i = val_start; i < json.size() && json[i] >= '0' && json[i] <= '9'; i++) {
                    num += json[i];
                }
                indices.push_back(static_cast<size_t>(std::stoi(num)));
            }
            return indices;
        };

        cmd_server->set_handler([&display, &cameras, parse_indices](const std::string& cmd_json) -> std::string {
            size_t pane_total = display.pane_count();

            // --- show: show specific panes, optionally disconnect hidden ones ---
            if (cmd_json.find("\"show\"") != std::string::npos) {
                auto indices = parse_indices(cmd_json, "show");
                if (indices.empty()) return "{\"error\": \"invalid show value\"}";

                for (size_t idx : indices) {
                    if (idx >= pane_total) {
                        return "{\"error\": \"index " + std::to_string(idx) + " out of range\"}";
                    }
                }

                display.show_only(indices);

                // If "disconnect": true, pause hidden cameras and unpause shown ones
                bool disconnect = JsonConfigParser::get_bool(cmd_json, "disconnect");
                if (disconnect) {
                    for (auto& ctx : cameras) {
                        bool is_shown = false;
                        for (size_t idx : indices) {
                            if (ctx->index == idx) { is_shown = true; break; }
                        }
                        if (is_shown) {
                            ctx->paused.store(false);
                        } else {
                            ctx->paused.store(true);
                        }
                    }
                }

                return "{\"ok\": true}";
            }

            // --- show_all: show all panes, reconnect any disconnected ---
            if (cmd_json.find("\"show_all\"") != std::string::npos) {
                display.show_all_panes();
                // Unpause all cameras so they reconnect
                for (auto& ctx : cameras) {
                    ctx->paused.store(false);
                }
                return "{\"ok\": true}";
            }

            // --- disconnect: pause specific cameras (panes stay visible) ---
            if (cmd_json.find("\"disconnect\"") != std::string::npos) {
                auto indices = parse_indices(cmd_json, "disconnect");
                if (indices.empty()) {
                    // disconnect all if value is true
                    if (JsonConfigParser::get_bool(cmd_json, "disconnect")) {
                        for (auto& ctx : cameras) {
                            ctx->paused.store(true);
                        }
                        return "{\"ok\": true}";
                    }
                    return "{\"error\": \"invalid disconnect value\"}";
                }
                for (size_t idx : indices) {
                    if (idx >= pane_total) {
                        return "{\"error\": \"index " + std::to_string(idx) + " out of range\"}";
                    }
                }
                for (auto& ctx : cameras) {
                    for (size_t idx : indices) {
                        if (ctx->index == idx) {
                            ctx->paused.store(true);
                            display.set_status(ctx->index, "Disconnected");
                            break;
                        }
                    }
                }
                return "{\"ok\": true}";
            }

            // --- connect: resume specific cameras ---
            if (cmd_json.find("\"connect\"") != std::string::npos) {
                auto indices = parse_indices(cmd_json, "connect");
                if (indices.empty()) {
                    // connect all if value is true
                    if (JsonConfigParser::get_bool(cmd_json, "connect")) {
                        for (auto& ctx : cameras) {
                            ctx->paused.store(false);
                        }
                        return "{\"ok\": true}";
                    }
                    return "{\"error\": \"invalid connect value\"}";
                }
                for (size_t idx : indices) {
                    if (idx >= pane_total) {
                        return "{\"error\": \"index " + std::to_string(idx) + " out of range\"}";
                    }
                }
                for (auto& ctx : cameras) {
                    for (size_t idx : indices) {
                        if (ctx->index == idx) {
                            ctx->paused.store(false);
                            break;
                        }
                    }
                }
                return "{\"ok\": true}";
            }

            // --- hide_ui: hide the window ---
            if (cmd_json.find("\"hide_ui\"") != std::string::npos) {
                display.hide_window();
                return "{\"ok\": true}";
            }

            // --- show_ui: show the window ---
            if (cmd_json.find("\"show_ui\"") != std::string::npos) {
                display.show_window();
                return "{\"ok\": true}";
            }

            // --- fullscreen: toggle fullscreen mode ---
            if (cmd_json.find("\"fullscreen\"") != std::string::npos) {
                bool fs = JsonConfigParser::get_bool(cmd_json, "fullscreen");
                display.set_fullscreen(fs);
                return "{\"ok\": true}";
            }

            // --- add: add a new camera ---
            size_t add_pos = cmd_json.find("\"add\"");
            if (add_pos != std::string::npos) {
                size_t colon = cmd_json.find(':', add_pos);
                if (colon == std::string::npos) return "{\"error\": \"invalid add command\"}";

                size_t obj_start = cmd_json.find('{', colon);
                if (obj_start == std::string::npos) return "{\"error\": \"missing camera object\"}";

                size_t obj_end = JsonConfigParser::find_matching_brace_pub(cmd_json, obj_start);
                if (obj_end == std::string::npos) return "{\"error\": \"invalid camera object\"}";

                std::string cam_json = cmd_json.substr(obj_start, obj_end - obj_start + 1);

                CameraConfig cam_config;
                try {
                    cam_config = JsonConfigParser::parse_camera(cam_json);
                } catch (const std::exception& e) {
                    return std::string("{\"error\": \"") + e.what() + "\"}";
                }

                bool replace = JsonConfigParser::get_bool(cam_json, "replace");
                size_t new_index = display.add_pane(cam_config, replace);

                auto ctx = std::make_unique<CameraContext>();
                ctx->index = new_index;
                ctx->config = cam_config;

                CameraContext* ctx_ptr = ctx.get();
                ctx->worker_thread = std::thread(camera_worker, ctx_ptr, &display);
                cameras.push_back(std::move(ctx));

                return "{\"ok\": true, \"index\": " + std::to_string(new_index) + "}";
            }

            // --- list: return feed info ---
            if (cmd_json.find("\"list\"") != std::string::npos) {
                // Build connected flags from camera contexts
                std::vector<bool> connected_flags(pane_total, false);
                for (auto& ctx : cameras) {
                    if (ctx->index < pane_total && !ctx->paused.load()) {
                        connected_flags[ctx->index] = true;
                    }
                }
                auto panes = display.get_pane_info(connected_flags);
                std::string result = "{\"ok\": true, \"feeds\": [";
                for (size_t i = 0; i < panes.size(); i++) {
                    if (i > 0) result += ", ";
                    result += "{\"index\": " + std::to_string(i) +
                              ", \"name\": \"" + panes[i].name + "\"" +
                              ", \"visible\": " + (panes[i].visible ? "true" : "false") +
                              ", \"connected\": " + (panes[i].connected ? "true" : "false") + "}";
                }
                result += "]}";
                return result;
            }

            return "{\"error\": \"unknown command\"}";
        });

        if (!cmd_server->start()) {
            LOG_ERROR("Failed to start command server");
        } else {
            LOG_INFO("Command server started");
        }
    }

    // Handle quit
    display.on_quit([&cameras, &cmd_server]() {
        g_quit.store(true);
        for (auto& ctx : cameras) {
            ctx->running.store(false);
        }
        if (cmd_server) cmd_server->stop();
    });

    // Run GTK main loop
    display.run();

    // Stop command server
    if (cmd_server) {
        cmd_server->stop();
    }

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
