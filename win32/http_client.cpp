#include "http_client.h"
#include <vector>
#include <sstream>
#include <iostream>
#include <thread>
#include <algorithm>

#pragma comment(lib, "winhttp.lib")

// WINHTTP_OPTION_STREAM_RESPONSE may not be defined in older MinGW headers
#ifndef WINHTTP_OPTION_STREAM_RESPONSE
#define WINHTTP_OPTION_STREAM_RESPONSE 156
#endif

// ─── Utf-8 / wide conversion ────────────────────────────────────────────────

std::wstring HttpClient::utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring buf(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &buf[0], len);
    return buf;
}

std::string HttpClient::wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string buf(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &buf[0], len, nullptr, nullptr);
    return buf;
}

// ─── URL parsing ────────────────────────────────────────────────────────────

HttpClient::UrlParts HttpClient::parse_url(const std::string& url) {
    UrlParts parts;
    parts.port = 0;
    parts.is_https = false;

    std::string u = url;
    if (u.find("https://") == 0) {
        parts.is_https = true;
        u = u.substr(8);
    } else if (u.find("http://") == 0) {
        u = u.substr(7);
    } else {
        // Default to HTTPS
        parts.is_https = true;
    }

    auto slash = u.find('/');
    std::string host_part;
    if (slash == std::string::npos) {
        host_part = u;
        parts.path = L"/";
    } else {
        host_part = u.substr(0, slash);
        parts.path = utf8_to_wide(u.substr(slash));
    }

    auto colon = host_part.find(':');
    if (colon != std::string::npos) {
        parts.port = std::stoi(host_part.substr(colon + 1));
        parts.host = utf8_to_wide(host_part.substr(0, colon));
    } else {
        parts.host = utf8_to_wide(host_part);
        parts.port = parts.is_https ? 443 : 80;
    }

    return parts;
}

// ─── Constructor / destructor ───────────────────────────────────────────────

HttpClient::HttpClient() {
    session_.reset(WinHttpOpen(L"Model-Gateway/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!session_) {
        std::cerr << "[http] WinHttpOpen failed: " << GetLastError() << std::endl;
    }
}

HttpClient::~HttpClient() = default;

void HttpClient::StreamControl::attach(HINTERNET request_handle) {
    HINTERNET handle_to_close = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        request_handle_ = request_handle;
        if (cancelled_) {
            handle_to_close = request_handle_;
            request_handle_ = nullptr;
        }
    }
    if (handle_to_close) {
        WinHttpCloseHandle(handle_to_close);
    }
}

HINTERNET HttpClient::StreamControl::detach() {
    std::lock_guard<std::mutex> lock(mtx_);
    HINTERNET handle = request_handle_;
    request_handle_ = nullptr;
    return handle;
}

void HttpClient::StreamControl::cancel() {
    HINTERNET handle_to_close = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cancelled_ = true;
        handle_to_close = request_handle_;
        request_handle_ = nullptr;
    }
    if (handle_to_close) {
        WinHttpCloseHandle(handle_to_close);
    }
}

bool HttpClient::StreamControl::is_cancelled() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return cancelled_;
}

// ─── Header helpers ─────────────────────────────────────────────────────────

static std::wstring build_header_string(
    const std::map<std::string, std::string>& headers,
    const std::string* content_type,
    size_t body_len)
{
    std::wstring hdrs;
    auto add = [&](const std::string& k, const std::string& v) {
        auto wk = HttpClient::utf8_to_wide(k);
        auto wv = HttpClient::utf8_to_wide(v);
        hdrs += wk + L": " + wv + L"\r\n";
    };
    for (auto& [k, v] : headers) add(k, v);
    if (content_type) {
        hdrs += L"Content-Type: " + HttpClient::utf8_to_wide(*content_type) + L"\r\n";
    }
    if (body_len > 0) {
        hdrs += L"Content-Length: " + std::to_wstring(body_len) + L"\r\n";
    }
    hdrs += L"\r\n"; // terminate headers
    return hdrs;
}

// ─── Core sendRequest ───────────────────────────────────────────────────────

