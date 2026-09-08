#include "video/dashboard_display.h"
#include "utils/logger.h"

#include <cstring>
#include <cctype>
#include <algorithm>
#include <thread>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>

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

    // Clamp the default size to the monitor work area so the window (and its
    // bottom controls like Quit) always fit on screen. Without this, a tall grid
    // pushes the menu's bottom buttons off the screen edge. The grid panes expand,
    // so they simply render a little smaller. Leave headroom for the title bar.
    if (GdkDisplay* disp = gdk_display_get_default()) {
        // Clamp to the SMALLEST monitor's work area: the WM may open the window on
        // any monitor, so fitting the smallest guarantees the bottom controls are
        // always on screen (multi-monitor setups can mix, e.g. 2160p + 1080p).
        int max_w = 0, max_h = 0;
        int n = gdk_display_get_n_monitors(disp);
        for (int i = 0; i < n; i++) {
            GdkMonitor* mon = gdk_display_get_monitor(disp, i);
            if (!mon) continue;
            GdkRectangle area;
            gdk_monitor_get_workarea(mon, &area);
            int avail_h = area.height - 80;  // room for the title bar / decorations
            if (max_w == 0 || area.width < max_w) max_w = area.width;
            if (max_h == 0 || avail_h    < max_h) max_h = avail_h;
        }
        if (max_w > 0 && window_width  > max_w) window_width  = max_w;
        if (max_h > 0 && window_height > max_h) window_height = max_h;
    }

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

    // Add Quit button at bottom, then Help just above it (pack_end stacks upward).
    GtkWidget* quit_button = gtk_button_new_with_label("Quit");
    gtk_box_pack_end(GTK_BOX(menu_box_), quit_button, FALSE, FALSE, 0);
    g_signal_connect(quit_button, "clicked", G_CALLBACK(on_quit_clicked), this);

    GtkWidget* help_button = gtk_button_new_with_label("Help");
    gtk_box_pack_end(GTK_BOX(menu_box_), help_button, FALSE, FALSE, 0);
    g_signal_connect(help_button, "clicked", G_CALLBACK(on_help_clicked), this);

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
        pane->type = cameras[i].type;

        // Create frame container with title
        pane->frame_widget = gtk_frame_new(pane->name.c_str());
        gtk_frame_set_label_align(GTK_FRAME(pane->frame_widget), 0.5, 0.5);

        // Create drawing area. Use a small minimum so the window can be clamped/
        // resized to fit the screen; the panes expand to fill available space, so
        // this only affects how small the grid may shrink, not its normal size.
        pane->drawing_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(pane->drawing_area, 160, 90);
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

    // Listen for keyboard hotkeys (1-9 focus a camera, 0 = overview, R = toggle resolution)
    gtk_widget_add_events(window_, GDK_KEY_PRESS_MASK);
    g_signal_connect(window_, "key-press-event", G_CALLBACK(on_key_press), this);

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

        // Copy BGRA data directly to surface (decoder outputs Cairo's native format)
        unsigned char* surf_data = cairo_image_surface_get_data(pane->surface);
        int stride = cairo_image_surface_get_stride(pane->surface);
        int src_stride = pane->frame_width * 4;

        cairo_surface_flush(pane->surface);

        for (int y = 0; y < pane->frame_height; ++y) {
            memcpy(surf_data + y * stride,
                   pane->frame_buffer.data() + y * src_stride,
                   src_stride);
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

namespace {

// Shared state for one open stream-info panel. Held by shared_ptr so worker
// threads and GTK idle callbacks can safely outlive early dialog closure:
// widgets are only ever touched on the GTK main thread, guarded by `alive`.
struct StreamInfoState {
    GtkWidget* dialog = nullptr;
    GtkWidget* text_view = nullptr;
    GtkWidget* discover_btn = nullptr;
    GtkWidget* onvif_btn = nullptr;
    GtkWidget* spinner = nullptr;
    std::atomic<bool> alive{true};
    std::atomic<bool> discovering{false};
    std::atomic<bool> onvif_running{false};

    int pane_index = -1;
    bool is_rtsp = false;   // gates the RTSP-only Discover/ONVIF buttons
    baichuan::DiscoverStreamsCallback discover_fn;
    baichuan::OnvifStreamsCallback onvif_fn;

    std::mutex mtx;
    std::string camera_name;
    std::vector<baichuan::StreamEntry> configured;
    std::string configured_status = "Probing configured streams...";
    std::vector<baichuan::StreamEntry> discovered;
    std::string discovered_status;  // empty until Discover is pressed
    std::vector<baichuan::StreamEntry> onvif;
    std::string onvif_status;       // empty until ONVIF is pressed
};

std::string render_state(StreamInfoState* st) {
    std::lock_guard<std::mutex> lock(st->mtx);
    std::string out = "Camera: " + st->camera_name + "\n";
    out += "==================================================\n\n";
    out += "CONFIGURED STREAMS\n";
    if (!st->configured.empty()) {
        for (auto& e : st->configured) {
            out += "  " + e.label + "\n";
            out += "    " + e.url + "\n";
            out += "    -> " + e.summary + "\n\n";
        }
    } else {
        out += "  " + st->configured_status + "\n\n";
    }
    out += "DISCOVERED STREAMS\n";
    if (!st->discovered_status.empty() || !st->discovered.empty()) {
        if (!st->discovered.empty()) {
            for (auto& e : st->discovered) {
                out += "  " + e.label + "\n";
                out += "    " + e.url + "\n";
                out += "    -> " + e.summary + "\n\n";
            }
        }
        if (!st->discovered_status.empty()) {
            out += "  " + st->discovered_status + "\n";
        }
    } else {
        out += st->is_rtsp ? "  (press \"Discover\" to probe common RTSP paths)\n"
                           : "  (RTSP cameras only)\n";
    }
    out += "\nONVIF (advertised by the camera)\n";
    if (!st->onvif_status.empty() || !st->onvif.empty()) {
        for (auto& e : st->onvif) {
            out += "  " + e.label + "\n";
            if (!e.url.empty()) out += "    " + e.url + "\n";
            out += "    -> " + e.summary + "\n\n";
        }
        if (!st->onvif_status.empty()) out += "  " + st->onvif_status + "\n";
    } else {
        out += st->is_rtsp ? "  (press \"ONVIF\" to query the camera's advertised streams)\n"
                           : "  (RTSP cameras only)\n";
    }
    return out;
}

// Runs on the GTK main thread. data owns a heap shared_ptr copy.
gboolean refresh_stream_info(gpointer data) {
    auto* holder = static_cast<std::shared_ptr<StreamInfoState>*>(data);
    std::shared_ptr<StreamInfoState> st = *holder;
    delete holder;
    if (st->alive.load()) {
        std::string text = render_state(st.get());
        GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(st->text_view));
        gtk_text_buffer_set_text(buf, text.c_str(), -1);
        bool disc = st->discovering.load();
        bool onv = st->onvif_running.load();
        gtk_widget_set_sensitive(st->discover_btn, st->is_rtsp && !disc);
        if (st->onvif_btn) gtk_widget_set_sensitive(st->onvif_btn, st->is_rtsp && !onv);
        bool busy = disc || onv;
        if (busy) { gtk_widget_show(st->spinner); gtk_spinner_start(GTK_SPINNER(st->spinner)); }
        else { gtk_spinner_stop(GTK_SPINNER(st->spinner)); gtk_widget_hide(st->spinner); }
    }
    return G_SOURCE_REMOVE;
}

void post_refresh(std::shared_ptr<StreamInfoState> st) {
    g_idle_add(refresh_stream_info, new std::shared_ptr<StreamInfoState>(std::move(st)));
}

// "Discover" button: probe common RTSP paths on a worker thread, then refresh.
void on_discover_clicked(GtkWidget* /*w*/, gpointer data) {
    // data is a borrowed shared_ptr* owned by the dialog (set via set_data_full).
    auto st = *static_cast<std::shared_ptr<StreamInfoState>*>(data);
    if (st->discovering.exchange(true)) return;   // already running
    if (!st->discover_fn) {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->discovered_status = "Discovery not available for this camera.";
        st->discovering.store(false);
        post_refresh(st);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->discovered.clear();
        st->discovered_status = "Discovering (probing common paths)...";
    }
    post_refresh(st);

    auto fn = st->discover_fn;
    int idx = st->pane_index;
    std::thread([st, fn, idx]() {
        std::vector<baichuan::StreamEntry> found = fn(idx);
        {
            std::lock_guard<std::mutex> lock(st->mtx);
            st->discovered = std::move(found);
            st->discovered_status = st->discovered.empty()
                ? "No additional streams found."
                : "";
        }
        st->discovering.store(false);
        post_refresh(st);
    }).detach();
}

// "ONVIF" button: query the camera's advertised streams on a worker thread.
void on_onvif_clicked(GtkWidget* /*w*/, gpointer data) {
    auto st = *static_cast<std::shared_ptr<StreamInfoState>*>(data);
    if (st->onvif_running.exchange(true)) return;
    if (!st->onvif_fn) {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->onvif_status = "ONVIF not available for this camera.";
        st->onvif_running.store(false);
        post_refresh(st);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->onvif.clear();
        st->onvif_status = "Querying ONVIF...";
    }
    post_refresh(st);

    auto fn = st->onvif_fn;
    int idx = st->pane_index;
    std::thread([st, fn, idx]() {
        std::vector<baichuan::StreamEntry> found = fn(idx);
        {
            std::lock_guard<std::mutex> lock(st->mtx);
            st->onvif = std::move(found);
            st->onvif_status = st->onvif.empty() ? "No ONVIF streams returned." : "";
        }
        st->onvif_running.store(false);
        post_refresh(st);
    }).detach();
}

} // namespace

