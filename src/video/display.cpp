#include "video/display.h"
#include "utils/logger.h"

#include <gtk/gtk.h>
#include <cairo/cairo.h>
#include <cstring>

namespace baichuan {

VideoDisplay::VideoDisplay() = default;

VideoDisplay::~VideoDisplay() {
    if (surface_) {
        cairo_surface_destroy(surface_);
        surface_ = nullptr;
    }
}

bool VideoDisplay::init_gtk(int* argc, char*** argv) {
    return gtk_init_check(argc, argv) == TRUE;
}

bool VideoDisplay::create(const std::string& title, int width, int height) {
    width_ = width;
    height_ = height;

    // Create main window
    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (!window_) {
        LOG_ERROR("Failed to create GTK window");
        return false;
    }

    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(window_), width + 100, height);  // Extra width for menu
    gtk_window_set_resizable(GTK_WINDOW(window_), TRUE);

    // Create horizontal box for menu + video area
    main_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), main_box_);

    // Create left menu panel
    menu_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_size_request(menu_box_, 100, -1);
    gtk_box_pack_start(GTK_BOX(main_box_), menu_box_, FALSE, FALSE, 0);

    // Style the menu box with a dark background
    GtkCssProvider* css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider,
        "box { background-color: #2d2d2d; padding: 10px; }"
        "button { margin: 5px; }",
        -1, nullptr);
    GtkStyleContext* context = gtk_widget_get_style_context(menu_box_);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(css_provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_USER);

    // Add Quit button
    GtkWidget* quit_button = gtk_button_new_with_label("Quit");
    gtk_box_pack_end(GTK_BOX(menu_box_), quit_button, FALSE, FALSE, 0);
    g_signal_connect(quit_button, "clicked", G_CALLBACK(on_quit_clicked), this);

    // Create drawing area for video
    drawing_area_ = gtk_drawing_area_new();
    if (!drawing_area_) {
        LOG_ERROR("Failed to create drawing area");
        gtk_widget_destroy(window_);
        window_ = nullptr;
        return false;
    }

    gtk_widget_set_size_request(drawing_area_, width, height);
    gtk_widget_set_hexpand(drawing_area_, TRUE);
    gtk_widget_set_vexpand(drawing_area_, TRUE);
    gtk_box_pack_start(GTK_BOX(main_box_), drawing_area_, TRUE, TRUE, 0);

    // Connect signals
    g_signal_connect(drawing_area_, "draw", G_CALLBACK(on_draw), this);
    g_signal_connect(window_, "delete-event", G_CALLBACK(on_delete_event), this);

    // Show window
    gtk_widget_show_all(window_);

    LOG_INFO("Created display window: {}x{}", width, height);
    return true;
}

void VideoDisplay::update_frame(const DecodedFrame& frame) {
    if (!window_ || quit_requested_.load()) {
        return;
    }

    // Copy frame data to buffer
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        frame_buffer_ = frame.rgb_data;
        frame_width_ = frame.width;
        frame_height_ = frame.height;
        frame_pending_.store(true);
        has_video_.store(true);
    }

    // Schedule redraw from main thread
    g_idle_add(on_idle_update, this);
}

void VideoDisplay::set_status(const std::string& status) {
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        status_message_ = status;
    }

    // Schedule redraw to show status
    if (drawing_area_) {
        g_idle_add([](gpointer data) -> gboolean {
            VideoDisplay* self = static_cast<VideoDisplay*>(data);
            if (self->drawing_area_) {
                gtk_widget_queue_draw(self->drawing_area_);
            }
            return FALSE;
        }, this);
    }
}

void VideoDisplay::run() {
    if (!window_) {
        LOG_ERROR("Window not created");
        return;
    }

    LOG_DEBUG("Starting GTK main loop");
    gtk_main();
    LOG_DEBUG("GTK main loop ended");
}

void VideoDisplay::quit() {
    quit_requested_.store(true);
    gtk_main_quit();
}

