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

namespace bachuan {

// Callback for quit event
using QuitCallback = std::function<void()>;

// Single camera pane within the dashboard
struct CameraPane {
    std::string name;
    std::string status;

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

    // Get number of panes
    size_t pane_count() const { return panes_.size(); }

private:
    GtkWidget* window_ = nullptr;
    GtkWidget* main_box_ = nullptr;      // Horizontal box for menu + grid
    GtkWidget* menu_box_ = nullptr;      // Left menu panel
    GtkWidget* grid_ = nullptr;          // Grid for camera panes

    std::vector<std::unique_ptr<CameraPane>> panes_;
    int columns_ = 2;

    std::atomic<bool> quit_requested_{false};
    QuitCallback quit_callback_;

    // GTK callbacks
    static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_delete_event(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static void on_quit_clicked(GtkWidget* widget, gpointer user_data);
    static gboolean on_idle_update(gpointer user_data);

    void update_pane_surface(CameraPane* pane);
    static void draw_pane(CameraPane* pane, cairo_t* cr, int width, int height);
};

} // namespace bachuan
