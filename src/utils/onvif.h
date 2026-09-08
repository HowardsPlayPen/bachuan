#pragma once

// Minimal best-effort ONVIF stream discovery.
//
// Queries a camera's ONVIF Media service (GetProfiles -> GetStreamUri) to obtain
// the streams it actually advertises, with their real RTSP URLs and resolution.
// This is the "authoritative" discovery path, but many cameras ship ONVIF
// disabled or locked down (EZVIZ commonly returns 403), so callers must treat
// failure as normal and fall back to other methods.

#include <string>
#include <vector>

namespace baichuan {

struct OnvifStream {
    std::string name;        // profile name/token
    std::string uri;         // rtsp:// URL (credentials injected by caller if needed)
    std::string resolution;  // "1920x1080" or empty
    std::string encoding;    // "H264"/"H265"/... or empty
};

struct OnvifResult {
    bool ok = false;                 // true if at least one profile/URI was retrieved
    std::string error;               // human-readable failure reason when !ok
    std::vector<OnvifStream> streams;
};

// Discover streams via ONVIF. host/user/pass come from the camera config; http_port
// is the ONVIF HTTP service port (usually 80). Blocking network call — run off the
// UI thread. timeout_seconds bounds each HTTP request.
OnvifResult onvif_discover(const std::string& host, int http_port,
                           const std::string& user, const std::string& pass,
                           int timeout_seconds = 5);

} // namespace baichuan
