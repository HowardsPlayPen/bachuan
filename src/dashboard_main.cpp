#include "client/connection.h"
#include "client/auth.h"
#include "client/stream.h"
#include "video/decoder.h"
#include "video/dashboard_display.h"
#include "rtsp/rtsp_source.h"
#include <sstream>
#include "mjpeg/mjpeg_source.h"
#include "control/command_server.h"
#include "utils/logger.h"
#include "utils/json_config.h"
#include "utils/net_compat.h"
#include "utils/onvif.h"

#include <iostream>
#include <string>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>
#include <thread>
#include <csignal>
#include <cstdlib>
#include <getopt.h>

using namespace baichuan;

// Documentation URL shown in --help. Set at compile time via CMake (-DHELP_URL=...);
// this fallback keeps the source self-contained if built without the definition.
#ifndef HELP_URL
#define HELP_URL "https://github.com/HowardsPlayPen/bachuan"
#endif

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
}

// ---------------------------------------------------------------------------
// Stream introspection & discovery (the in-app "Streams" panel).
//
// introspect_stream() opens an RTSP/HTTP URL with libavformat and returns a
// one-line human summary of what it carries (codec / resolution / fps / audio).
// It is a blocking network call and MUST be run off the GTK main thread.
// ---------------------------------------------------------------------------
static std::string introspect_stream(const std::string& url,
                                     const std::string& transport,
                                     int timeout_seconds = 4) {
    AVFormatContext* fmt = avformat_alloc_context();
    if (!fmt) return "probe error";

    AVDictionary* opts = nullptr;
    if (url.rfind("rtsp://", 0) == 0) {
        av_dict_set(&opts, "rtsp_transport", transport.c_str(), 0);
    }
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "%d", timeout_seconds * 1000000);
    av_dict_set(&opts, "timeout", tbuf, 0);
    av_dict_set(&opts, "stimeout", tbuf, 0);

    int ret = avformat_open_input(&fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char err[128];
        av_strerror(ret, err, sizeof(err));
        // Normalise the two cases the UI cares about most.
        std::string e = err;
        if (e.find("401") != std::string::npos || e.find("Unauthorized") != std::string::npos)
            return "unauthorized (bad credentials)";
        if (e.find("404") != std::string::npos || e.find("Not Found") != std::string::npos)
            return "not found (wrong path)";
        return std::string("unreachable: ") + err;
    }

    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return "opened, but no stream info";
    }

    std::string vsum, asum;
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        AVCodecParameters* cp = fmt->streams[i]->codecpar;
        const char* name = avcodec_get_name(cp->codec_id);
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO && vsum.empty()) {
            int fps = 0;
            AVRational fr = fmt->streams[i]->avg_frame_rate;
            if (fr.den <= 0 || fr.num <= 0) fr = fmt->streams[i]->r_frame_rate;
            if (fr.den > 0) fps = fr.num / fr.den;
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %dx%d @%dfps",
                     name, cp->width, cp->height, fps);
            vsum = buf;
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO && asum.empty()) {
            asum = std::string(" + ") + name;
        }
    }
    avformat_close_input(&fmt);
    if (vsum.empty()) return "no video stream";
    return vsum + asum;
}

// Split an rtsp:// URL into its base (scheme + optional creds + host + port)
// and path, so discovery can swap the path while keeping credentials.
// Returns false if url is not rtsp://.
static bool split_rtsp_url(const std::string& url, std::string& base_out) {
    if (url.rfind("rtsp://", 0) != 0) return false;
    size_t host_start = 7;  // after "rtsp://"
    size_t slash = url.find('/', host_start);
    base_out = (slash == std::string::npos) ? url : url.substr(0, slash);
    return true;
}

// Parse rtsp://[user:pass@]host[:port]/... into its parts. Returns false if not rtsp.
static bool parse_rtsp_url(const std::string& url, std::string& user,
                           std::string& pass, std::string& host, int& port) {
    if (url.rfind("rtsp://", 0) != 0) return false;
    std::string rest = url.substr(7);
    size_t slash = rest.find('/');
    std::string authority = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    size_t at = authority.find('@');
    std::string hostport = authority;
    user.clear(); pass.clear();
    if (at != std::string::npos) {
        std::string creds = authority.substr(0, at);
        hostport = authority.substr(at + 1);
        size_t colon = creds.find(':');
        user = (colon == std::string::npos) ? creds : creds.substr(0, colon);
        pass = (colon == std::string::npos) ? ""    : creds.substr(colon + 1);
    }
    size_t hc = hostport.find(':');
    host = (hc == std::string::npos) ? hostport : hostport.substr(0, hc);
    port = (hc == std::string::npos) ? 554 : std::atoi(hostport.c_str() + hc + 1);
    return !host.empty();
}

