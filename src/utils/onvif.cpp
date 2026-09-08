#include "utils/onvif.h"
#include "utils/net_compat.h"
#include "utils/logger.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>
#include <ctime>
#include <sstream>

namespace baichuan {
namespace {

// ---- small crypto/encoding helpers -------------------------------------------------

std::string base64_encode(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? tbl[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? tbl[v & 0x3F] : '=');
    }
    return out;
}

std::string sha1_raw(const std::string& in) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), nullptr);
    EVP_DigestUpdate(ctx, in.data(), in.size());
    EVP_DigestFinal_ex(ctx, digest, &dlen);
    EVP_MD_CTX_free(ctx);
    return std::string(reinterpret_cast<char*>(digest), dlen);
}

std::string iso8601_utc() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

// WS-Security UsernameToken (PasswordDigest) header.
std::string wss_header(const std::string& user, const std::string& pass) {
    unsigned char nonce[16];
    if (RAND_bytes(nonce, sizeof(nonce)) != 1) {
        for (auto& b : nonce) b = static_cast<unsigned char>(std::rand());
    }
    std::string created = iso8601_utc();
    std::string nonce_str(reinterpret_cast<char*>(nonce), sizeof(nonce));
    std::string digest = base64_encode(
        reinterpret_cast<const unsigned char*>(sha1_raw(nonce_str + created + pass).data()), 20);
    std::string nonce64 = base64_encode(nonce, sizeof(nonce));
    std::ostringstream o;
    o << "<Security s:mustUnderstand=\"1\" "
         "xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
         "<UsernameToken><Username>" << user << "</Username>"
         "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">"
      << digest << "</Password>"
         "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">"
      << nonce64 << "</Nonce>"
         "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">"
      << created << "</Created></UsernameToken></Security>";
    return o.str();
}

std::string soap_envelope(const std::string& user, const std::string& pass,
                          const std::string& body) {
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
         "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
         "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" "
         "xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
         "xmlns:tt=\"http://www.onvif.org/ver10/schema\">"
         "<s:Header>" << wss_header(user, pass) << "</s:Header>"
         "<s:Body>" << body << "</s:Body></s:Envelope>";
    return o.str();
}

// ---- minimal HTTP POST over a socket ----------------------------------------------

struct HttpResponse { int status = -1; std::string body; };

HttpResponse http_post(const std::string& host, int port, const std::string& path,
                       const std::string& body, int timeout_s) {
    HttpResponse r;
    net::socket_t fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd == net::kInvalidSocket) { r.body = "socket() failed"; return r; }
    net::set_recv_timeout(fd, timeout_s * 1000);
    net::set_send_timeout(fd, timeout_s * 1000);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        net::close_socket(fd); r.body = "bad host"; return r;
    }
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        net::close_socket(fd); r.body = "connect failed"; return r;
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << "\r\n"
        << "Content-Type: application/soap+xml; charset=utf-8\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    std::string reqs = req.str();
    size_t sent = 0;
    while (sent < reqs.size()) {
        int n = ::send(fd, reqs.data() + sent, static_cast<int>(reqs.size() - sent), MSG_NOSIGNAL);
        if (n <= 0) { net::close_socket(fd); r.body = "send failed"; return r; }
        sent += static_cast<size_t>(n);
    }

    std::string resp;
    char buf[4096];
    for (;;) {
        int n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, static_cast<size_t>(n));
        if (resp.size() > 1 << 20) break;  // 1 MB cap
    }
    net::close_socket(fd);

    // Parse status line + split headers/body.
    if (resp.rfind("HTTP/", 0) == 0) {
        size_t sp = resp.find(' ');
        if (sp != std::string::npos) r.status = std::atoi(resp.c_str() + sp + 1);
    }
    size_t hdr_end = resp.find("\r\n\r\n");
    r.body = (hdr_end != std::string::npos) ? resp.substr(hdr_end + 4) : resp;
    return r;
}

// ---- tiny XML text extraction (namespace-prefix tolerant) --------------------------

// Returns the text of the first <...Tag> element (prefix optional), or "".
std::string tag_text(const std::string& xml, const std::string& tag, size_t from = 0) {
    std::string open = tag + ">";
    size_t p = xml.find(open, from);
    while (p != std::string::npos) {
        // The char immediately before the tag name must be '<' or a namespace
        // ':' separator, so "Uri" does not also match "MediaUri" / "GetStreamUri".
        char before = (p > 0) ? xml[p - 1] : '<';
        if (before == '<' || before == ':') {
            size_t start = p + open.size();
            size_t end = xml.find('<', start);
            if (end != std::string::npos) return xml.substr(start, end - start);
        }
        p = xml.find(open, p + open.size());
    }
    return "";
}

// The element marker for a media profile: <trt:Profiles token="...">. Keying off
// this (rather than any token=) avoids matching nested configuration tokens.
static const char kProfileMarker[] = "Profiles token=\"";

