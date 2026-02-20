#include "mjpeg/mjpeg_source.h"
#include "utils/logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <algorithm>
#include <sstream>

#include <jpeglib.h>

namespace baichuan {

MjpegSource::MjpegSource() = default;

MjpegSource::~MjpegSource() {
    stop();
    cleanup();
}

void MjpegSource::set_url(const std::string& url) {
    url_ = url;
}

void MjpegSource::set_timeout(int seconds) {
    timeout_seconds_ = seconds;
}

bool MjpegSource::connect() {
    if (url_.empty()) {
        LOG_ERROR("MJPEG URL not set");
        return false;
    }

    cleanup();

    // Parse URL
    if (!parse_url()) {
        LOG_ERROR("Failed to parse MJPEG URL: {}", url_);
        return false;
    }

    LOG_INFO("Connecting to MJPEG: {}:{}{}", host_, port_, path_);

    // Resolve hostname
    struct hostent* server = gethostbyname(host_.c_str());
    if (!server) {
        LOG_ERROR("Failed to resolve hostname: {}", host_);
        return false;
    }

    // Create socket
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        LOG_ERROR("Failed to create socket");
        return false;
    }

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec = timeout_seconds_;
    tv.tv_usec = 0;
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(socket_fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // Connect
    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_);
    std::memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (::connect(socket_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("Failed to connect to {}:{}", host_, port_);
        cleanup();
        return false;
    }

    // Send HTTP request
    if (!send_http_request()) {
        LOG_ERROR("Failed to send HTTP request");
        cleanup();
        return false;
    }

    // Read HTTP response headers
    if (!read_http_headers()) {
        LOG_ERROR("Failed to read HTTP response");
        cleanup();
        return false;
    }

    connected_.store(true);
    LOG_INFO("MJPEG connected, boundary: {}", boundary_);

    return true;
}

bool MjpegSource::start() {
    if (!connected_.load()) {
        LOG_ERROR("MJPEG not connected");
        return false;
    }

    if (running_.load()) {
        LOG_WARN("MJPEG already streaming");
        return true;
    }

    running_.store(true);
    receive_thread_ = std::thread(&MjpegSource::receive_loop, this);

    LOG_INFO("MJPEG streaming started");
    return true;
}

void MjpegSource::stop() {
    running_.store(false);

    // Close socket to unblock any reads
    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
    }

    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }

    LOG_INFO("MJPEG streaming stopped");
}

bool MjpegSource::is_connected() const {
    return connected_.load();
}

bool MjpegSource::is_streaming() const {
    return running_.load();
}

void MjpegSource::on_frame(DecodedFrameCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    frame_callback_ = std::move(cb);
}

void MjpegSource::on_error(ErrorCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    error_callback_ = std::move(cb);
}

void MjpegSource::on_info(InfoCallback cb) {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    info_callback_ = std::move(cb);
}

bool MjpegSource::parse_url() {
    // Format: http://[user:pass@]host[:port]/path
    std::string url = url_;

    // Check and strip http://
    if (url.substr(0, 7) != "http://") {
        LOG_ERROR("MJPEG URL must start with http://");
        return false;
    }
    url = url.substr(7);

    // Extract credentials if present
    size_t at_pos = url.find('@');
    if (at_pos != std::string::npos) {
        std::string credentials = url.substr(0, at_pos);
        auth_header_ = "Authorization: Basic " + base64_encode(credentials) + "\r\n";
        url = url.substr(at_pos + 1);
    }

    // Extract path
    size_t path_pos = url.find('/');
    if (path_pos != std::string::npos) {
        path_ = url.substr(path_pos);
        url = url.substr(0, path_pos);
    } else {
        path_ = "/";
    }

    // Extract port
    size_t port_pos = url.find(':');
    if (port_pos != std::string::npos) {
        port_ = static_cast<uint16_t>(std::stoi(url.substr(port_pos + 1)));
        host_ = url.substr(0, port_pos);
    } else {
        port_ = 80;
        host_ = url;
    }

    return !host_.empty();
}

std::string MjpegSource::base64_encode(const std::string& input) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;

    int val = 0;
    int bits = -6;

    for (unsigned char c : input) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            result.push_back(chars[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }

    if (bits > -6) {
        result.push_back(chars[((val << 8) >> (bits + 8)) & 0x3F]);
    }

    while (result.size() % 4) {
        result.push_back('=');
    }

    return result;
}

