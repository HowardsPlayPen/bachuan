#include "protocol/bc_xml.h"
#include "utils/logger.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xmlwriter.h>
#include <sstream>
#include <cstring>
#include <memory>

namespace baichuan {

namespace {

// RAII wrapper for xmlDoc
struct XmlDocDeleter {
    void operator()(xmlDoc* doc) { if (doc) xmlFreeDoc(doc); }
};
using XmlDocPtr = std::unique_ptr<xmlDoc, XmlDocDeleter>;

// RAII wrapper for xmlTextWriter
struct XmlWriterDeleter {
    void operator()(xmlTextWriter* writer) { if (writer) xmlFreeTextWriter(writer); }
};
using XmlWriterPtr = std::unique_ptr<xmlTextWriter, XmlWriterDeleter>;

// RAII wrapper for xmlBuffer
struct XmlBufferDeleter {
    void operator()(xmlBuffer* buf) { if (buf) xmlBufferFree(buf); }
};
using XmlBufferPtr = std::unique_ptr<xmlBuffer, XmlBufferDeleter>;

// Helper to find a child element by name
xmlNode* find_child(xmlNode* parent, const char* name) {
    for (xmlNode* cur = parent->children; cur; cur = cur->next) {
        if (cur->type == XML_ELEMENT_NODE && xmlStrcmp(cur->name, BAD_CAST name) == 0) {
            return cur;
        }
    }
    return nullptr;
}

// Helper to get text content of an element
std::string get_content(xmlNode* node) {
    xmlChar* content = xmlNodeGetContent(node);
    if (!content) return "";
    std::string result(reinterpret_cast<char*>(content));
    xmlFree(content);
    return result;
}

// Helper to get attribute value
std::string get_attr(xmlNode* node, const char* name) {
    xmlChar* attr = xmlGetProp(node, BAD_CAST name);
    if (!attr) return "";
    std::string result(reinterpret_cast<char*>(attr));
    xmlFree(attr);
    return result;
}

} // anonymous namespace

// EncryptionXml implementation
std::optional<EncryptionXml> EncryptionXml::parse(const std::string& xml) {
    XmlDocPtr doc(xmlReadMemory(xml.c_str(), static_cast<int>(xml.size()),
                                nullptr, nullptr, XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
    if (!doc) {
        LOG_ERROR("Failed to parse encryption XML");
        return std::nullopt;
    }

    xmlNode* root = xmlDocGetRootElement(doc.get());
    if (!root) return std::nullopt;

    // Find Encryption element (could be root or child of body)
    xmlNode* enc_node = nullptr;
    if (xmlStrcmp(root->name, BAD_CAST "Encryption") == 0) {
        enc_node = root;
    } else {
        enc_node = find_child(root, "Encryption");
    }

    if (!enc_node) {
        LOG_DEBUG("No Encryption element found");
        return std::nullopt;
    }

    EncryptionXml result;
    result.version = get_attr(enc_node, "version");
    if (result.version.empty()) result.version = XML_VERSION;

    xmlNode* type_node = find_child(enc_node, "type");
    if (type_node) result.type = get_content(type_node);

    xmlNode* nonce_node = find_child(enc_node, "nonce");
    if (nonce_node) result.nonce = get_content(nonce_node);

    if (result.nonce.empty()) {
        LOG_ERROR("Encryption XML missing nonce");
        return std::nullopt;
    }

    LOG_DEBUG("Parsed encryption: type={}, nonce={}", result.type, result.nonce);
    return result;
}

// LoginUserXml implementation
std::string LoginUserXml::serialize() const {
    std::ostringstream oss;
    oss << "<LoginUser version=\"" << version << "\">"
        << "<userName>" << user_name << "</userName>"
        << "<password>" << password << "</password>"
        << "<userVer>" << user_ver << "</userVer>"
        << "</LoginUser>";
    return oss.str();
}

// LoginNetXml implementation
std::string LoginNetXml::serialize() const {
    std::ostringstream oss;
    oss << "<LoginNet version=\"" << version << "\">"
        << "<type>" << type << "</type>"
        << "<udpPort>" << udp_port << "</udpPort>"
        << "</LoginNet>";
    return oss.str();
}

// PreviewXml implementation
std::string PreviewXml::serialize() const {
    std::ostringstream oss;
    oss << "<Preview version=\"" << version << "\">"
        << "<channelId>" << static_cast<int>(channel_id) << "</channelId>"
        << "<handle>" << handle << "</handle>"
        << "<streamType>" << stream_type << "</streamType>"
        << "</Preview>";
    return oss.str();
}

// ExtensionXml implementation
std::string ExtensionXml::serialize() const {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        << "<Extension version=\"" << version << "\">";

    if (binary_data) {
        oss << "<binaryData>" << *binary_data << "</binaryData>";
    }
    if (user_name) {
        oss << "<userName>" << *user_name << "</userName>";
    }
    if (token) {
        oss << "<token>" << *token << "</token>";
    }
    if (channel_id) {
        oss << "<channelId>" << static_cast<int>(*channel_id) << "</channelId>";
    }
    if (encrypt_len) {
        oss << "<encryptLen>" << *encrypt_len << "</encryptLen>";
    }

    oss << "</Extension>";
    return oss.str();
}

std::optional<ExtensionXml> ExtensionXml::parse(const std::string& xml) {
    XmlDocPtr doc(xmlReadMemory(xml.c_str(), static_cast<int>(xml.size()),
                                nullptr, nullptr, XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
    if (!doc) return std::nullopt;

    xmlNode* root = xmlDocGetRootElement(doc.get());
    if (!root) return std::nullopt;

    xmlNode* ext_node = nullptr;
    if (xmlStrcmp(root->name, BAD_CAST "Extension") == 0) {
        ext_node = root;
    } else {
        ext_node = find_child(root, "Extension");
    }

    if (!ext_node) return std::nullopt;

    ExtensionXml result;
    result.version = get_attr(ext_node, "version");
    if (result.version.empty()) result.version = XML_VERSION;

    if (xmlNode* n = find_child(ext_node, "binaryData")) {
        result.binary_data = std::stoul(get_content(n));
    }
    if (xmlNode* n = find_child(ext_node, "userName")) {
        result.user_name = get_content(n);
    }
    if (xmlNode* n = find_child(ext_node, "token")) {
        result.token = get_content(n);
    }
    if (xmlNode* n = find_child(ext_node, "channelId")) {
        result.channel_id = static_cast<uint8_t>(std::stoul(get_content(n)));
    }
    if (xmlNode* n = find_child(ext_node, "encryptLen")) {
        result.encrypt_len = std::stoul(get_content(n));
    }

    return result;
}

// DeviceInfoXml implementation
std::optional<DeviceInfoXml> DeviceInfoXml::parse(const std::string& xml) {
    XmlDocPtr doc(xmlReadMemory(xml.c_str(), static_cast<int>(xml.size()),
                                nullptr, nullptr, XML_PARSE_NOERROR | XML_PARSE_NOWARNING));
    if (!doc) return std::nullopt;

    xmlNode* root = xmlDocGetRootElement(doc.get());
    if (!root) return std::nullopt;

    xmlNode* info_node = nullptr;
    if (xmlStrcmp(root->name, BAD_CAST "DeviceInfo") == 0) {
        info_node = root;
    } else {
        info_node = find_child(root, "DeviceInfo");
    }

    if (!info_node) return std::nullopt;

    DeviceInfoXml result;
    std::string ver = get_attr(info_node, "version");
    if (!ver.empty()) result.version = ver;

    if (xmlNode* res_node = find_child(info_node, "resolution")) {
        if (xmlNode* w = find_child(res_node, "width")) {
            result.resolution_width = std::stoul(get_content(w));
        }
        if (xmlNode* h = find_child(res_node, "height")) {
            result.resolution_height = std::stoul(get_content(h));
        }
    }

    return result;
}

// LoginRequestXml implementation
std::string LoginRequestXml::serialize() const {
    std::ostringstream oss;
    // Note: Space before ?> matches quick_xml format used by Rust neolink
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        << "<body>"
        << login_user.serialize()
        << login_net.serialize()
        << "</body>";
    return oss.str();
}

// BcXmlBuilder static methods
std::string BcXmlBuilder::create_login_request(const std::string& hashed_username,
                                                const std::string& hashed_password) {
    LoginRequestXml req;
    req.login_user.user_name = hashed_username;
    req.login_user.password = hashed_password;
    return req.serialize();
}

std::string BcXmlBuilder::create_preview_request(uint8_t channel_id,
                                                  uint32_t handle,
                                                  const std::string& stream_type) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
        << "<body>";

    PreviewXml preview;
    preview.channel_id = channel_id;
    preview.handle = handle;
    preview.stream_type = stream_type;
    oss << preview.serialize();

    oss << "</body>";
    return oss.str();
}

std::string BcXmlBuilder::create_binary_extension(uint8_t channel_id) {
    ExtensionXml ext;
    ext.binary_data = 1;
    ext.channel_id = channel_id;
    return ext.serialize();
}

std::optional<EncryptionXml> BcXmlBuilder::parse_encryption(const std::string& xml) {
    return EncryptionXml::parse(xml);
}

std::optional<DeviceInfoXml> BcXmlBuilder::parse_device_info(const std::string& xml) {
    return DeviceInfoXml::parse(xml);
}

std::optional<ExtensionXml> BcXmlBuilder::parse_extension(const std::string& xml) {
    return ExtensionXml::parse(xml);
}

std::optional<std::string> BcXmlBuilder::extract_tag(const std::string& xml, const std::string& tag) {
    // Simple regex-free extraction for basic cases
    std::string open_tag = "<" + tag;
    std::string close_tag = "</" + tag + ">";

    size_t start = xml.find(open_tag);
    if (start == std::string::npos) return std::nullopt;

    // Find the end of the opening tag
    size_t content_start = xml.find('>', start);
    if (content_start == std::string::npos) return std::nullopt;
    content_start++; // Skip the '>'

    size_t end = xml.find(close_tag, content_start);
    if (end == std::string::npos) return std::nullopt;

    return xml.substr(content_start, end - content_start);
}

} // namespace baichuan
