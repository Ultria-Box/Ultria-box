#include "http.h"
#include "sys.h"
#include "crypto.h"
#include <windows.h>
#include <winhttp.h>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif

namespace http {

namespace {

std::wstring host_of(const std::string& url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    std::wstring wurl = util::widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
        return {};
    return host;
}

std::wstring path_of(const std::string& url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 4095;
    std::wstring wurl = util::widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
        return {};
    std::wstring result = path;
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength)
        result.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    return result;
}

INTERNET_PORT port_of(const std::string& url) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2047;
    std::wstring wurl = util::widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc))
        return 0;
    return uc.nPort;
}

bool is_https(const std::string& url) { return util::starts_with(util::to_lower(url), "https://"); }

} // namespace

Response request(const std::string& verb, const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::vector<uint8_t>& body, const std::string& user_agent, int timeout_ms) {
    Response r;
    std::wstring whost = host_of(url), wpath = path_of(url), wverb = util::widen(verb), wua = util::widen(user_agent);
    if (whost.empty()) return r;
    bool https = is_https(url);

    HINTERNET session = WinHttpOpen(wua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return r;
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    HINTERNET conn = WinHttpConnect(session, whost.c_str(), port_of(url), 0);
    if (!conn) { WinHttpCloseHandle(session); return r; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    // WINHTTP_NO_REFERER (not the default accept types) stops WinHTTP from
    // adding its own "Content-Type: text/plain" when a body is posted.
    HINTERNET req = WinHttpOpenRequest(conn, wverb.c_str(), wpath.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_NO_REFERER, flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return r; }

    // Ignore cert issues on misconfigured hosts
    DWORD sec = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(req, WINHTTP_OPTION_SECURITY_FLAGS, &sec, sizeof(sec));

    std::wstring whdrs;
    for (auto& [k, v] : headers) whdrs += util::widen(k + ": " + v + "\r\n");

    BOOL sent = WinHttpSendRequest(req,
        whdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : whdrs.c_str(),
        whdrs.empty() ? 0 : (DWORD)whdrs.size(),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
        (DWORD)body.size(), (DWORD)body.size(), 0);

    if (sent && WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        r.status = (int)status;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            size_t off = r.body.size();
            r.body.resize(off + avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, r.body.data() + off, avail, &read)) { r.body.resize(off); break; }
            r.body.resize(off + read);
        }
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return r;
}

Form::Form() {
    auto rnd = crypto::random_bytes(12);
    static const char hexd[] = "0123456789abcdef";
    std::string token;
    for (uint8_t c : rnd) { token += hexd[c >> 4]; token += hexd[c & 0xF]; }
    // Looks like plain form boundaries to network inspection.
    boundary = "----WebKitFormBoundary" + token;
}

void Form::field(const std::string& name, const std::string& value) {
    std::string head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name + "\"\r\n\r\n";
    body.insert(body.end(), head.begin(), head.end());
    body.insert(body.end(), value.begin(), value.end());
    body.insert(body.end(), {'\r', '\n'});
}

void Form::file(const std::string& name, const std::string& filename,
                const std::vector<uint8_t>& data, const std::string& content_type) {
    std::string head = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name +
                       "\"; filename=\"" + filename + "\"\r\nContent-Type: " + content_type + "\r\n\r\n";
    body.insert(body.end(), head.begin(), head.end());
    body.insert(body.end(), data.begin(), data.end());
    body.insert(body.end(), {'\r', '\n'});
}

void Form::finish() {
    if (finished) return;
    std::string tail = "--" + boundary + "--\r\n";
    body.insert(body.end(), tail.begin(), tail.end());
    finished = true;
}

} // namespace http
