#include "video/dashboard_display.h"
#include "utils/logger.h"

#include <cstring>
#include <algorithm>

namespace baichuan {

DashboardDisplay::DashboardDisplay() = default;

DashboardDisplay::~DashboardDisplay() {
    panes_.clear();
}

bool DashboardDisplay::init_gtk(int* argc, char*** argv) {
    return gtk_init_check(argc, argv) == TRUE;
}

bool DashboardDisplay::create(const std::string& title, const std::vector<CameraConfig>& cameras, int columns) {
    columns_ = columns;
    int num_cameras = static_cast<int>(cameras.size());
    int rows = (num_cameras + columns - 1) / columns;

    // Calculate window size based on grid
    int pane_width = 640;
    int pane_height = 360;
    int menu_width = 120;
    int window_width = menu_width + (pane_width * columns);
    int window_height = pane_height * rows;

    // Create main window
    window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    if (!window_) {
        LOG_ERROR("Failed to create GTK window");
        return false;
    }

    gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
    gtk_window_set_default_size(GTK_WINDOW(window_), window_width, window_height);
    gtk_window_set_resizable(GTK_WINDOW(window_), TRUE);

    // Create horizontal box for menu + grid
    main_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_container_add(GTK_CONTAINER(window_), main_box_);

    // Create left menu panel
    menu_box_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_size_request(menu_box_, menu_width, -1);
    gtk_box_pack_start(GTK_BOX(main_box_), menu_box_, FALSE, FALSE, 0);

    // Style the menu box
    GtkCssProvider* css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider,
        "box { background-color: #2d2d2d; padding: 10px; }"
        "button { margin: 5px; }"
        "label { color: white; }",
        -1, nullptr);
    GtkStyleContext* context = gtk_widget_get_style_context(menu_box_);
    gtk_style_context_add_provider(context, GTK_STYLE_PROVIDER(css_provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_USER);

    // Add title label
    GtkWidget* title_label = gtk_label_new("Dashboard");
    gtk_box_pack_start(GTK_BOX(menu_box_), title_label, FALSE, FALSE, 10);

    // Add camera count label
    char count_text[64];
    snprintf(count_text, sizeof(count_text), "%d cameras", num_cameras);
    GtkWidget* count_label = gtk_label_new(count_text);
    gtk_box_pack_start(GTK_BOX(menu_box_), count_label, FALSE, FALSE, 5);

    // Add Quit button at bottom
    GtkWidget* quit_button = gtk_button_new_with_label("Quit");
    gtk_box_pack_end(GTK_BOX(menu_box_), quit_button, FALSE, FALSE, 0);
    g_signal_connect(quit_button, "clicked", G_CALLBACK(on_quit_clicked), this);

    // Create grid for camera panes
    grid_ = gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(grid_), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid_), TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(grid_), 2);
    gtk_grid_set_column_spacing(GTK_GRID(grid_), 2);
    gtk_widget_set_hexpand(grid_, TRUE);
    gtk_widget_set_vexpand(grid_, TRUE);
    gtk_box_pack_start(GTK_BOX(main_box_), grid_, TRUE, TRUE, 0);

    // Create camera panes
    for (int i = 0; i < num_cameras; i++) {
        auto pane = std::make_unique<CameraPane>();
        pane->name = cameras[i].name.empty() ? cameras[i].host : cameras[i].name;
        pane->status = "Connecting...";

        // Create frame container with title
        pane->frame_widget = gtk_frame_new(pane->name.c_str());
        gtk_frame_set_label_align(GTK_FRAME(pane->frame_widget), 0.5, 0.5);

        // Create drawing area
        pane->drawing_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(pane->drawing_area, pane_width - 10, pane_height - 30);
        gtk_widget_set_hexpand(pane->drawing_area, TRUE);
        gtk_widget_set_vexpand(pane->drawing_area, TRUE);
        gtk_container_add(GTK_CONTAINER(pane->frame_widget), pane->drawing_area);

        // Connect draw signal - store pane index in user data
        g_object_set_data(G_OBJECT(pane->drawing_area), "pane_index", GINT_TO_POINTER(i));
        g_object_set_data(G_OBJECT(pane->drawing_area), "dashboard", this);
        g_signal_connect(pane->drawing_area, "draw", G_CALLBACK(on_draw), pane.get());

        // Add to grid
        int row = i / columns;
        int col = i % columns;
        gtk_grid_attach(GTK_GRID(grid_), pane->frame_widget, col, row, 1, 1);

        panes_.push_back(std::move(pane));
    }

    // Connect window close
    g_signal_connect(window_, "delete-event", G_CALLBACK(on_delete_event), this);

    // Show window
    gtk_widget_show_all(window_);

    LOG_INFO("Created dashboard: {} cameras in {}x{} grid", num_cameras, columns, rows);
    return true;
}

