#include "http_client.h"
#include <httplib.h>
#include <vector>
#include <sstream>
#include <iostream>
#include <thread>
#include <algorithm>

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
        parts.is_https = true;
    }

    auto slash = u.find('/');
    std::string host_part;
    if (slash == std::string::npos) {
        host_part = u;
        parts.path = "/";
    } else {
        host_part = u.substr(0, slash);
        parts.path = u.substr(slash);
    }

    auto colon = host_part.find(':');
    if (colon != std::string::npos) {
        parts.port = std::stoi(host_part.substr(colon + 1));
        parts.host = host_part.substr(0, colon);
    } else {
        parts.host = host_part;
        parts.port = parts.is_https ? 443 : 80;
    }

    return parts;
}

// ─── StreamControl ──────────────────────────────────────────────────────────

void HttpClient::StreamControl::cancel() {
    std::lock_guard<std::mutex> lock(mtx_);
    cancelled_ = true;
}

bool HttpClient::StreamControl::is_cancelled() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return cancelled_;
}

// ─── Constructor / destructor ───────────────────────────────────────────────

HttpClient::HttpClient() = default;
HttpClient::~HttpClient() = default;

// ─── Helper: build httplib headers ─────────────────────────────────────────

static httplib::Headers build_headers(const std::map<std::string, std::string>& headers) {
    httplib::Headers h;
    for (const auto& [k, v] : headers) {
        h.emplace(k, v);
    }
    return h;
}

// ─── GET ────────────────────────────────────────────────────────────────────

HttpClient::Response HttpClient::get(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    int timeout_ms)
{
    auto parts = parse_url(url);
    Response resp;

    try {
        httplib::Client cli(parts.host, parts.port);
        cli.set_connection_timeout(std::chrono::milliseconds(timeout_ms));
        cli.set_read_timeout(std::chrono::milliseconds(timeout_ms));
        auto result = cli.Get(parts.path, build_headers(headers));
        if (result) {
            resp.status_code = result->status;
            resp.body = result->body;
        } else {
            resp.status_code = 503;
            std::cerr << "[http] GET failed: " << static_cast<int>(result.error()) << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[http] GET exception: " << e.what() << std::endl;
        resp.status_code = 503;
    }

    return resp;
}

// ─── POST (non-streaming) ──────────────────────────────────────────────────

HttpClient::Response HttpClient::post(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    const std::string& content_type,
    int timeout_ms)
{
    auto parts = parse_url(url);
    Response resp;

    try {
        httplib::Client cli(parts.host, parts.port);
        cli.set_connection_timeout(std::chrono::milliseconds(timeout_ms));
        cli.set_read_timeout(std::chrono::milliseconds(timeout_ms));
        auto result = cli.Post(parts.path, build_headers(headers), body, content_type);
        if (result) {
            resp.status_code = result->status;
            resp.body = result->body;
        } else {
            resp.status_code = 503;
            std::cerr << "[http] POST failed: " << static_cast<int>(result.error()) << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[http] POST exception: " << e.what() << std::endl;
        resp.status_code = 503;
    }

    return resp;
}

// ─── POST (streaming) ──────────────────────────────────────────────────────

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
    Response resp;

    auto notify_status = [&](int status_code) {
        if (status_cb) {
            status_cb(status_code);
        }
    };

    try {
        httplib::Client cli(parts.host, parts.port);
        cli.set_connection_timeout(std::chrono::milliseconds(timeout_ms));
        cli.set_read_timeout(std::chrono::milliseconds(timeout_ms));

        // Use ContentReceiver-based Post for streaming
        auto result = cli.Post(parts.path, build_headers(headers), body, content_type,
            [&](const char* data, size_t len) {
                if (stream_control && stream_control->is_cancelled()) {
                    return false;
                }
                if (chunk_cb) {
                    return chunk_cb(data, len);
                }
                resp.body.append(data, len);
                return true;
            }
        );

        if (result) {
            resp.status_code = result->status;
            notify_status(result->status);
        } else {
            resp.status_code = 503;
            notify_status(503);
            std::cerr << "[http] POST stream failed: " << static_cast<int>(result.error()) << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "[http] POST stream exception: " << e.what() << std::endl;
        resp.status_code = 503;
        notify_status(503);
    }

    return resp;
}
