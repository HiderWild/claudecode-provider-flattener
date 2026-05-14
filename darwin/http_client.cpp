#include "http_client.h"
#include <httplib.h>
#include <vector>
#include <sstream>
#include <iostream>
#include <thread>
#include <algorithm>
#include <memory>

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

void HttpClient::StreamControl::set_cancel_handler(std::function<void()> handler) {
    std::function<void()> cancel_handler;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cancel_handler_ = std::move(handler);
        if (cancelled_) {
            cancel_handler = cancel_handler_;
        }
    }
    if (cancel_handler) {
        cancel_handler();
    }
}

void HttpClient::StreamControl::clear_cancel_handler() {
    std::lock_guard<std::mutex> lock(mtx_);
    cancel_handler_ = nullptr;
}

void HttpClient::StreamControl::cancel() {
    std::function<void()> cancel_handler;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        cancelled_ = true;
        cancel_handler = cancel_handler_;
    }
    if (cancel_handler) {
        cancel_handler();
    }
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

static std::shared_ptr<httplib::ClientImpl> make_client(const std::string& host, int port, bool is_https) {
    if (is_https) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        return std::make_shared<httplib::SSLClient>(host, port);
#else
        return nullptr;
#endif
    }
    return std::make_shared<httplib::ClientImpl>(host, port);
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
        auto cli = make_client(parts.host, parts.port, parts.is_https);
        if (!cli) {
            resp.status_code = 503;
            std::cerr << "[http] GET HTTPS requested without SSL support" << std::endl;
            return resp;
        }
        cli->set_connection_timeout(std::chrono::milliseconds(timeout_ms));
        cli->set_read_timeout(std::chrono::milliseconds(timeout_ms));
        auto result = cli->Get(parts.path, build_headers(headers));
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
        auto cli = make_client(parts.host, parts.port, parts.is_https);
        if (!cli) {
            resp.status_code = 503;
            std::cerr << "[http] POST HTTPS requested without SSL support" << std::endl;
            return resp;
        }
        cli->set_connection_timeout(std::chrono::milliseconds(timeout_ms));
        cli->set_read_timeout(std::chrono::milliseconds(timeout_ms));
        auto result = cli->Post(parts.path, build_headers(headers), body, content_type);
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
        auto cli = make_client(parts.host, parts.port, parts.is_https);
        if (!cli) {
            resp.status_code = 503;
            notify_status(503);
            std::cerr << "[http] POST stream HTTPS requested without SSL support" << std::endl;
            return resp;
        }
        cli->set_connection_timeout(std::chrono::milliseconds(timeout_ms));
        cli->set_read_timeout(std::chrono::milliseconds(timeout_ms));

        if (stream_control) {
            std::weak_ptr<httplib::ClientImpl> weak_cli = cli;
            stream_control->set_cancel_handler([weak_cli]() {
                if (auto strong_cli = weak_cli.lock()) {
                    strong_cli->stop();
                }
            });
        }

        auto clear_cancel_handler = [&]() {
            if (stream_control) {
                stream_control->clear_cancel_handler();
            }
        };

        httplib::Request req;
        req.method = "POST";
        req.path = parts.path;
        req.headers = build_headers(headers);
        if (!content_type.empty() && req.headers.find("Content-Type") == req.headers.end()) {
            req.headers.emplace("Content-Type", content_type);
        }
        req.body = body;
        req.response_handler = [&](const httplib::Response& response) {
            resp.status_code = response.status;
            notify_status(response.status);
            return !(stream_control && stream_control->is_cancelled());
        };
        req.content_receiver = [&](const char* data, size_t data_length, size_t /*offset*/, size_t /*total_length*/) {
            if (stream_control && stream_control->is_cancelled()) {
                return false;
            }
            if (chunk_cb) {
                return chunk_cb(data, data_length);
            }
            resp.body.append(data, data_length);
            return true;
        };

        httplib::Response upstream_response;
        httplib::Error error = httplib::Error::Success;
        const bool ok = cli->send(req, upstream_response, error);
        clear_cancel_handler();

        if (!ok) {
            if (resp.status_code == 0) {
                resp.status_code = 503;
                notify_status(503);
            }
            const bool cancelled = stream_control && stream_control->is_cancelled();
            if (!cancelled) {
                std::cerr << "[http] POST stream failed: " << static_cast<int>(error) << std::endl;
            }
            return resp;
        }

        if (resp.status_code == 0) {
            resp.status_code = upstream_response.status;
            notify_status(resp.status_code);
        }
    } catch (const std::exception& e) {
        std::cerr << "[http] POST stream exception: " << e.what() << std::endl;
        resp.status_code = 503;
        notify_status(503);
    }

    return resp;
}