void DashboardDisplay::update_frame(size_t pane_index, const DecodedFrame& frame) {
    if (pane_index >= panes_.size() || quit_requested_.load()) {
        return;
    }

    auto& pane = panes_[pane_index];

    {
        std::lock_guard<std::mutex> lock(pane->mutex);
        pane->frame_buffer = frame.rgb_data;
        pane->frame_width = frame.width;
        pane->frame_height = frame.height;
        pane->has_video.store(true);
        pane->frame_pending.store(true);
    }

    // Schedule redraw
    g_idle_add([](gpointer data) -> gboolean {
        CameraPane* p = static_cast<CameraPane*>(data);
        if (p->drawing_area && p->frame_pending.load()) {
            gtk_widget_queue_draw(p->drawing_area);
            p->frame_pending.store(false);
        }
        return FALSE;
    }, pane.get());
}

void DashboardDisplay::set_status(size_t pane_index, const std::string& status) {
    if (pane_index >= panes_.size()) {
        return;
    }

    auto& pane = panes_[pane_index];

    {
        std::lock_guard<std::mutex> lock(pane->mutex);
        pane->status = status;
    }

    // Schedule redraw
    if (pane->drawing_area) {
        g_idle_add([](gpointer data) -> gboolean {
            CameraPane* p = static_cast<CameraPane*>(data);
            if (p->drawing_area) {
                gtk_widget_queue_draw(p->drawing_area);
            }
            return FALSE;
        }, pane.get());
    }
}

void DashboardDisplay::run() {
    if (!window_) {
        LOG_ERROR("Window not created");
        return;
    }

    LOG_DEBUG("Starting GTK main loop");
    gtk_main();
    LOG_DEBUG("GTK main loop ended");
}

void DashboardDisplay::quit() {
    quit_requested_.store(true);
    gtk_main_quit();
}

gboolean DashboardDisplay::on_draw(GtkWidget* widget, cairo_t* cr, gpointer user_data) {
    CameraPane* pane = static_cast<CameraPane*>(user_data);

    int area_width = gtk_widget_get_allocated_width(widget);
    int area_height = gtk_widget_get_allocated_height(widget);

    // Clear background
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_paint(cr);

    draw_pane(pane, cr, area_width, area_height);

    return FALSE;
}