bool MjpegSource::send_http_request() {
    std::ostringstream request;
    request << "GET " << path_ << " HTTP/1.1\r\n"
            << "Host: " << host_ << "\r\n"
            << auth_header_
            << "Connection: keep-alive\r\n"
            << "Accept: multipart/x-mixed-replace\r\n"
            << "\r\n";

    std::string req_str = request.str();
    ssize_t sent = send(socket_fd_, req_str.c_str(), req_str.length(), 0);

    return sent == static_cast<ssize_t>(req_str.length());
}

std::string MjpegSource::read_line() {
    std::string line;
    char c;

    while (recv(socket_fd_, &c, 1, 0) == 1) {
        if (c == '\n') {
            // Remove trailing \r if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        line += c;
    }

    return line;
}

bool MjpegSource::read_http_headers() {
    // Read status line
    std::string status_line = read_line();
    if (status_line.find("200") == std::string::npos) {
        LOG_ERROR("HTTP error: {}", status_line);
        return false;
    }

    // Read headers until empty line
    while (true) {
        std::string header = read_line();
        if (header.empty()) {
            break;
        }

        // Look for Content-Type header with boundary
        if (header.find("Content-Type:") != std::string::npos ||
            header.find("content-type:") != std::string::npos) {

            size_t boundary_pos = header.find("boundary=");
            if (boundary_pos != std::string::npos) {
                boundary_ = header.substr(boundary_pos + 9);
                // Remove quotes if present
                if (!boundary_.empty() && boundary_[0] == '"') {
                    boundary_ = boundary_.substr(1);
                    size_t end_quote = boundary_.find('"');
                    if (end_quote != std::string::npos) {
                        boundary_ = boundary_.substr(0, end_quote);
                    }
                }
                // Remove any trailing whitespace or semicolon
                size_t end = boundary_.find_first_of(" \t;");
                if (end != std::string::npos) {
                    boundary_ = boundary_.substr(0, end);
                }
            }
        }

        LOG_DEBUG("MJPEG header: {}", header);
    }

    if (boundary_.empty()) {
        LOG_ERROR("No boundary found in Content-Type header");
        return false;
    }

    return true;
}

bool MjpegSource::read_bytes(std::vector<uint8_t>& buffer, size_t count) {
    buffer.resize(count);
    size_t total_read = 0;

    while (total_read < count && running_.load()) {
        ssize_t n = recv(socket_fd_, buffer.data() + total_read, count - total_read, 0);
        if (n <= 0) {
            return false;
        }
        total_read += n;
    }

    return total_read == count;
}

bool MjpegSource::find_boundary() {
    // Look for --boundary
    std::string search = "--" + boundary_;
    std::string buffer;
    char c;

    while (running_.load()) {
        if (recv(socket_fd_, &c, 1, 0) != 1) {
            return false;
        }

        buffer += c;

        // Keep buffer size manageable
        if (buffer.length() > search.length() * 2) {
            buffer = buffer.substr(buffer.length() - search.length());
        }

        if (buffer.find(search) != std::string::npos) {
            // Read the newline after boundary
            read_line();
            return true;
        }
    }

    return false;
}

bool MjpegSource::read_part_headers(size_t& content_length) {
    content_length = 0;

    while (true) {
        std::string header = read_line();
        if (header.empty()) {
            break;
        }

        // Look for Content-Length
        if (header.find("Content-Length:") != std::string::npos ||
            header.find("content-length:") != std::string::npos) {
            size_t colon = header.find(':');
            if (colon != std::string::npos) {
                std::string len_str = header.substr(colon + 1);
                // Trim whitespace
                size_t start = len_str.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    content_length = std::stoul(len_str.substr(start));
                }
            }
        }
    }

    return true;
}

