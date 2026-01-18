#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace bachuan {

// Camera configuration from JSON
struct CameraConfig {
    std::string name;       // Display name
    std::string host;       // IP address
    uint16_t port = 9000;   // Port (default 9000)
    std::string username;   // Username
    std::string password;   // Password
    std::string encryption; // none, bc, aes
    std::string stream;     // main, sub, extern
    uint8_t channel = 0;    // Channel ID
};

// Dashboard configuration
struct DashboardConfig {
    std::vector<CameraConfig> cameras;
    int columns = 2;        // Grid columns
};

// Simple JSON parser for dashboard config
class JsonConfigParser {
public:
    static DashboardConfig parse(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open config file: " + filename);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string json = buffer.str();

        return parse_json(json);
    }

private:
    static DashboardConfig parse_json(const std::string& json) {
        DashboardConfig config;
        size_t pos = 0;

        // Find "columns" field
        size_t cols_pos = json.find("\"columns\"");
        if (cols_pos != std::string::npos) {
            config.columns = parse_int(json, cols_pos);
        }

        // Find "cameras" array
        size_t cameras_pos = json.find("\"cameras\"");
        if (cameras_pos == std::string::npos) {
            throw std::runtime_error("No 'cameras' array found in config");
        }

        // Find the array start
        size_t arr_start = json.find('[', cameras_pos);
        size_t arr_end = find_matching_bracket(json, arr_start);

        if (arr_start == std::string::npos || arr_end == std::string::npos) {
            throw std::runtime_error("Invalid cameras array");
        }

        // Parse each camera object
        pos = arr_start + 1;
        while (pos < arr_end) {
            size_t obj_start = json.find('{', pos);
            if (obj_start == std::string::npos || obj_start >= arr_end) break;

            size_t obj_end = find_matching_brace(json, obj_start);
            if (obj_end == std::string::npos) break;

            std::string obj_str = json.substr(obj_start, obj_end - obj_start + 1);
            config.cameras.push_back(parse_camera(obj_str));

            pos = obj_end + 1;
        }

        return config;
    }

    static CameraConfig parse_camera(const std::string& json) {
        CameraConfig cam;

        cam.name = parse_string(json, "name", "Camera");
        cam.host = parse_string(json, "host", "");
        cam.username = parse_string(json, "username", "admin");
        cam.password = parse_string(json, "password", "");
        cam.encryption = parse_string(json, "encryption", "aes");
        cam.stream = parse_string(json, "stream", "main");

        size_t port_pos = json.find("\"port\"");
        if (port_pos != std::string::npos) {
            cam.port = static_cast<uint16_t>(parse_int(json, port_pos));
        }

        size_t channel_pos = json.find("\"channel\"");
        if (channel_pos != std::string::npos) {
            cam.channel = static_cast<uint8_t>(parse_int(json, channel_pos));
        }

        if (cam.host.empty()) {
            throw std::runtime_error("Camera config missing 'host' field");
        }

        return cam;
    }

    static std::string parse_string(const std::string& json, const std::string& key, const std::string& default_val) {
        std::string search = "\"" + key + "\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return default_val;

        // Find the colon
        size_t colon = json.find(':', pos);
        if (colon == std::string::npos) return default_val;

        // Find opening quote
        size_t start = json.find('"', colon + 1);
        if (start == std::string::npos) return default_val;

        // Find closing quote
        size_t end = json.find('"', start + 1);
        if (end == std::string::npos) return default_val;

        return json.substr(start + 1, end - start - 1);
    }

    static int parse_int(const std::string& json, size_t key_pos) {
        size_t colon = json.find(':', key_pos);
        if (colon == std::string::npos) return 0;

        // Skip whitespace
        size_t start = colon + 1;
        while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) {
            start++;
        }

        // Parse number
        std::string num_str;
        while (start < json.size() && (isdigit(json[start]) || json[start] == '-')) {
            num_str += json[start++];
        }

        return num_str.empty() ? 0 : std::stoi(num_str);
    }

    static size_t find_matching_bracket(const std::string& json, size_t start) {
        if (start >= json.size() || json[start] != '[') return std::string::npos;

        int depth = 1;
        for (size_t i = start + 1; i < json.size(); i++) {
            if (json[i] == '[') depth++;
            else if (json[i] == ']') {
                depth--;
                if (depth == 0) return i;
            }
        }
        return std::string::npos;
    }

    static size_t find_matching_brace(const std::string& json, size_t start) {
        if (start >= json.size() || json[start] != '{') return std::string::npos;

        int depth = 1;
        for (size_t i = start + 1; i < json.size(); i++) {
            if (json[i] == '{') depth++;
            else if (json[i] == '}') {
                depth--;
                if (depth == 0) return i;
            }
        }
        return std::string::npos;
    }
};

} // namespace bachuan