void DashboardDisplay::draw_pane(CameraPane* pane, cairo_t* cr, int width, int height) {
    std::lock_guard<std::mutex> lock(pane->mutex);

    if (pane->has_video.load() && !pane->frame_buffer.empty() &&
        pane->frame_width > 0 && pane->frame_height > 0) {

        // Update surface if needed
        if (pane->surface) {
            int surf_width = cairo_image_surface_get_width(pane->surface);
            int surf_height = cairo_image_surface_get_height(pane->surface);
            if (surf_width != pane->frame_width || surf_height != pane->frame_height) {
                cairo_surface_destroy(pane->surface);
                pane->surface = nullptr;
            }
        }

        if (!pane->surface) {
            pane->surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24,
                                                        pane->frame_width, pane->frame_height);
        }

        // Copy RGB data to surface
        unsigned char* surf_data = cairo_image_surface_get_data(pane->surface);
        int stride = cairo_image_surface_get_stride(pane->surface);

        cairo_surface_flush(pane->surface);

        for (int y = 0; y < pane->frame_height; ++y) {
            const uint8_t* src = pane->frame_buffer.data() + y * pane->frame_width * 3;
            uint8_t* dst = surf_data + y * stride;

            for (int x = 0; x < pane->frame_width; ++x) {
                dst[0] = src[2];  // B
                dst[1] = src[1];  // G
                dst[2] = src[0];  // R
                dst[3] = 255;     // A
                src += 3;
                dst += 4;
            }
        }

        cairo_surface_mark_dirty(pane->surface);

        // Draw scaled
        double scale_x = static_cast<double>(width) / pane->frame_width;
        double scale_y = static_cast<double>(height) / pane->frame_height;
        double scale = std::min(scale_x, scale_y);

        double x_offset = (width - pane->frame_width * scale) / 2.0;
        double y_offset = (height - pane->frame_height * scale) / 2.0;

        cairo_save(cr);
        cairo_translate(cr, x_offset, y_offset);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, pane->surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
        cairo_paint(cr);
        cairo_restore(cr);

    } else {
        // Show status message
        if (!pane->status.empty()) {
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, 14.0);

            cairo_text_extents_t extents;
            cairo_text_extents(cr, pane->status.c_str(), &extents);

            double x = (width - extents.width) / 2.0;
            double y = (height + extents.height) / 2.0;

            cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
            cairo_move_to(cr, x, y);
            cairo_show_text(cr, pane->status.c_str());
        }
    }
}

// --- Dynamic pane control ---

struct ShowOnlyData {
    DashboardDisplay* display;
    std::vector<size_t> indices;
};

void DashboardDisplay::show_only(const std::vector<size_t>& indices) {
    auto* data = new ShowOnlyData{this, indices};
    g_idle_add([](gpointer user_data) -> gboolean {
        auto* d = static_cast<ShowOnlyData*>(user_data);
        auto* self = d->display;

        for (size_t i = 0; i < self->panes_.size(); i++) {
            bool should_show = false;
            for (size_t idx : d->indices) {
                if (idx == i) { should_show = true; break; }
            }
            if (should_show) {
                gtk_widget_show(self->panes_[i]->frame_widget);
            } else {
                gtk_widget_hide(self->panes_[i]->frame_widget);
            }
        }

        delete d;
        return FALSE;
    }, data);
}

void DashboardDisplay::show_all_panes() {
    g_idle_add([](gpointer user_data) -> gboolean {
        auto* self = static_cast<DashboardDisplay*>(user_data);
        for (auto& pane : self->panes_) {
            gtk_widget_show(pane->frame_widget);
        }
        return FALSE;
    }, this);
}

struct AddPaneData {
    DashboardDisplay* display;
    std::string name;
    bool replace;  // if true, hide all existing panes
};