bool MjpegSource::read_jpeg_frame(std::vector<uint8_t>& jpeg_data, size_t content_length) {
    if (content_length > 0) {
        // Content-Length specified, read exact bytes
        return read_bytes(jpeg_data, content_length);
    }

    // No Content-Length, read until boundary
    // Look for JPEG SOI (0xFFD8) and EOI (0xFFD9) markers
    jpeg_data.clear();
    jpeg_data.reserve(100000);  // Typical JPEG size

    uint8_t prev_byte = 0;
    uint8_t curr_byte = 0;
    bool found_soi = false;

    while (running_.load()) {
        if (recv(socket_fd_, &curr_byte, 1, 0) != 1) {
            return false;
        }

        // Check for SOI marker (start of JPEG)
        if (!found_soi) {
            if (prev_byte == 0xFF && curr_byte == 0xD8) {
                jpeg_data.push_back(0xFF);
                jpeg_data.push_back(0xD8);
                found_soi = true;
            }
            prev_byte = curr_byte;
            continue;
        }

        jpeg_data.push_back(curr_byte);

        // Check for EOI marker (end of JPEG)
        if (prev_byte == 0xFF && curr_byte == 0xD9) {
            return true;
        }

        prev_byte = curr_byte;

        // Safety limit
        if (jpeg_data.size() > 10 * 1024 * 1024) {
            LOG_ERROR("JPEG frame too large, aborting");
            return false;
        }
    }

    return false;
}

bool MjpegSource::decode_jpeg(const std::vector<uint8_t>& jpeg_data, DecodedFrame& frame) {
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);

    // Set source to memory buffer
    jpeg_mem_src(&cinfo, jpeg_data.data(), jpeg_data.size());

    // Read header
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    // Request RGB output (libjpeg decodes to RGB)
    cinfo.out_color_space = JCS_RGB;

    // Start decompression
    if (!jpeg_start_decompress(&cinfo)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    frame.width = cinfo.output_width;
    frame.height = cinfo.output_height;
    int rgb_stride = cinfo.output_width * cinfo.output_components;

    // Decode row by row, converting RGB -> BGRA to match decoder output format
    std::vector<uint8_t> rgb_row(rgb_stride);
    frame.rgb_data.resize(frame.width * frame.height * 4);

    while (cinfo.output_scanline < cinfo.output_height) {
        uint8_t* row_ptr = rgb_row.data();
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);

        int y = cinfo.output_scanline - 1;
        uint8_t* dst = frame.rgb_data.data() + y * frame.width * 4;
        const uint8_t* src = rgb_row.data();
        for (int x = 0; x < frame.width; x++) {
            dst[0] = src[2];  // B
            dst[1] = src[1];  // G
            dst[2] = src[0];  // R
            dst[3] = 255;     // A
            src += 3;
            dst += 4;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    return true;
}

void MjpegSource::receive_loop() {
    std::vector<uint8_t> jpeg_data;
    DecodedFrame frame;

    while (running_.load()) {
        // Find next boundary
        if (!find_boundary()) {
            if (running_.load()) {
                LOG_ERROR("Failed to find MJPEG boundary");
                std::lock_guard<std::mutex> lock(callback_mutex_);
                if (error_callback_) {
                    error_callback_("Lost MJPEG stream");
                }
            }
            break;
        }

        // Read part headers
        size_t content_length = 0;
        if (!read_part_headers(content_length)) {
            continue;
        }

        // Read JPEG frame
        if (!read_jpeg_frame(jpeg_data, content_length)) {
            if (running_.load()) {
                LOG_ERROR("Failed to read JPEG frame");
            }
            continue;
        }

        stats_.frames_received++;
        stats_.bytes_received += jpeg_data.size();

        // Decode JPEG
        if (!decode_jpeg(jpeg_data, frame)) {
            LOG_WARN("Failed to decode JPEG frame");
            stats_.decode_errors++;
            continue;
        }

        // Send info callback on first successful decode
        if (!info_sent_) {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (info_callback_) {
                info_callback_(frame.width, frame.height, 0);  // FPS unknown for MJPEG
            }
            info_sent_ = true;
            LOG_INFO("MJPEG stream: {}x{}", frame.width, frame.height);
        }

        // Deliver decoded frame
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (frame_callback_) {
            frame_callback_(frame);
        }
    }

    running_.store(false);
}

void MjpegSource::cleanup() {
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    boundary_.clear();
    auth_header_.clear();
    info_sent_ = false;
    connected_.store(false);
}

} // namespace baichuan
