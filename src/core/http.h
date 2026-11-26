#pragma once
// WinHTTP client + multipart builder (Telegram / panel uploads).
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace http {

struct Response {
    int status = 0;
    std::string body;
    bool ok() const { return status >= 200 && status < 300; }
};

Response request(const std::string& verb, const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::vector<uint8_t>& body,
                 const std::string& user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
                 int timeout_ms = 30000);

inline Response get(const std::string& url) { return request("GET", url, {}, {}); }
inline Response post(const std::string& url, const std::vector<uint8_t>& body,
                     const std::string& content_type = "application/octet-stream") {
    return request("POST", url, {{"Content-Type", content_type}}, body);
}

// multipart/form-data
struct Form {
    std::string boundary;
    std::vector<uint8_t> body;
    bool finished = false;
    Form();
    void field(const std::string& name, const std::string& value);
    void file(const std::string& name, const std::string& filename,
              const std::vector<uint8_t>& data, const std::string& content_type = "application/octet-stream");
    // Closes the multipart body. Called automatically by content_type(), so a
    // form with several parts (payload_json + file) stays valid.
    void finish();
    std::string content_type() { finish(); return "multipart/form-data; boundary=" + boundary; }
};

} // namespace http