// Common vendor RTSP stream paths, ordered most-likely-first. Used by the
// on-demand "Discover" action. Intentionally short to limit request volume
// (some cameras rate-limit / lock out aggressive probing).
static const std::vector<std::string>& discovery_paths() {
    static const std::vector<std::string> paths = {
        "/Streaming/Channels/101",           // Hikvision/EZVIZ main
        "/Streaming/Channels/102",           // Hikvision/EZVIZ sub
        "/h264/ch1/main/av_stream",          // EZVIZ alias (may be HEVC)
        "/h264/ch1/sub/av_stream",
        "/cam/realmonitor?channel=1&subtype=0",  // Dahua main
        "/cam/realmonitor?channel=1&subtype=1",  // Dahua sub
        "/live/ch0",                         // generic
        "/live/ch1",
        "/stream1",
        "/stream2",
        "/h264Preview_01_main",              // Reolink main
        "/h264Preview_01_sub",               // Reolink sub
    };
    return paths;
}

// Global flag for signal handling
static std::atomic<bool> g_quit{false};

void signal_handler(int signum) {
    (void)signum;
    LOG_INFO("Received signal, shutting down...");
    g_quit.store(true);
}

// Default config path.
//   Windows: %APPDATA%\baichuan\config.json
//   POSIX:   $XDG_CONFIG_HOME/baichuan/config.json, falling back to
//            $HOME/.config/baichuan/config.json (XDG Base Directory spec).
std::string default_config_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) {
        return std::string(appdata) + "\\baichuan\\config.json";
    }
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile && *userprofile) {
        return std::string(userprofile) + "\\.config\\baichuan\\config.json";
    }
    return {};
#else
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        return std::string(xdg) + "/baichuan/config.json";
    }
    const char* home = std::getenv("HOME");
    if (home && *home) {
        return std::string(home) + "/.config/baichuan/config.json";
    }
    return {};
#endif
}

// Build the full help/usage text. Shared by --help (stdout) and the in-app Help
// dialog so the two never drift.
std::string usage_text(const char* program) {
    std::ostringstream o;
    o << "Usage: " << program << " [-c <config.json>] [options]\n"
      << "\n"
      << "Options:\n"
      << "  -c, --config <file>   JSON configuration file\n"
      << "                        (default: $XDG_CONFIG_HOME/baichuan/config.json,\n"
      << "                         or ~/.config/baichuan/config.json)\n"
      << "  -d, --debug           Enable debug logging\n"
      << "  -H, --hidden          Start with window hidden (headless mode)\n"
      << "  --help                Show this help message\n"
      << "\n"
      << "Keyboard controls (when the dashboard window has focus):\n"
      << "  1-9                   Focus a single camera (1 = first camera)\n"
      << "  0                     Return to the overview (show all cameras)\n"
      << "  i                     Show the stream-info panel for the focused camera\n"
      << "                        (codec/resolution/fps; for RTSP cameras a Discover\n"
      << "                        button probes common paths and an ONVIF button queries\n"
      << "                        the camera's advertised streams; baichuan cameras list\n"
      << "                        their available main/sub/extern feeds)\n"
      << "  r                     Toggle low/high resolution (sub/main stream)\n"
      << "                        of the focused camera, or all in overview.\n"
      << "                        Baichuan cameras switch the protocol stream; RTSP\n"
      << "                        cameras switch between their \"url\" and \"sub\" URLs.\n"
      << "  An optional modifier can be required via \"hotkey_modifier\" in the\n"
      << "  config (e.g. \"ctrl\" makes the shortcuts CTRL+1 .. CTRL+0 / CTRL+R).\n"
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
      << "        \"url\": \"rtsp://admin:password@192.168.1.101:554/main\",\n"
      << "        \"sub\": \"rtsp://admin:password@192.168.1.101:554/sub\",\n"
      << "        \"transport\": \"tcp\"\n"
      << "      }\n"
      << "    ]\n"
      << "  }\n"
      << "  (\"sub\" is optional; when present, the overview matrix shows the sub\n"
      << "   (low-res) feed by default and the 'r' key toggles it with the main \"url\".)\n"
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
      << "  " << program << " -c cameras.json\n"
      << "\n"
      << "For more help and documentation, see:\n"
      << "  " << HELP_URL << "\n";
    return o.str();
}