gboolean VideoDisplay::on_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    VideoDisplay* self = static_cast<VideoDisplay*>(user_data);

    // Get drawing area size
    int area_width = gtk_widget_get_allocated_width(widget);
    int area_height = gtk_widget_get_allocated_height(widget);

    // Clear background
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    if (self->surface_ && self->frame_width_ > 0 && self->frame_height_ > 0) {
        // Calculate scaling to fit window while maintaining aspect ratio
        double scale_x = static_cast<double>(area_width) / self->frame_width_;
        double scale_y = static_cast<double>(area_height) / self->frame_height_;
        double scale = std::min(scale_x, scale_y);

        // Calculate centered position
        double x_offset = (area_width - self->frame_width_ * scale) / 2.0;
        double y_offset = (area_height - self->frame_height_ * scale) / 2.0;

        // Draw scaled image
        cairo_save(cr);
        cairo_translate(cr, x_offset, y_offset);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, self->surface_, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);
    } else if (!self->has_video_.load()) {
        // Show status message before video starts
        std::string status;
        {
            std::lock_guard<std::mutex> lock(self->frame_mutex_);
            status = self->status_message_;
        }

        if (!status.empty()) {
            // Set up font
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 18.0);

            // Split status into lines
            std::vector<std::string> lines;
            size_t pos = 0;
            size_t prev = 0;
            while ((pos = status.find('\n', prev)) != std::string::npos) {
                lines.push_back(status.substr(prev, pos - prev));
                prev = pos + 1;
            }
            lines.push_back(status.substr(prev));

            // Calculate total height
            cairo_text_extents_t extents;
            cairo_text_extents(cr, "Ay", &extents);
            double line_height = extents.height + 8;
            double total_height = line_height * lines.size();

            // Draw each line centered
            double y = (area_height - total_height) / 2.0 + extents.height;
            for (const auto& line : lines) {
                cairo_text_extents(cr, line.c_str(), &extents);
                double x = (area_width - extents.width) / 2.0;

                // Draw text with white color
                cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
                cairo_move_to(cr, x, y);
                cairo_show_text(cr, line.c_str());

                y += line_height;
            }
        }
    }

    return FALSE;
}

gboolean VideoDisplay::on_delete_event(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
    (void)widget;
    (void)event;

    VideoDisplay* self = static_cast<VideoDisplay*>(user_data);

    LOG_DEBUG("Window close requested");
    self->quit_requested_.store(true);

    if (self->close_callback_) {
        self->close_callback_();
    }

    gtk_main_quit();
    return FALSE;
}

void VideoDisplay::on_quit_clicked(GtkWidget* widget, gpointer user_data) {
    (void)widget;

    VideoDisplay* self = static_cast<VideoDisplay*>(user_data);

    LOG_DEBUG("Quit button clicked");
    self->quit_requested_.store(true);

    if (self->close_callback_) {
        self->close_callback_();
    }

    gtk_main_quit();
}

gboolean VideoDisplay::on_idle_update(gpointer user_data) {
    VideoDisplay* self = static_cast<VideoDisplay*>(user_data);

    if (!self->frame_pending_.load() || !self->drawing_area_) {
        return FALSE;  // Don't call again
    }

    self->update_surface();
    self->frame_pending_.store(false);

    // Request redraw
    gtk_widget_queue_draw(self->drawing_area_);

    return FALSE;  // Don't call again (one-shot)
}

void VideoDisplay::update_surface() {
    std::lock_guard<std::mutex> lock(frame_mutex_);

    if (frame_buffer_.empty() || frame_width_ <= 0 || frame_height_ <= 0) {
        return;
    }

    // Recreate surface if size changed
    if (surface_) {
        int surf_width = cairo_image_surface_get_width(surface_);
        int surf_height = cairo_image_surface_get_height(surface_);
        if (surf_width != frame_width_ || surf_height != frame_height_) {
            cairo_surface_destroy(surface_);
            surface_ = nullptr;
        }
    }

    if (!surface_) {
        surface_ = cairo_image_surface_create(CAIRO_FORMAT_RGB24, frame_width_, frame_height_);
        if (cairo_surface_status(surface_) != CAIRO_STATUS_SUCCESS) {
            LOG_ERROR("Failed to create cairo surface");
            cairo_surface_destroy(surface_);
            surface_ = nullptr;
            return;
        }
        LOG_DEBUG("Created cairo surface: {}x{}", frame_width_, frame_height_);
    }

    // Copy RGB24 data to ARGB32 surface (cairo uses BGRA internally)
    unsigned char* surf_data = cairo_image_surface_get_data(surface_);
    int stride = cairo_image_surface_get_stride(surface_);

    cairo_surface_flush(surface_);

    for (int y = 0; y < frame_height_; ++y) {
        const uint8_t* src = frame_buffer_.data() + y * frame_width_ * 3;
        uint8_t* dst = surf_data + y * stride;

        for (int x = 0; x < frame_width_; ++x) {
            // Convert RGB24 to BGRA (cairo's native format)
            dst[0] = src[2];  // B
            dst[1] = src[1];  // G
            dst[2] = src[0];  // R
            dst[3] = 255;     // A
            src += 3;
            dst += 4;
        }
    }

    cairo_surface_mark_dirty(surface_);
}

} // namespace baichuan