void DashboardDisplay::show_stream_info(int pane_index) {
    if (pane_index < 0 || static_cast<size_t>(pane_index) >= panes_.size()) return;

    auto st = std::make_shared<StreamInfoState>();
    st->pane_index = pane_index;
    st->discover_fn = discover_streams_callback_;
    st->onvif_fn = onvif_streams_callback_;
    st->camera_name = panes_[pane_index]->name;
    const bool is_rtsp = panes_[pane_index]->type == CameraType::Rtsp;
    st->is_rtsp = is_rtsp;

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Streams", GTK_WINDOW(window_),
        static_cast<GtkDialogFlags>(GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 620, 460);
    st->dialog = dialog;

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    // Action row: Discover button + spinner.
    GtkWidget* action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    st->discover_btn = gtk_button_new_with_label("Discover");
    gtk_widget_set_tooltip_text(st->discover_btn, "Probe common RTSP paths for streams");
    gtk_box_pack_start(GTK_BOX(action_row), st->discover_btn, FALSE, FALSE, 4);
    st->onvif_btn = gtk_button_new_with_label("ONVIF");
    gtk_widget_set_tooltip_text(st->onvif_btn, "Query the camera's advertised streams via ONVIF");
    gtk_box_pack_start(GTK_BOX(action_row), st->onvif_btn, FALSE, FALSE, 4);
    st->spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(action_row), st->spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), action_row, FALSE, FALSE, 4);

    // Discovery (RTSP path probing) and ONVIF only make sense for RTSP cameras;
    // for baichuan/mjpeg the panel just lists the configured/available feeds.
    gtk_widget_set_sensitive(st->discover_btn, is_rtsp);
    gtk_widget_set_sensitive(st->onvif_btn, is_rtsp);

    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);
    st->text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(st->text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(st->text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(st->text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(st->text_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(st->text_view), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(st->text_view), 8);
    gtk_container_add(GTK_CONTAINER(scrolled), st->text_view);
    gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 0);

    // The dialog owns one reference to the shared state; its deleter runs on
    // destroy. Worker threads hold their own references and check `alive`.
    auto* dialog_ref = new std::shared_ptr<StreamInfoState>(st);
    g_object_set_data_full(G_OBJECT(dialog), "stream-state", dialog_ref,
        [](gpointer p) {
            auto* ref = static_cast<std::shared_ptr<StreamInfoState>*>(p);
            (*ref)->alive.store(false);   // stop pending idle callbacks touching widgets
            delete ref;
        });

    g_signal_connect(st->discover_btn, "clicked",
                     G_CALLBACK(on_discover_clicked), dialog_ref);
    g_signal_connect(st->onvif_btn, "clicked",
                     G_CALLBACK(on_onvif_clicked), dialog_ref);
    g_signal_connect(dialog, "response",
                     G_CALLBACK(gtk_widget_destroy), nullptr);

    gtk_widget_show_all(dialog);
    gtk_widget_hide(st->spinner);   // shown only while discovering

    // Render the initial "probing..." placeholder immediately.
    post_refresh(st);

    // Introspect the configured streams on a worker thread.
    if (probe_streams_callback_) {
        auto fn = probe_streams_callback_;
        int idx = pane_index;
        std::thread([st, fn, idx]() {
            std::vector<StreamEntry> cfg = fn(idx);
            {
                std::lock_guard<std::mutex> lock(st->mtx);
                st->configured = std::move(cfg);
                if (st->configured.empty())
                    st->configured_status = "No configured streams to probe.";
            }
            post_refresh(st);
        }).detach();
    } else {
        std::lock_guard<std::mutex> lock(st->mtx);
        st->configured_status = "Introspection not available.";
        post_refresh(st);
    }
}

void DashboardDisplay::on_help_clicked(GtkWidget* widget, gpointer user_data) {
    (void)widget;
    DashboardDisplay* self = static_cast<DashboardDisplay*>(user_data);

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Help", GTK_WINDOW(self->window_),
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "_Close", GTK_RESPONSE_CLOSE, nullptr);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 560);

    // Monospace, read-only, scrollable view of the help text.
    GtkWidget* scrolled = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_hexpand(scrolled, TRUE);
    gtk_widget_set_vexpand(scrolled, TRUE);

    GtkWidget* text_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text_view), 10);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(text_view), 10);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text_view), 8);

    GtkTextBuffer* buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    const std::string& text = self->help_text_.empty()
        ? std::string("No help text available.") : self->help_text_;
    gtk_text_buffer_set_text(buffer, text.c_str(), -1);

    gtk_container_add(GTK_CONTAINER(scrolled), text_view);
    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(content), scrolled, TRUE, TRUE, 0);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));   // nested loop; idle video updates keep running
    gtk_widget_destroy(dialog);
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