void print_usage(const char* program) {
    std::cerr << usage_text(program);
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
    // Stream switching: protected by mutex since std::string isn't atomic
    std::mutex stream_mutex;
    std::string pending_stream;  // Empty means no change requested

    // Last-known live resolution of the *active* stream (0 until first info).
    // Populated by the RTSP/baichuan info callbacks; read by the stream-info panel.
    std::atomic<int> live_width{0};
    std::atomic<int> live_height{0};
    std::atomic<int> live_fps{0};
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

    // Apply any pending stream switch (the 'r' hotkey) before connecting.
    {
        std::lock_guard<std::mutex> lock(ctx->stream_mutex);
        if (!ctx->pending_stream.empty()) {
            ctx->config.stream = ctx->pending_stream;
            ctx->pending_stream.clear();
        }
    }

    // Select the URL for the active stream: "sub" uses url_sub when configured,
    // everything else uses the primary url.
    const bool want_sub = (ctx->config.stream == "sub") && !ctx->config.url_sub.empty();
    const std::string& active_url = want_sub ? ctx->config.url_sub : ctx->config.url;
    LOG_INFO("Camera {} (RTSP): using {} stream", ctx->index, want_sub ? "sub" : "main");

    // Create RTSP source
    ctx->rtsp_source = std::make_unique<RtspSource>();
    ctx->rtsp_source->set_url(active_url);
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
        ctx->live_width.store(width);
        ctx->live_height.store(height);
        ctx->live_fps.store(fps);
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

    // Wait until quit, pause, or a stream switch is requested.
    while (ctx->running.load() && !g_quit.load() && !ctx->paused.load()) {
        {
            std::lock_guard<std::mutex> lock(ctx->stream_mutex);
            if (!ctx->pending_stream.empty()) break;  // Stream switch requested
        }
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

    // Apply any pending stream switch before configuring
    {
        std::lock_guard<std::mutex> lock(ctx->stream_mutex);
        if (!ctx->pending_stream.empty()) {
            ctx->config.stream = ctx->pending_stream;
            ctx->pending_stream.clear();
        }
    }

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
        ctx->live_width.store(static_cast<int>(info.video_width));
        ctx->live_height.store(static_cast<int>(info.video_height));
        ctx->live_fps.store(static_cast<int>(info.fps));
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
            // The decoded frame carries the true resolution; the protocol's
            // BcMediaInfo often reports 0, so trust the frame for the stream-info panel.
            if (decoded.width > 0 && decoded.height > 0) {
                ctx->live_width.store(decoded.width);
                ctx->live_height.store(decoded.height);
            }
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

    // Wait until quit, pause, stream switch, or connection lost
    while (ctx->running.load() && !g_quit.load() && !ctx->paused.load()) {
        {
            std::lock_guard<std::mutex> lock(ctx->stream_mutex);
            if (!ctx->pending_stream.empty()) break;  // Stream switch requested
        }
        // Detect dead connection (e.g. after system freeze/resume)
        if (!ctx->stream->is_streaming() || !ctx->connection->is_connected()) {
            LOG_WARN("Camera {}: Connection lost, will reconnect", ctx->index);
            break;
        }
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

        // If quitting, exit
        if (g_quit.load()) break;

        // If paused, loop back to wait for unpause
        if (ctx->paused.load()) continue;

        // If stream switch is pending, reconnect immediately
        {
            std::lock_guard<std::mutex> lock(ctx->stream_mutex);
            if (!ctx->pending_stream.empty()) {
                display->set_status(ctx->index, "Switching stream...");
                continue;
            }
        }

        // Stream dropped — reconnect after a delay
        display->set_status(ctx->index, "Reconnecting...");
        for (int i = 0; i < 50 && !g_quit.load() && !ctx->paused.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

int main(int argc, char* argv[]) {
    // Initialize the socket subsystem (WSAStartup on Windows; no-op on POSIX).
    net::Init net_guard;
    if (!net_guard.ok) {
        std::cerr << "Error: failed to initialize networking\n";
        return 1;
    }

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
        config_file = default_config_path();
        if (config_file.empty()) {
            std::cerr << "Error: No config file specified and no default location available\n\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // Configure logging
    if (debug) {
        Logger::instance().set_level(LogLevel::Debug);
    }

    LOG_INFO("Baichuan Dashboard");

    // Parse configuration
    LOG_INFO("Loading config: {}", config_file);
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

    // Feed the same text as --help into the in-app Help dialog.
    display.set_help_text(usage_text(argv[0]));

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

            // --- stream: switch stream type for baichuan cameras ---
            if (cmd_json.find("\"stream\"") != std::string::npos) {
                std::string new_stream = JsonConfigParser::get_string(cmd_json, "stream", "");
                if (new_stream != "main" && new_stream != "sub" && new_stream != "extern") {
                    return "{\"error\": \"stream must be \\\"main\\\", \\\"sub\\\", or \\\"extern\\\"\"}";
                }

                // Determine which cameras to switch
                auto indices = parse_indices(cmd_json, "index");
                std::vector<CameraContext*> targets;

                if (indices.empty()) {
                    // No index specified — apply to all baichuan cameras
                    for (auto& ctx : cameras) {
                        if (ctx->config.type == CameraType::Baichuan) {
                            targets.push_back(ctx.get());
                        }
                    }
                } else {
                    for (size_t idx : indices) {
                        if (idx >= cameras.size()) {
                            return "{\"error\": \"index " + std::to_string(idx) + " out of range\"}";
                        }
                        if (cameras[idx]->config.type != CameraType::Baichuan) {
                            return "{\"error\": \"camera " + std::to_string(idx) + " is not baichuan\"}";
                        }
                        targets.push_back(cameras[idx].get());
                    }
                }

                for (auto* ctx : targets) {
                    {
                        std::lock_guard<std::mutex> lock(ctx->stream_mutex);
                        if (ctx->config.stream == new_stream && ctx->pending_stream.empty()) {
                            continue;  // Already on this stream
                        }
                        ctx->pending_stream = new_stream;
                    }
                    // Kick the worker out of its wait loop
                    ctx->running.store(false);
                    LOG_INFO("Camera {}: Switching to {} stream", ctx->index, new_stream);
                }

                return "{\"ok\": true}";
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
                              ", \"connected\": " + (panes[i].connected ? "true" : "false");
                    // Include stream type for baichuan cameras
                    if (i < cameras.size() && cameras[i]->config.type == CameraType::Baichuan) {
                        result += ", \"stream\": \"" + cameras[i]->config.stream + "\"";
                    }
                    result += "}";
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

    // Configure keyboard hotkeys: optional modifier + resolution toggle
    display.set_hotkey_modifier(config.hotkey_modifier);
    display.on_toggle_resolution([&cameras](int focused_index) {
        // Toggle each target camera between its low (sub) and high (main) resolution
        // stream. focused_index == -1 means "all cameras". Baichuan cameras switch the
        // protocol stream; RTSP cameras switch between their "url" and "sub" URLs, and
        // are only eligible when a sub URL is configured.
        for (auto& ctx : cameras) {
            const bool is_baichuan = ctx->config.type == CameraType::Baichuan;
            const bool is_rtsp_dual =
                ctx->config.type == CameraType::Rtsp && !ctx->config.url_sub.empty();
            if (!is_baichuan && !is_rtsp_dual) continue;
            if (focused_index >= 0 && ctx->index != static_cast<size_t>(focused_index)) continue;

            std::string target;
            {
                std::lock_guard<std::mutex> lock(ctx->stream_mutex);
                const std::string& current =
                    ctx->pending_stream.empty() ? ctx->config.stream : ctx->pending_stream;
                target = (current == "main") ? "sub" : "main";
                ctx->pending_stream = target;
            }
            // Kick the worker out of its wait loop so it reconnects on the new stream
            ctx->running.store(false);
            LOG_INFO("Camera {}: Toggling resolution to {} stream", ctx->index, target);
        }
    });

    // Stream-info panel ('i' on a focused pane): introspect configured streams (B)...
    display.on_probe_streams([&cameras](int idx) -> std::vector<StreamEntry> {
        std::vector<StreamEntry> out;
        if (idx < 0 || static_cast<size_t>(idx) >= cameras.size()) return out;
        const auto& cfg = cameras[idx]->config;
        if (cfg.type == CameraType::Rtsp) {
            out.push_back({"main (configured)", cfg.url,
                           introspect_stream(cfg.url, cfg.transport)});
            if (!cfg.url_sub.empty()) {
                out.push_back({"sub (configured)", cfg.url_sub,
                               introspect_stream(cfg.url_sub, cfg.transport)});
            }
        } else if (cfg.type == CameraType::Mjpeg) {
            out.push_back({"stream (configured)", cfg.url,
                           introspect_stream(cfg.url, "tcp")});
        } else {  // Baichuan: list the available protocol feeds (main/sub/extern),
                  // marking the active one and showing its live resolution.
            int w = cameras[idx]->live_width.load();
            int h = cameras[idx]->live_height.load();
            int f = cameras[idx]->live_fps.load();
            char live[64] = "connecting... (resolution not yet available)";
            if (w > 0 && h > 0) {
                if (f > 0) snprintf(live, sizeof(live), "%dx%d @%dfps", w, h, f);
                else       snprintf(live, sizeof(live), "%dx%d", w, h);
            }
            const char* feeds[] = {"main", "sub", "extern"};
            for (const char* feed : feeds) {
                bool active = (cfg.stream == feed);
                std::string label = std::string(feed) + " stream" +
                                    (active ? "  [active]" : "");
                std::string endpoint = cfg.host + ":" + std::to_string(cfg.port) +
                                       " (channel " + std::to_string(cfg.channel) + ", " + feed + ")";
                std::string summary = active ? std::string(live)
                                             : "select with 'r' to view resolution";
                out.push_back({label, endpoint, summary});
            }
        }
        return out;
    });

    // ...and on-demand discovery of common RTSP paths (C).
    display.on_discover_streams([&cameras](int idx) -> std::vector<StreamEntry> {
        std::vector<StreamEntry> out;
        if (idx < 0 || static_cast<size_t>(idx) >= cameras.size()) return out;
        const auto& cfg = cameras[idx]->config;
        std::string base;
        if (cfg.type != CameraType::Rtsp || !split_rtsp_url(cfg.url, base)) {
            return out;  // discovery only meaningful for RTSP cameras
        }
        for (const auto& path : discovery_paths()) {
            std::string url = base + path;
            if (url == cfg.url || url == cfg.url_sub) continue;  // already listed as configured
            std::string summary = introspect_stream(url, cfg.transport, 3);
            // Keep only paths that actually resolved to a video stream.
            if (summary.find("fps") != std::string::npos) {
                out.push_back({path, url, summary});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(300));  // gentle: avoid camera lockout
        }
        return out;
    });

    // ...and ONVIF discovery (D): ask the camera what it actually advertises.
    display.on_onvif_streams([&cameras](int idx) -> std::vector<StreamEntry> {
        std::vector<StreamEntry> out;
        if (idx < 0 || static_cast<size_t>(idx) >= cameras.size()) return out;
        const auto& cfg = cameras[idx]->config;
        std::string user, pass, host; int rtsp_port = 554;
        if (cfg.type != CameraType::Rtsp || !parse_rtsp_url(cfg.url, user, pass, host, rtsp_port)) {
            out.push_back({"ONVIF", "", "only available for RTSP cameras"});
            return out;
        }
        // ONVIF HTTP service is conventionally on port 80.
        OnvifResult r = onvif_discover(host, 80, user, pass);
        if (!r.ok) {
            out.push_back({"ONVIF query failed", "", r.error});
            return out;
        }
        for (const auto& s : r.streams) {
            std::string summary;
            if (!s.encoding.empty()) summary += s.encoding + " ";
            summary += s.resolution.empty() ? "(resolution not reported)" : s.resolution;
            out.push_back({s.name, s.uri, summary});
        }
        return out;
    });

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