size_t DashboardDisplay::add_pane(const CameraConfig& config, bool replace) {
    auto pane = std::make_unique<CameraPane>();
    pane->name = config.name.empty() ? config.host : config.name;
    pane->status = "Connecting...";

    size_t new_index = panes_.size();
    panes_.push_back(std::move(pane));

    auto* data = new AddPaneData{this, panes_.back()->name, replace};

    // We need to create the GTK widgets on the main thread
    g_idle_add([](gpointer user_data) -> gboolean {
        auto* d = static_cast<AddPaneData*>(user_data);
        auto* self = d->display;

        size_t idx = self->panes_.size() - 1;
        auto& pane = self->panes_[idx];

        // Create frame container
        pane->frame_widget = gtk_frame_new(pane->name.c_str());
        gtk_frame_set_label_align(GTK_FRAME(pane->frame_widget), 0.5, 0.5);

        // Create drawing area
        pane->drawing_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(pane->drawing_area, 630, 330);
        gtk_widget_set_hexpand(pane->drawing_area, TRUE);
        gtk_widget_set_vexpand(pane->drawing_area, TRUE);
        gtk_container_add(GTK_CONTAINER(pane->frame_widget), pane->drawing_area);

        // Connect draw signal
        g_object_set_data(G_OBJECT(pane->drawing_area), "pane_index",
                          GINT_TO_POINTER(static_cast<int>(idx)));
        g_object_set_data(G_OBJECT(pane->drawing_area), "dashboard", self);
        g_signal_connect(pane->drawing_area, "draw", G_CALLBACK(on_draw), pane.get());

        // Add to grid
        int row = static_cast<int>(idx) / self->columns_;
        int col = static_cast<int>(idx) % self->columns_;
        gtk_grid_attach(GTK_GRID(self->grid_), pane->frame_widget, col, row, 1, 1);

        gtk_widget_show_all(pane->frame_widget);

        if (d->replace) {
            for (size_t i = 0; i < self->panes_.size() - 1; i++) {
                gtk_widget_hide(self->panes_[i]->frame_widget);
            }
        }

        delete d;
        return FALSE;
    }, data);

    return new_index;
}

void DashboardDisplay::hide_window() {
    g_idle_add([](gpointer user_data) -> gboolean {
        auto* self = static_cast<DashboardDisplay*>(user_data);
        if (self->window_) {
            gtk_widget_hide(self->window_);
        }
        return FALSE;
    }, this);
}

void DashboardDisplay::show_window() {
    g_idle_add([](gpointer user_data) -> gboolean {
        auto* self = static_cast<DashboardDisplay*>(user_data);
        if (self->window_) {
            gtk_widget_show(self->window_);
        }
        return FALSE;
    }, this);
}

void DashboardDisplay::set_fullscreen(bool fullscreen) {
    auto* data = new std::pair<DashboardDisplay*, bool>(this, fullscreen);
    g_idle_add([](gpointer user_data) -> gboolean {
        auto* d = static_cast<std::pair<DashboardDisplay*, bool>*>(user_data);
        if (d->first->window_) {
            if (d->second) {
                gtk_window_fullscreen(GTK_WINDOW(d->first->window_));
            } else {
                gtk_window_unfullscreen(GTK_WINDOW(d->first->window_));
            }
        }
        delete d;
        return FALSE;
    }, data);
}

std::vector<DashboardDisplay::PaneInfo> DashboardDisplay::get_pane_info(const std::vector<bool>& connected_flags) {
    std::vector<PaneInfo> result;
    for (size_t i = 0; i < panes_.size(); i++) {
        auto& pane = panes_[i];
        bool visible = pane->frame_widget ? gtk_widget_get_visible(pane->frame_widget) : false;
        bool connected = (i < connected_flags.size()) ? connected_flags[i] : true;
        result.push_back({pane->name, visible, connected});
    }
    return result;
}

gboolean DashboardDisplay::on_delete_event(GtkWidget* widget, GdkEvent* event, gpointer user_data) {
    (void)widget;
    (void)event;

    DashboardDisplay* self = static_cast<DashboardDisplay*>(user_data);

    LOG_DEBUG("Dashboard close requested");
    self->quit_requested_.store(true);

    if (self->quit_callback_) {
        self->quit_callback_();
    }

    gtk_main_quit();
    return FALSE;
}

void DashboardDisplay::on_quit_clicked(GtkWidget* widget, gpointer user_data) {
    (void)widget;

    DashboardDisplay* self = static_cast<DashboardDisplay*>(user_data);

    LOG_DEBUG("Quit button clicked");
    self->quit_requested_.store(true);

    if (self->quit_callback_) {
        self->quit_callback_();
    }

    gtk_main_quit();
}

} // namespace baichuan