// --- Keyboard hotkeys ---

void DashboardDisplay::set_hotkey_modifier(const std::string& modifier) {
    std::string m = modifier;
    std::transform(m.begin(), m.end(), m.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    if (m.empty() || m == "none") {
        hotkey_modifier_mask_ = 0;
    } else if (m == "ctrl" || m == "control") {
        hotkey_modifier_mask_ = GDK_CONTROL_MASK;
    } else if (m == "alt" || m == "mod1") {
        hotkey_modifier_mask_ = GDK_MOD1_MASK;
    } else if (m == "shift") {
        hotkey_modifier_mask_ = GDK_SHIFT_MASK;
    } else if (m == "super" || m == "win") {
        hotkey_modifier_mask_ = GDK_SUPER_MASK;
    } else {
        LOG_WARN("Unknown hotkey modifier '{}', using none", modifier);
        hotkey_modifier_mask_ = 0;
    }
}

void DashboardDisplay::focus_pane(size_t index) {
    if (index >= panes_.size()) return;
    focused_pane_ = static_cast<int>(index);
    show_only({index});
}

void DashboardDisplay::show_overview() {
    focused_pane_ = -1;
    show_all_panes();
}

gboolean DashboardDisplay::on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
    (void)widget;
    DashboardDisplay* self = static_cast<DashboardDisplay*>(user_data);

    // Only react when the required modifier (and no other) is held.
    // Ignore Lock/Shift here except when Shift is the configured modifier.
    guint mods = event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK | GDK_SHIFT_MASK);
    if (self->hotkey_modifier_mask_ != GDK_SHIFT_MASK) {
        mods &= ~GDK_SHIFT_MASK;  // Shift not part of the chord — tolerate it
    }
    if (mods != self->hotkey_modifier_mask_) {
        return FALSE;  // let other handlers process it
    }

    guint kv = event->keyval;

    // Digit 0 (top row or keypad) -> overview
    if (kv == GDK_KEY_0 || kv == GDK_KEY_KP_0) {
        self->show_overview();
        return TRUE;
    }

    // Digits 1-9 -> focus that camera (1 = pane 0)
    int digit = -1;
    if (kv >= GDK_KEY_1 && kv <= GDK_KEY_9) {
        digit = static_cast<int>(kv - GDK_KEY_1) + 1;
    } else if (kv >= GDK_KEY_KP_1 && kv <= GDK_KEY_KP_9) {
        digit = static_cast<int>(kv - GDK_KEY_KP_1) + 1;
    }
    if (digit >= 1) {
        size_t idx = static_cast<size_t>(digit - 1);
        if (idx < self->panes_.size()) {
            self->focus_pane(idx);
        }
        return TRUE;
    }

    // R -> toggle low/high resolution stream
    if (kv == GDK_KEY_r || kv == GDK_KEY_R) {
        if (self->resolution_callback_) {
            self->resolution_callback_(self->focused_pane_);
        }
        return TRUE;
    }

    // I -> show the stream-info panel for the focused camera (no-op in overview)
    if (kv == GDK_KEY_i || kv == GDK_KEY_I) {
        if (self->focused_pane_ >= 0) {
            self->show_stream_info(self->focused_pane_);
        }
        return TRUE;
    }

    return FALSE;
}

} // namespace baichuan
