#pragma once

#include "video/decoder.h"
#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>

#include <gtk/gtk.h>
#include <cairo/cairo.h>

namespace bachuan {

// Callback for window close event
using CloseCallback = std::function<void()>;

class VideoDisplay {
public:
    VideoDisplay();
    ~VideoDisplay();

    // Initialize GTK (call once from main thread)
    static bool init_gtk(int* argc, char*** argv);

    // Create display window
    bool create(const std::string& title, int width = 1280, int height = 720);

    // Update display with decoded frame
    void update_frame(const DecodedFrame& frame);

    // Run GTK main loop (blocking, call from main thread)
    void run();

    // Request quit (can be called from any thread)
    void quit();

    // Check if window is still open
    bool is_open() const { return window_ != nullptr && !quit_requested_.load(); }

    // Set close callback
    void on_close(CloseCallback cb) { close_callback_ = std::move(cb); }

    // Set status message (shown before video starts)
    void set_status(const std::string& status);

    // Get window dimensions
    int width() const { return width_; }
    int height() const { return height_; }

private:
    GtkWidget* window_ = nullptr;
    GtkWidget* main_box_ = nullptr;      // Horizontal box for menu + video
    GtkWidget* menu_box_ = nullptr;      // Left menu panel
    GtkWidget* drawing_area_ = nullptr;
    cairo_surface_t* surface_ = nullptr;

    int width_ = 0;
    int height_ = 0;
    int frame_width_ = 0;
    int frame_height_ = 0;

    std::atomic<bool> quit_requested_{false};
    CloseCallback close_callback_;

    // Frame buffer protected by mutex
    std::vector<uint8_t> frame_buffer_;
    std::mutex frame_mutex_;
    std::atomic<bool> frame_pending_{false};

    // Status message (shown before video starts)
    std::string status_message_;
    std::atomic<bool> has_video_{false};

    // GTK callbacks (static, with user_data pointer)
    static gboolean on_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data);
    static gboolean on_delete_event(GtkWidget* widget, GdkEvent* event, gpointer user_data);
    static gboolean on_idle_update(gpointer user_data);
    static void on_quit_clicked(GtkWidget* widget, gpointer user_data);

    void update_surface();
};

} // namespace bachuan
