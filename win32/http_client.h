#pragma once
#include <string>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <cstdint>
#include <windows.h>
#include <winhttp.h>

// RAII wrappers for WinHTTP handles
struct WinHttpDeleter {
    void operator()(HINTERNET h) const { if (h) WinHttpCloseHandle(h); }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpDeleter>;

// Thread-safe buffer for streaming data between threads
struct StreamBuffer {
    std::queue<std::string> chunks;
    std::mutex mtx;
    std::condition_variable cv;
    size_t buffered_bytes = 0;
    size_t max_buffered_bytes = 1024 * 1024;
    bool completed = false;
    bool error = false;
    bool cancelled = false;
    bool client_disconnected = false;
    bool monitor_finalized = false;
    uint64_t monitor_stream_id = 0;
    std::string error_msg;
    std::function<void()> cancel_upstream;
};

class HttpClient {
public:
    using StreamCallback = std::function<bool(const char* data, size_t len)>;
    using StatusCallback = std::function<void(int status_code)>;

    struct StreamControl {
        void attach(HINTERNET request_handle);
        HINTERNET detach();
        void cancel();
        bool is_cancelled() const;

    private:
        mutable std::mutex mtx_;
        HINTERNET request_handle_ = nullptr;
        bool cancelled_ = false;
    };

    struct Response {
        int status_code = 0;
        std::string body;
    };

    HttpClient();
    ~HttpClient();

    // Non-streaming POST — returns full response
    Response post(const std::string& url,
                  const std::map<std::string, std::string>& headers,
                  const std::string& body,
                  const std::string& content_type = "application/json",
                  int timeout_ms = 120000);

    // Streaming POST — calls chunk_cb as data arrives, returns final status
    // chunk_cb is called from the background thread
    Response postStream(const std::string& url,
                        const std::map<std::string, std::string>& headers,
                        const std::string& body,
                        const std::string& content_type,
                        StreamCallback chunk_cb,
                        int timeout_ms = 300000,
                        StatusCallback status_cb = nullptr,
                        std::shared_ptr<StreamControl> stream_control = nullptr);

    // GET request
    Response get(const std::string& url,
                 const std::map<std::string, std::string>& headers,
                 int timeout_ms = 30000);

    // Public utility methods
    static std::wstring utf8_to_wide(const std::string& s);
    static std::string wide_to_utf8(const std::wstring& s);

private:
    WinHttpHandle session_;

    // Parse URL into components
    struct UrlParts {
        std::wstring host;
        int port;
        std::wstring path;
        bool is_https;
    };
    static UrlParts parse_url(const std::string& url);

    // Internal: create WinHTTP request, send, read response
    Response sendRequest(const std::wstring& verb,
                         const std::wstring& host, int port,
                         const std::wstring& path, bool is_https,
                         const std::map<std::string, std::string>& headers,
                         const std::string* body,
                         const std::string* content_type,
                         StreamCallback chunk_cb,
                         StatusCallback status_cb,
                         std::shared_ptr<StreamControl> stream_control,
                         int timeout_ms);
};
