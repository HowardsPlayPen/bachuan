#pragma once

#include "video/decoder.h"
#include "utils/json_config.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>

#include <gtk/gtk.h>
#include <cairo/cairo.h>

namespace baichuan {

// Callback for quit event
using QuitCallback = std::function<void()>;

// Callback for the "toggle resolution" hotkey.
// Argument is the currently focused pane index, or -1 when in overview
// (meaning: apply to all cameras).
using ToggleResolutionCallback = std::function<void(int focused_index)>;

// One row in the per-camera stream-info panel (opened with 'i' on a focused pane).
struct StreamEntry {
    std::string label;    // e.g. "main (configured)", "/Streaming/Channels/101"
    std::string url;      // full stream URL
    std::string summary;  // e.g. "hevc 1920x1080 @15fps + aac", or an error note
};

// Characterize a pane's *configured* streams (main/sub). Runs on a worker thread.
using ProbeStreamsCallback = std::function<std::vector<StreamEntry>(int pane_index)>;
// Actively *discover* additional streams for a pane's camera (probes common
// RTSP paths). Runs on a worker thread.
using DiscoverStreamsCallback = std::function<std::vector<StreamEntry>(int pane_index)>;
// Discover streams via ONVIF for a pane's camera. Runs on a worker thread.
// Returns entries; on failure a single entry whose summary carries the reason.
using OnvifStreamsCallback = std::function<std::vector<StreamEntry>(int pane_index)>;

// Single camera pane within the dashboard
struct CameraPane {
    std::string name;
    std::string status;
    CameraType type = CameraType::Baichuan;  // source type (gates the Streams panel actions)

    // Video frame data
    std::vector<uint8_t> frame_buffer;
    int frame_width = 0;
    int frame_height = 0;
    cairo_surface_t* surface = nullptr;
    std::mutex mutex;
    std::atomic<bool> has_video{false};
    std::atomic<bool> frame_pending{false};

    // GTK widgets
    GtkWidget* frame_widget = nullptr;  // GtkFrame container
    GtkWidget* drawing_area = nullptr;

    ~CameraPane() {
        if (surface) {
            cairo_surface_destroy(surface);
        }
    }
};

class DashboardDisplay {
public:
    DashboardDisplay();
    ~DashboardDisplay();

    // Initialize GTK (call once from main thread)
    static bool init_gtk(int* argc, char*** argv);

    // Create dashboard window with specified number of panes
    bool create(const std::string& title, const std::vector<CameraConfig>& cameras, int columns = 2);

    // Update a specific camera pane with decoded frame
    void update_frame(size_t pane_index, const DecodedFrame& frame);

    // Set status message for a specific pane
    void set_status(size_t pane_index, const std::string& status);

    // Run GTK main loop (blocking, call from main thread)
    void run();

    // Request quit (can be called from any thread)
    void quit();

    // Check if window is still open
    bool is_open() const { return window_ != nullptr && !quit_requested_.load(); }

    // Set quit callback
    void on_quit(QuitCallback cb) { quit_callback_ = std::move(cb); }

    // Set the text shown by the in-app Help dialog (typically the --help output).
    void set_help_text(const std::string& text) { help_text_ = text; }

    // Configure the modifier key required for keyboard hotkeys.
    // Accepts "", "none", "ctrl"/"control", "alt", "shift", "super"/"win".
    // Unknown values fall back to no modifier.
    void set_hotkey_modifier(const std::string& modifier);

    // Set the callback invoked when the resolution-toggle hotkey is pressed.
    void on_toggle_resolution(ToggleResolutionCallback cb) { resolution_callback_ = std::move(cb); }

    // Set the callbacks backing the per-camera stream-info panel.
    void on_probe_streams(ProbeStreamsCallback cb) { probe_streams_callback_ = std::move(cb); }
    void on_discover_streams(DiscoverStreamsCallback cb) { discover_streams_callback_ = std::move(cb); }
    void on_onvif_streams(OnvifStreamsCallback cb) { onvif_streams_callback_ = std::move(cb); }

    // Focus a single camera pane (hides the others). index must be < pane_count().
    void focus_pane(size_t index);

    // Return to the overview showing all panes.
    void show_overview();

    // Get number of panes
    size_t pane_count() const { return panes_.size(); }

    // Show only the specified pane indices (hides all others)
    void show_only(const std::vector<size_t>& indices);

    // Show all panes
    void show_all_panes();

    // Add a new pane dynamically, returns the new pane index
    // If replace=true, hides all existing panes
    size_t add_pane(const CameraConfig& config, bool replace = false);

    // Hide the entire window (headless mode)
    void hide_window();

    // Show the window
    void show_window();

    // Set fullscreen mode
    void set_fullscreen(bool fullscreen);

    // Pane info for the "list" command
    struct PaneInfo {
        std::string name;
        bool visible;
        bool connected;  // whether the camera worker is active
    };

    // Get current pane info (names + visibility + connection state)
    // connected_flags: one bool per pane, true if camera is connected
    std::vector<PaneInfo> get_pane_info(const std::vector<bool>& connected_flags = {});

private:
    GtkWidget* window_ = nullptr;
    GtkWidget* main_box_ = nullptr;      // Horizontal box for menu + grid
    GtkWidget* menu_box_ = nullptr;      // Left menu panel
    GtkWidget* grid_ = nullptr;          // Grid for camera panes

    std::vector<std::unique_ptr<CameraPane>> panes_;
    int columns_ = 2;

    std::atomic<bool> quit_requested_{false};
    QuitCallback quit_callback_;
    std::string help_text_;   // shown by the Help dialog

    // Keyboard hotkey state
    guint hotkey_modifier_mask_ = 0;   // required modifier mask (0 = none)
    int focused_pane_ = -1;            // currently focused pane, -1 = overview
    ToggleResolutionCallback resolution_callback_;
    ProbeStreamsCallback probe_streams_callback_;
    DiscoverStreamsCallback discover_streams_callback_;
    OnvifStreamsCallback onvif_streams_callback_;

    // Open the stream-info panel for a focused pane (triggered by the 'i' key).
    void show_stream_info(int pane_index);

    // GTK callbacks
    static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_delete_event(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static void on_quit_clicked(GtkWidget* widget, gpointer user_data);
    static void on_help_clicked(GtkWidget* widget, gpointer user_data);
    static gboolean on_idle_update(gpointer user_data);
    static gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data);

    void update_pane_surface(CameraPane* pane);
    static void draw_pane(CameraPane* pane, cairo_t* cr, int width, int height);
};

} // namespace baichuan