HttpClient::Response HttpClient::sendRequest(
    const std::wstring& verb,
    const std::wstring& host, int port,
    const std::wstring& path, bool is_https,
    const std::map<std::string, std::string>& headers,
    const std::string* body,
    const std::string* content_type,
    StreamCallback chunk_cb,
    StatusCallback status_cb,
    std::shared_ptr<StreamControl> stream_control,
    int timeout_ms)
{
    Response resp;
    auto notify_status = [&](int status_code) {
        if (status_cb) {
            status_cb(status_code);
        }
    };

    if (!session_) {
        resp.status_code = 503;
        notify_status(resp.status_code);
        return resp;
    }

    WinHttpHandle connect(WinHttpConnect(session_.get(), host.c_str(), (INTERNET_PORT)port, 0));
    if (!connect) {
        std::cerr << "[http] WinHttpConnect failed: " << GetLastError() << std::endl;
        resp.status_code = 503;
        notify_status(resp.status_code);
        return resp;
    }

    DWORD flags = 0;
    if (is_https) flags |= WINHTTP_FLAG_SECURE;
    HINTERNET request = WinHttpOpenRequest(connect.get(), verb.c_str(), path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    auto close_request = [&]() {
        HINTERNET handle_to_close = request;
        if (stream_control) {
            handle_to_close = stream_control->detach();
        }
        request = nullptr;
        if (handle_to_close) {
            WinHttpCloseHandle(handle_to_close);
        }
    };

    if (!request) {
        std::cerr << "[http] WinHttpOpenRequest failed: " << GetLastError() << std::endl;
        resp.status_code = 503;
        notify_status(resp.status_code);
        return resp;
    }

    if (stream_control) {
        stream_control->attach(request);
        if (stream_control->is_cancelled()) {
            resp.status_code = 499;
            notify_status(resp.status_code);
            close_request();
            return resp;
        }
    }

    // Timeouts
    WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

    // Allow redirects
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                     &redirect_policy, sizeof(redirect_policy));

    // Enable streaming response if chunk_cb is set
    if (chunk_cb) {
        DWORD stream_response = 1;
        WinHttpSetOption(request, WINHTTP_OPTION_STREAM_RESPONSE,
                         &stream_response, sizeof(stream_response));
    }

    // Headers
    size_t body_len = body ? body->size() : 0;
    std::wstring hdr_str = build_header_string(headers, content_type, body_len);

    // Send
    void* body_data = nullptr;
    DWORD body_data_len = 0;
    if (body && !body->empty()) {
        body_data = (void*)body->data();
        body_data_len = (DWORD)body->size();
    }

    if (!WinHttpSendRequest(request, hdr_str.c_str(), (DWORD)-1L,
                            body_data, body_data_len, body_data_len, 0))
    {
        std::cerr << "[http] WinHttpSendRequest failed: " << GetLastError() << std::endl;
        resp.status_code = 503;
        notify_status(resp.status_code);
        close_request();
        return resp;
    }

    if (!WinHttpReceiveResponse(request, nullptr)) {
        DWORD err = GetLastError();
        std::cerr << "[http] WinHttpReceiveResponse failed: " << err << std::endl;
        resp.status_code = 503;
        notify_status(resp.status_code);
        close_request();
        return resp;
    }

    // Status code
    DWORD status_code = 0;
    DWORD sc_size = sizeof(status_code);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        nullptr, &status_code, &sc_size, nullptr);
    resp.status_code = (int)status_code;
    notify_status(resp.status_code);

    // Read body
    DWORD bytes_read = 0;
    char buf[16384];
    bool cb_ok = true;
    while (cb_ok && WinHttpReadData(request, buf, sizeof(buf), &bytes_read)) {
        if (bytes_read == 0) break;
        if (chunk_cb) {
            cb_ok = chunk_cb(buf, bytes_read);
        } else {
            resp.body.append(buf, bytes_read);
        }

        if (stream_control && stream_control->is_cancelled()) {
            cb_ok = false;
        }
    }

    close_request();

    return resp;
}

// ─── Public API ─────────────────────────────────────────────────────────────

HttpClient::Response HttpClient::post(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    const std::string& content_type,
    int timeout_ms)
{
    auto parts = parse_url(url);
    return sendRequest(L"POST", parts.host, parts.port, parts.path, parts.is_https,
                       headers, &body, &content_type, nullptr, nullptr, nullptr, timeout_ms);
}

HttpClient::Response HttpClient::postStream(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    const std::string& content_type,
    StreamCallback chunk_cb,
    int timeout_ms,
    StatusCallback status_cb,
    std::shared_ptr<StreamControl> stream_control)
{
    auto parts = parse_url(url);
    return sendRequest(L"POST", parts.host, parts.port, parts.path, parts.is_https,
                       headers, &body, &content_type, chunk_cb, status_cb, std::move(stream_control), timeout_ms);
}

HttpClient::Response HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    int timeout_ms)
{
    auto parts = parse_url(url);
    return sendRequest(L"GET", parts.host, parts.port, parts.path, parts.is_https,
                       headers, nullptr, nullptr, nullptr, nullptr, nullptr, timeout_ms);
}