std::vector<std::string> profile_tokens(const std::string& xml) {
    std::vector<std::string> out;
    size_t p = 0;
    const std::string marker = kProfileMarker;
    while ((p = xml.find(marker, p)) != std::string::npos) {
        size_t s = p + marker.size();
        size_t e = xml.find('"', s);
        if (e != std::string::npos) out.push_back(xml.substr(s, e - s));
        p = s;
    }
    return out;
}

// Decode the handful of XML entities that appear in ONVIF stream URIs.
std::string xml_unescape(std::string v) {
    struct { const char* ent; char ch; } tbl[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}
    };
    for (auto& e : tbl) {
        std::string ent = e.ent;
        size_t pos = 0;
        while ((pos = v.find(ent, pos)) != std::string::npos) {
            v.replace(pos, ent.size(), 1, e.ch);
            pos += 1;
        }
    }
    return v;
}

std::string strip_host(const std::string& url) {
    // http://host:port/path -> /path
    size_t s = url.find("://");
    if (s == std::string::npos) return url;
    size_t slash = url.find('/', s + 3);
    return (slash == std::string::npos) ? "/" : url.substr(slash);
}

// Inject user:pass into an rtsp URL that lacks credentials.
std::string inject_creds(const std::string& uri, const std::string& user, const std::string& pass) {
    if (uri.rfind("rtsp://", 0) != 0 || uri.find('@') != std::string::npos || user.empty())
        return uri;
    return "rtsp://" + user + ":" + pass + "@" + uri.substr(7);
}

} // namespace

OnvifResult onvif_discover(const std::string& host, int http_port,
                           const std::string& user, const std::string& pass,
                           int timeout_seconds) {
    OnvifResult res;
    const std::string dev_path = "/onvif/device_service";

    // 1) GetCapabilities -> media service path (fall back to common defaults).
    std::string media_path = "/onvif/Media";
    {
        HttpResponse r = http_post(host, http_port, dev_path,
            soap_envelope(user, pass,
                "<tds:GetCapabilities><tds:Category>All</tds:Category></tds:GetCapabilities>"),
            timeout_seconds);
        if (r.status < 0) {
            res.error = "ONVIF unreachable on port " + std::to_string(http_port) +
                        " (" + r.body + ")";
            return res;
        }
        // Look for the Media capability's XAddr.
        size_t m = r.body.find("Media>");
        if (m != std::string::npos) {
            std::string xaddr = tag_text(r.body, "XAddr", m);
            if (!xaddr.empty()) media_path = strip_host(xaddr);
        }
    }

    // 2) GetProfiles -> tokens + resolution + encoding.
    HttpResponse pr = http_post(host, http_port, media_path,
        soap_envelope(user, pass, "<trt:GetProfiles/>"), timeout_seconds);
    if (pr.status != 200) {
        std::string reason = tag_text(pr.body, "Text");
        if (reason.empty()) reason = tag_text(pr.body, "faultstring");
        res.error = "GetProfiles failed (HTTP " + std::to_string(pr.status) + ")" +
                    (reason.empty() ? "" : ": " + reason);
        return res;
    }

    std::vector<std::string> tokens = profile_tokens(pr.body);
    if (tokens.empty()) {
        res.error = "No ONVIF profiles returned.";
        return res;
    }

    // Parse per-profile resolution/encoding by slicing the response at each profile.
    auto profile_slice = [&](const std::string& token) -> std::string {
        std::string marker = std::string(kProfileMarker) + token + "\"";
        size_t t = pr.body.find(marker);
        if (t == std::string::npos) return "";
        // Up to the start of the NEXT profile element (or end of document).
        size_t next = pr.body.find(kProfileMarker, t + marker.size());
        return pr.body.substr(t, (next == std::string::npos ? std::string::npos : next - t));
    };

    // 3) GetStreamUri per profile.
    for (const auto& token : tokens) {
        OnvifStream st;
        st.name = token;
        std::string slice = profile_slice(token);
        std::string w = tag_text(slice, "Width");
        std::string h = tag_text(slice, "Height");
        if (!w.empty() && !h.empty()) st.resolution = w + "x" + h;
        st.encoding = tag_text(slice, "Encoding");

        HttpResponse ur = http_post(host, http_port, media_path,
            soap_envelope(user, pass,
                "<trt:GetStreamUri><trt:StreamSetup>"
                "<tt:Stream>RTP-Unicast</tt:Stream>"
                "<tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport>"
                "</trt:StreamSetup><trt:ProfileToken>" + token +
                "</trt:ProfileToken></trt:GetStreamUri>"),
            timeout_seconds);
        std::string uri = xml_unescape(tag_text(ur.body, "Uri"));
        st.uri = inject_creds(uri, user, pass);
        res.streams.push_back(std::move(st));
    }

    res.ok = !res.streams.empty();
    if (!res.ok) res.error = "ONVIF returned profiles but no stream URIs.";
    return res;
}

} // namespace baichuan
