#include <iostream>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <vector>
#include <thread>
#include <chrono>
#include <cstdio>
// httplib must be included before windows.h on Windows (it handles winsock2.h order)
#include <httplib.h>
#include <windows.h>
#include <tlhelp32.h>
#include "config.h"
#include "providers.h"
#include "http_client.h"
#include "server_thread_pool.h"
#include "webui.h"

// ─── Forward declarations ──────────────────────────────────────────────────
static void handle_list_models(const httplib::Request&, httplib::Response& res);
static void handle_messages(const httplib::Request& req, httplib::Response& res);
static void handle_count_tokens(const httplib::Request& req, httplib::Response& res);
static void handle_webui(const httplib::Request&, httplib::Response& res);
static void handle_get_config(const httplib::Request&, httplib::Response& res);
static void handle_get_monitor(const httplib::Request&, httplib::Response& res);
static void handle_post_config(const httplib::Request& req, httplib::Response& res);
static void handle_test_provider(const httplib::Request& req, httplib::Response& res);
static void log_debug(const std::string& message, bool show_console);
static void log_info(const std::string& message, bool show_console);
static void log_warn(const std::string& message, bool show_console);
static void log_error(const std::string& message, bool show_console);

static Config g_config;
static std::mutex g_config_mutex;
static int g_effective_thread_pool_size = ServerThreadPool::kDefaultThreadCount;
static std::mutex g_runtime_log_mutex;
static bool g_console_visible = false;
static HANDLE g_instance_lock_handle = INVALID_HANDLE_VALUE;
static std::wstring g_instance_lock_path;
static httplib::Server* g_console_control_server = nullptr;
static std::atomic<DWORD> g_console_shutdown_event{0};

struct MonitorCounters {
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> total_stream_requests{0};
    std::atomic<uint64_t> total_nonstream_requests{0};
    std::atomic<uint64_t> total_count_token_requests{0};
    std::atomic<uint64_t> total_request_errors{0};
    std::atomic<uint64_t> total_stream_completions{0};
    std::atomic<uint64_t> total_stream_cancellations{0};
    std::atomic<uint64_t> total_stream_disconnects{0};
    std::atomic<uint64_t> total_config_saves{0};
    std::atomic<uint64_t> total_provider_tests{0};
};

struct ActiveStreamMonitor {
    uint64_t id = 0;
    std::string model;
    std::string session_id;
    std::chrono::system_clock::time_point started_at;
    std::shared_ptr<StreamBuffer> buffer;
};

struct SessionMonitor {
    std::string id;
    std::string last_model;
    std::chrono::system_clock::time_point first_seen;
    std::chrono::system_clock::time_point last_seen;
    uint64_t total_requests = 0;
    uint64_t total_stream_requests = 0;
    uint64_t total_nonstream_requests = 0;
    uint64_t total_count_token_requests = 0;
    uint64_t total_errors = 0;
    uint64_t total_stream_completions = 0;
    uint64_t total_stream_cancellations = 0;
    uint64_t total_stream_disconnects = 0;
    size_t active_streams = 0;
};

static MonitorCounters g_monitor_counters;
static std::atomic<uint64_t> g_next_stream_id{1};
static std::mutex g_active_streams_mutex;
static std::map<uint64_t, ActiveStreamMonitor> g_active_streams;
static std::mutex g_sessions_mutex;
static std::map<std::string, SessionMonitor> g_sessions;
static const auto g_monitor_started_wall = std::chrono::system_clock::now();
static const auto g_monitor_started_steady = std::chrono::steady_clock::now();

struct StartupOptions {
    bool show_console = false;
    bool show_help = false;
    bool has_port_override = false;
    int port_override = 0;
};

static std::wstring get_executable_path_w() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (len == 0) {
        return L"";
    }
    path.resize(len);
    return path;
}

static std::wstring get_runtime_directory_w() {
    std::wstring path = get_executable_path_w();
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

static std::string get_runtime_log_path() {
    return HttpClient::wide_to_utf8(get_runtime_directory_w()) + "/model-gateway.log";
}

static const char* console_shutdown_event_name(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
            return "CTRL_C_EVENT";
        case CTRL_BREAK_EVENT:
            return "CTRL_BREAK_EVENT";
        case CTRL_CLOSE_EVENT:
            return "CTRL_CLOSE_EVENT";
        case CTRL_LOGOFF_EVENT:
            return "CTRL_LOGOFF_EVENT";
        case CTRL_SHUTDOWN_EVENT:
            return "CTRL_SHUTDOWN_EVENT";
        default:
            return "UNKNOWN_CONSOLE_EVENT";
    }
}

static BOOL WINAPI handle_console_shutdown(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            g_console_shutdown_event.store(ctrl_type, std::memory_order_relaxed);
            if (g_console_control_server) {
                g_console_control_server->stop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

static void detach_from_console_if_needed(bool show_console) {
    if (show_console) {
        return;
    }

    FreeConsole();

    FILE* ignored = nullptr;
    freopen_s(&ignored, "NUL", "r", stdin);
    freopen_s(&ignored, "NUL", "w", stdout);
    freopen_s(&ignored, "NUL", "w", stderr);
}

static std::string get_home_directory() {
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    return home ? home : "";
}

static std::string normalize_path_separators(std::string path) {
    for (char& ch : path) {
        if (ch == '\\') ch = '/';
    }
    return path;
}

static std::string redact_local_path(const std::string& path) {
    if (path.empty()) {
        return path;
    }

    const std::string normalized_path = normalize_path_separators(path);
    const std::string home_dir = normalize_path_separators(get_home_directory());
    if (!home_dir.empty() && normalized_path.rfind(home_dir, 0) == 0) {
        return "~" + normalized_path.substr(home_dir.size());
    }

    return normalized_path;
}

static std::wstring get_instance_lock_path_w() {
    std::wstring path = HttpClient::utf8_to_wide(Config::getConfigDir() + "/model-gateway.lock");
    std::replace(path.begin(), path.end(), L'/', L'\\');
    return path;
}

static bool is_invalid_set_file_pointer(DWORD result) {
    return result == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR;
}

static bool acquire_instance_lock(std::string& error_message) {
    try {
        std::filesystem::create_directories(Config::getConfigDir());
    } catch (const std::exception& e) {
        error_message = "failed to prepare runtime directory: " + std::string(e.what());
        return false;
    }

    const std::wstring lock_path = get_instance_lock_path_w();
    HANDLE lock_handle = CreateFileW(lock_path.c_str(),
                                     GENERIC_READ | GENERIC_WRITE,
                                     0,
                                     nullptr,
                                     OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL,
                                     nullptr);
    if (lock_handle == INVALID_HANDLE_VALUE) {
        const DWORD error_code = GetLastError();
        if (error_code == ERROR_SHARING_VIOLATION || error_code == ERROR_LOCK_VIOLATION) {
            error_message = "another gateway instance is already running";
            return false;
        }

        error_message = "CreateFileW failed with code " + std::to_string(error_code);
        return false;
    }

    SetLastError(NO_ERROR);
    if (is_invalid_set_file_pointer(SetFilePointer(lock_handle, 0, nullptr, FILE_BEGIN))) {
        error_message = "failed to seek lock file with code " + std::to_string(GetLastError());
        CloseHandle(lock_handle);
        DeleteFileW(lock_path.c_str());
        return false;
    }

    if (!SetEndOfFile(lock_handle)) {
        error_message = "failed to truncate lock file with code " + std::to_string(GetLastError());
        CloseHandle(lock_handle);
        DeleteFileW(lock_path.c_str());
        return false;
    }

    const std::string pid_text = std::to_string(GetCurrentProcessId()) + "\r\n";
    DWORD bytes_written = 0;
    if (!WriteFile(lock_handle,
                   pid_text.data(),
                   static_cast<DWORD>(pid_text.size()),
                   &bytes_written,
                   nullptr) ||
        bytes_written != pid_text.size()) {
        error_message = "failed to write lock file with code " + std::to_string(GetLastError());
        CloseHandle(lock_handle);
        DeleteFileW(lock_path.c_str());
        return false;
    }

    g_instance_lock_handle = lock_handle;
    g_instance_lock_path = lock_path;
    return true;
}

static void release_instance_lock() {
    if (g_instance_lock_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_instance_lock_handle);
        g_instance_lock_handle = INVALID_HANDLE_VALUE;
    }

    if (!g_instance_lock_path.empty()) {
        DeleteFileW(g_instance_lock_path.c_str());
        g_instance_lock_path.clear();
    }
}

static GatewayRequestContext extract_request_context(const httplib::Request& req) {
    GatewayRequestContext context;

    auto add_header = [&](const std::string& header_name, const std::string& forwarded_name = std::string()) {
        if (!req.has_header(header_name)) {
            return;
        }
        const std::string value = req.get_header_value(header_name);
        if (value.empty()) {
            return;
        }
        context.forwarded_headers[forwarded_name.empty() ? header_name : forwarded_name] = value;
    };

    add_header("anthropic-beta");
    add_header("anthropic-version");
    add_header("x-claude-code-session-id", "X-Claude-Code-Session-Id");

    auto it = context.forwarded_headers.find("X-Claude-Code-Session-Id");
    if (it != context.forwarded_headers.end()) {
        context.session_id = it->second;
    }

    return context;
}

static void log_session_event(const std::string& session_id, const std::string& message) {
    if (session_id.empty()) {
        return;
    }
    log_info("[session " + session_id + "] " + message, false);
}

static SessionMonitor& ensure_session_monitor_locked(const std::string& session_id,
                                                     const std::string& model,
                                                     const std::chrono::system_clock::time_point& now) {
    SessionMonitor& session = g_sessions[session_id];
    if (session.id.empty()) {
        session.id = session_id;
        session.first_seen = now;
    }
    session.last_seen = now;
    if (!model.empty()) {
        session.last_model = model;
    }
    return session;
}

static void note_session_request(const std::string& session_id,
                                 const std::string& model,
                                 bool stream,
                                 bool count_tokens = false) {
    if (session_id.empty()) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    SessionMonitor& session = ensure_session_monitor_locked(session_id, model, now);
    if (count_tokens) {
        ++session.total_count_token_requests;
        return;
    }

    ++session.total_requests;
    if (stream) {
        ++session.total_stream_requests;
    } else {
        ++session.total_nonstream_requests;
    }
}

static void note_session_error(const std::string& session_id, const std::string& model) {
    if (session_id.empty()) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    SessionMonitor& session = ensure_session_monitor_locked(session_id, model, now);
    ++session.total_errors;
}

static void note_session_stream_registered(const std::string& session_id, const std::string& model) {
    if (session_id.empty()) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    SessionMonitor& session = ensure_session_monitor_locked(session_id, model, now);
    ++session.active_streams;
}

static void note_session_stream_finalized(const std::string& session_id,
                                          const std::string& model,
                                          bool error,
                                          bool cancelled,
                                          bool client_disconnected) {
    if (session_id.empty()) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    std::lock_guard<std::mutex> lock(g_sessions_mutex);
    SessionMonitor& session = ensure_session_monitor_locked(session_id, model, now);
    if (session.active_streams > 0) {
        --session.active_streams;
    }
    if (error) {
        ++session.total_errors;
        return;
    }
    if (cancelled) {
        ++session.total_stream_cancellations;
        if (client_disconnected) {
            ++session.total_stream_disconnects;
        }
        return;
    }
    ++session.total_stream_completions;
}

static void append_runtime_log(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_runtime_log_mutex);
    std::ofstream log_file(get_runtime_log_path(), std::ios::app);
    if (!log_file.good()) {
        return;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    log_file << std::setfill('0')
             << std::setw(4) << now.wYear << '-'
             << std::setw(2) << now.wMonth << '-'
             << std::setw(2) << now.wDay << ' '
             << std::setw(2) << now.wHour << ':'
             << std::setw(2) << now.wMinute << ':'
             << std::setw(2) << now.wSecond << '.'
             << std::setw(3) << now.wMilliseconds
             << " [" << level << "]"
             << " [pid " << GetCurrentProcessId() << "] "
             << message << std::endl;
}

static void log_info(const std::string& message, bool show_console) {
    append_runtime_log("INFO", message);
    if (show_console) {
        std::cout << message << std::endl;
    }
}

static void log_debug(const std::string& message, bool show_console) {
    append_runtime_log("DEBUG", message);
    if (show_console) {
        std::cout << message << std::endl;
    }
}

void gateway_log_debug(const std::string& message) {
    log_debug(message, false);
}

static void log_warn(const std::string& message, bool show_console) {
    append_runtime_log("WARN", message);
    if (show_console) {
        std::cerr << message << std::endl;
    }
}

static void log_error(const std::string& message, bool show_console) {
    append_runtime_log("ERROR", message);
    if (show_console) {
        std::cerr << message << std::endl;
    }
}

static void print_usage() {
    std::cout << "Usage: model-gateway.exe [--show] [port]" << std::endl;
    std::cout << "  --show    Print startup details in the current console." << std::endl;
    std::cout << "  port      Optional port override for this launch." << std::endl;
}

struct DebugContentSummary {
    size_t char_count = 0;
    bool empty = true;
    std::string preview;
};

static void append_preview_text(const std::string& source,
                                std::string& out,
                                size_t max_chars) {
    bool previous_space = !out.empty() && out.back() == ' ';
    for (char ch : source) {
        if (out.size() >= max_chars) {
            break;
        }

        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::iscntrl(uch)) {
            if (!previous_space && !out.empty()) {
                out.push_back(' ');
                previous_space = true;
            }
            continue;
        }

        if (std::isspace(uch)) {
            if (previous_space || out.empty()) {
                continue;
            }
            out.push_back(' ');
            previous_space = true;
            continue;
        }

        out.push_back(ch);
        previous_space = false;
    }
}

static size_t debug_content_char_count(const json& value) {
    if (value.is_null()) {
        return 0;
    }
    if (value.is_string()) {
        return value.get<std::string>().size();
    }
    if (value.is_array()) {
        size_t total = 0;
        for (const auto& item : value) {
            total += debug_content_char_count(item);
        }
        return total;
    }
    if (value.is_object()) {
        auto text_it = value.find("text");
        if (text_it != value.end() && text_it->is_string()) {
            return text_it->get<std::string>().size();
        }
        auto content_it = value.find("content");
        if (content_it != value.end()) {
            return debug_content_char_count(*content_it);
        }
        return value.dump().size();
    }
    return value.dump().size();
}

static void append_debug_content_preview(const json& value,
                                         std::string& out,
                                         size_t max_chars) {
    if (out.size() >= max_chars || value.is_null()) {
        return;
    }

    if (value.is_string()) {
        append_preview_text(value.get<std::string>(), out, max_chars);
        return;
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            append_debug_content_preview(item, out, max_chars);
            if (out.size() >= max_chars) {
                break;
            }
        }
        return;
    }

    if (value.is_object()) {
        auto text_it = value.find("text");
        if (text_it != value.end() && text_it->is_string()) {
            append_preview_text(text_it->get<std::string>(), out, max_chars);
            return;
        }
        auto content_it = value.find("content");
        if (content_it != value.end()) {
            append_debug_content_preview(*content_it, out, max_chars);
            return;
        }
        append_preview_text(value.dump(), out, max_chars);
        return;
    }

    append_preview_text(value.dump(), out, max_chars);
}

static std::string escape_debug_preview(const std::string& preview) {
    std::string escaped;
    escaped.reserve(preview.size());
    for (char ch : preview) {
        if (ch == '\\') {
            escaped += "\\\\";
        } else if (ch == '"') {
            escaped += "\\\"";
        } else {
            escaped.push_back(ch);
        }
    }
    return escaped;
}

static DebugContentSummary summarize_debug_content(const json& value,
                                                  size_t preview_limit = 96) {
    DebugContentSummary summary;
    summary.char_count = debug_content_char_count(value);
    summary.empty = summary.char_count == 0;
    append_debug_content_preview(value, summary.preview, preview_limit);
    if (summary.preview.size() == preview_limit && summary.char_count > summary.preview.size() &&
        preview_limit > 3) {
        summary.preview = summary.preview.substr(0, preview_limit - 3) + "...";
    }
    summary.preview = escape_debug_preview(summary.preview);
    return summary;
}

static std::string summarize_message_block(const json& block,
                                           std::map<std::string, std::string>& tool_names_by_id,
                                           std::vector<std::string>& debug_lines) {
    if (block.is_string()) {
        return "text:" + std::to_string(block.get<std::string>().size());
    }

    if (!block.is_object()) {
        return "json:" + std::to_string(debug_content_char_count(block));
    }

    const std::string type = block.value("type", "object");
    if (type == "text") {
        return "text:" + std::to_string(block.value("text", std::string()).size());
    }

    if (type == "tool_use") {
        const std::string tool_id = block.value("id", std::string());
        const std::string tool_name = block.value("name", std::string());
        if (!tool_id.empty() && !tool_name.empty()) {
            tool_names_by_id[tool_id] = tool_name;
        }
        if (!tool_name.empty()) {
            return "tool_use:" + tool_name;
        }
        if (!tool_id.empty()) {
            return "tool_use:" + tool_id;
        }
        return "tool_use";
    }

    if (type == "tool_result") {
        const std::string tool_id = block.value("tool_use_id", std::string());
        const auto tool_it = tool_names_by_id.find(tool_id);
        const std::string tool_label = tool_it != tool_names_by_id.end()
            ? tool_it->second
            : (!tool_id.empty() ? tool_id : "unknown");
        const json content = block.contains("content") ? block["content"] : json();
        const DebugContentSummary summary = summarize_debug_content(content, 96);
        debug_lines.push_back(
            "[debug] tool_result id=" + (tool_id.empty() ? std::string("<missing>") : tool_id) +
            " chars=" + std::to_string(summary.char_count) +
            " empty=" + (summary.empty ? "true" : "false") +
            " preview=\"" + summary.preview + "\"");
        return "tool_result:" + tool_label +
               " chars=" + std::to_string(summary.char_count) +
               " empty=" + (summary.empty ? "true" : "false");
    }

    if (type == "image") {
        return "image";
    }

    if (type == "document") {
        return "document";
    }

    if (block.contains("text") && block["text"].is_string()) {
        return type + ":" + std::to_string(block["text"].get<std::string>().size());
    }

    return type;
}

static void log_request_structure_debug(const json& body) {
    const bool stream = body.value("stream", false);
    const std::string requested_model = body.value("model", std::string());
    const json messages = body.contains("messages") ? body["messages"] : json::array();
    const size_t message_count = messages.is_array() ? messages.size() : 0;

    log_debug("[debug] request model=" + requested_model +
              " stream=" + (stream ? std::string("true") : std::string("false")) +
              " messages=" + std::to_string(message_count), false);

    if (!messages.is_array()) {
        return;
    }

    std::map<std::string, std::string> tool_names_by_id;
    for (size_t index = 0; index < messages.size(); ++index) {
        const auto& message = messages[index];
        const std::string role = message.value("role", "unknown");

        std::vector<std::string> block_summaries;
        std::vector<std::string> debug_lines;
        auto content_it = message.find("content");
        if (content_it == message.end()) {
            block_summaries.push_back("missing");
        } else if (content_it->is_array()) {
            for (const auto& block : *content_it) {
                block_summaries.push_back(summarize_message_block(block, tool_names_by_id, debug_lines));
            }
        } else {
            block_summaries.push_back(summarize_message_block(*content_it, tool_names_by_id, debug_lines));
        }

        if (block_summaries.empty()) {
            block_summaries.push_back("empty");
        }

        std::ostringstream blocks_stream;
        for (size_t block_index = 0; block_index < block_summaries.size(); ++block_index) {
            if (block_index > 0) {
                blocks_stream << ", ";
            }
            blocks_stream << block_summaries[block_index];
        }

        log_debug("[debug] msg[" + std::to_string(index) + "] role=" + role +
                  " blocks=[" + blocks_stream.str() + "]", false);
        for (const auto& line : debug_lines) {
            log_debug(line, false);
        }
    }
}

static bool try_parse_port(const std::string& value, int& port) {
    try {
        size_t consumed = 0;
        port = std::stoi(value, &consumed);
        return consumed == value.size();
    } catch (...) {
        return false;
    }
}

static bool parse_startup_options(int argc, char* argv[], StartupOptions& options,
                                  std::string& error_message) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--show") {
            options.show_console = true;
            continue;
        }
        if (arg == "--daemon") {
            error_message = "--daemon is no longer supported on win32; use Task Scheduler, NSSM, or another external supervisor.";
            return false;
        }
        if (arg == "--internal-run" || arg == "--internal-daemon") {
            error_message = "Internal daemon modes are no longer supported on win32.";
            return false;
        }
        if (arg == "--help" || arg == "-h") {
            options.show_help = true;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            error_message = "Unknown option: " + arg;
            return false;
        }
        if (options.has_port_override) {
            error_message = "Only one port override may be specified.";
            return false;
        }
        if (!try_parse_port(arg, options.port_override)) {
            error_message = "Invalid port override: " + arg;
            return false;
        }
        options.has_port_override = true;
    }

    return true;
}

static int run_gateway_server(const StartupOptions& options) {
    const bool show_console = options.show_console;
    g_console_visible = show_console;
    detach_from_console_if_needed(show_console);

    if (show_console) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        std::cout << "+------------------------------------------+" << std::endl;
        std::cout << "|     Model Gateway v1.0.0                 |" << std::endl;
        std::cout << "|     Multi-provider proxy for Claude Code |" << std::endl;
        std::cout << "+------------------------------------------+" << std::endl;
        std::cout << std::endl;
    }

    std::string lock_error;
    if (!acquire_instance_lock(lock_error)) {
        log_error("[gateway] " + lock_error, true);
        return 1;
    }

    struct InstanceLockGuard {
        ~InstanceLockGuard() {
            release_instance_lock();
        }
    } instance_lock_guard;

    g_config = Config::load();
    if (options.has_port_override) {
        g_config.port = options.port_override;
    }

    log_info("[gateway] config: " + redact_local_path(Config::getConfigPath()), show_console);
    log_info("[gateway] " + std::to_string(g_config.providers.size()) + " provider(s), " +
             std::to_string(g_config.aliases.size()) + " alias(es), " +
             std::to_string(g_config.models.size()) + " model(s)", show_console);

    httplib::Server svr;
    g_console_shutdown_event.store(0, std::memory_order_relaxed);
    g_console_control_server = &svr;
    struct ConsoleHandlerGuard {
        ~ConsoleHandlerGuard() {
            g_console_control_server = nullptr;
            g_console_shutdown_event.store(0, std::memory_order_relaxed);
            SetConsoleCtrlHandler(handle_console_shutdown, FALSE);
        }
    } console_handler_guard;
    SetConsoleCtrlHandler(handle_console_shutdown, TRUE);
    svr.set_socket_options([](socket_t sock) {
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 0);
#ifdef SO_EXCLUSIVEADDRUSE
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#endif
    });
    const int thread_pool_size = g_config.thread_pool_size;
    g_effective_thread_pool_size = thread_pool_size;
    svr.new_task_queue = [thread_pool_size]() {
        return new ServerThreadPool(static_cast<size_t>(thread_pool_size));
    };

    svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.method == "OPTIONS") {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }
        res.set_header("Access-Control-Allow-Origin", "*");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get("/v1/models", handle_list_models);
    svr.Post("/v1/messages/count_tokens", handle_count_tokens);
    svr.Post("/v1/messages", handle_messages);
    svr.Get("/", handle_webui);
    svr.Get("/api/config", handle_get_config);
    svr.Get("/api/monitor", handle_get_monitor);
    svr.Post("/api/config", handle_post_config);
    svr.Get("/api/config/test", handle_test_provider);

    std::string addr = g_config.bind;
    int port = g_config.port;

    log_info("[gateway] server starting on http://" + addr + ":" + std::to_string(port),
             show_console);
    if (thread_pool_size == 0) {
        log_info("[gateway] thread pool: unbounded dynamic workers (base " +
                 std::to_string(ServerThreadPool::kDefaultThreadCount) + ")",
                 show_console);
    } else {
        log_info("[gateway] thread pool: fixed " + std::to_string(thread_pool_size) +
                 " worker(s)", show_console);
    }
    log_info("[gateway] web UI:  http://" + addr + ":" + std::to_string(port) + "/",
             show_console);
    log_info("[gateway] models:  http://" + addr + ":" + std::to_string(port) + "/v1/models",
             show_console);

    if (show_console) {
        std::cout << std::endl;
        std::cout << "To use with Claude Code, set these environment variables before launch:" << std::endl;
        std::cout << "  $env:ANTHROPIC_BASE_URL = \"http://" << addr << ":" << port << "\"" << std::endl;
        std::cout << "  $env:ANTHROPIC_AUTH_TOKEN = \"local-gateway\"" << std::endl;
        std::cout << "Then switch gateway aliases inside Claude Code with /model <alias>." << std::endl;
        std::cout << std::endl;
    }

    if (!svr.listen(addr, port)) {
        const DWORD shutdown_event = g_console_shutdown_event.load(std::memory_order_relaxed);
        if (shutdown_event != 0) {
            log_info(std::string("[gateway] shutdown requested (") +
                         console_shutdown_event_name(shutdown_event) + ")",
                     show_console);
            return 0;
        }
        log_error("[gateway] failed to start on " + addr + ":" + std::to_string(port),
                  show_console);
        return 1;
    }

    log_warn("[gateway] server stopped", show_console);
    return 0;
}

static long long unix_ms_since_epoch(const std::chrono::system_clock::time_point& time_point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        time_point.time_since_epoch()).count();
}

static size_t get_current_process_thread_count() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    const DWORD current_pid = GetCurrentProcessId();
    size_t count = 0;

    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == current_pid) {
                ++count;
            }
            entry.dwSize = sizeof(entry);
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return count;
}

static void register_stream_monitor(const std::shared_ptr<StreamBuffer>& buf,
                                    const std::string& model,
                                    const std::string& session_id) {
    const uint64_t stream_id = g_next_stream_id.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(buf->mtx);
        buf->monitor_stream_id = stream_id;
        buf->monitor_finalized = false;
        buf->client_disconnected = false;
    }

    ActiveStreamMonitor monitor;
    monitor.id = stream_id;
    monitor.model = model.empty() ? "<unspecified>" : model;
    monitor.session_id = session_id;
    monitor.started_at = std::chrono::system_clock::now();
    monitor.buffer = buf;

    note_session_stream_registered(session_id, monitor.model);

    std::lock_guard<std::mutex> lock(g_active_streams_mutex);
    g_active_streams[stream_id] = std::move(monitor);
}

static void finalize_stream_monitor(const std::shared_ptr<StreamBuffer>& buf) {
    uint64_t stream_id = 0;
    bool cancelled = false;
    bool error = false;
    bool client_disconnected = false;
    std::string session_id;
    std::string model = "<unspecified>";

    {
        std::lock_guard<std::mutex> lock(buf->mtx);
        if (buf->monitor_stream_id == 0 || buf->monitor_finalized) {
            return;
        }

        buf->monitor_finalized = true;
        stream_id = buf->monitor_stream_id;
        cancelled = buf->cancelled;
        error = buf->error;
        client_disconnected = buf->client_disconnected;
    }

    {
        std::lock_guard<std::mutex> lock(g_active_streams_mutex);
        auto it = g_active_streams.find(stream_id);
        if (it != g_active_streams.end()) {
            session_id = it->second.session_id;
            model = it->second.model;
            g_active_streams.erase(it);
        }
    }

    note_session_stream_finalized(session_id, model, error, cancelled, client_disconnected);

    if (error) {
        g_monitor_counters.total_request_errors.fetch_add(1, std::memory_order_relaxed);
        log_session_event(session_id, "stream error model=" + model);
        return;
    }

    if (cancelled) {
        g_monitor_counters.total_stream_cancellations.fetch_add(1, std::memory_order_relaxed);
        if (client_disconnected) {
            g_monitor_counters.total_stream_disconnects.fetch_add(1, std::memory_order_relaxed);
        }
        log_session_event(session_id, std::string("stream cancelled model=") + model +
            (client_disconnected ? " reason=client_disconnect" : ""));
        return;
    }

    g_monitor_counters.total_stream_completions.fetch_add(1, std::memory_order_relaxed);
    log_session_event(session_id, "stream completed model=" + model);
}

// ─── Request body helpers ──────────────────────────────────────────────────
static std::string get_body(const httplib::Request& req) {
    return req.body;
}

static json parse_json_body(const httplib::Request& req, bool& ok) {
    ok = false;
    try {
        json j = json::parse(req.body);
        ok = true;
        return j;
    } catch (...) {
        return json::object();
    }
}

// ─── Anthropic error response helper ───────────────────────────────────────
static std::string make_error(const std::string& type, const std::string& message) {
    json err;
    err["type"] = "error";
    err["error"] = {{"type", type}, {"message", message}};
    return err.dump();
}

static std::string error_type_for_status(int status_code) {
    if (status_code == 400) return "invalid_request_error";
    if (status_code == 401) return "authentication_error";
    if (status_code == 429) return "rate_limit_error";
    return "api_error";
}

static std::string normalize_error_payload(int status_code, const std::string& raw_payload) {
    const std::string fallback_type = error_type_for_status(status_code);
    const std::string fallback_message = raw_payload.empty()
        ? ("Backend error (" + std::to_string(status_code) + ")")
        : raw_payload;

    try {
        json payload = json::parse(raw_payload);
        if (payload.value("type", "") == "error" &&
            payload.contains("error") && payload["error"].is_object()) {
            if (!payload["error"].contains("type") || !payload["error"]["type"].is_string()) {
                payload["error"]["type"] = fallback_type;
            }
            return payload.dump();
        }

        if (payload.contains("error") && payload["error"].is_object()) {
            std::string message = payload["error"].value(
                "message",
                "Backend error (" + std::to_string(status_code) + ")"
            );
            return make_error(fallback_type, message);
        }
    } catch (...) {
        // Fall back to a wrapped Anthropic-style error below.
    }

    return make_error(fallback_type, fallback_message);
}

static Config get_config_snapshot() {
    std::lock_guard<std::mutex> lock(g_config_mutex);
    return g_config;
}

static std::string mask_secret_value(const std::string& secret) {
    if (secret.empty()) {
        return "";
    }
    if (secret.size() <= 4) {
        return std::string(secret.size(), '*');
    }
    return std::string(secret.size() - 4, '*') + secret.substr(secret.size() - 4);
}

static bool is_masked_secret_reference(const std::string& candidate, const std::string& existing_secret) {
    return !candidate.empty() && !existing_secret.empty() && candidate == mask_secret_value(existing_secret);
}

static std::string resolve_secret_update(const std::string& candidate, const std::string& existing_secret) {
    if (is_masked_secret_reference(candidate, existing_secret)) {
        return existing_secret;
    }
    return candidate;
}

static std::string extract_control_plane_token(const httplib::Request& req) {
    if (req.has_header("X-Gateway-Admin-Token")) {
        return req.get_header_value("X-Gateway-Admin-Token");
    }
    if (req.has_header("X-Admin-Token")) {
        return req.get_header_value("X-Admin-Token");
    }
    if (req.has_header("Authorization")) {
        const std::string auth = req.get_header_value("Authorization");
        const std::string bearer = "Bearer ";
        if (auth.rfind(bearer, 0) == 0) {
            return auth.substr(bearer.size());
        }
    }
    if (req.has_param("token")) {
        return req.get_param_value("token");
    }
    return "";
}

static bool ensure_control_plane_access(const httplib::Request& req, httplib::Response& res) {
    const Config cfg = get_config_snapshot();
    if (cfg.admin_token.empty()) {
        return true;
    }

    if (extract_control_plane_token(req) == cfg.admin_token) {
        return true;
    }

    res.status = 401;
    res.set_content(make_error("authentication_error", "control plane token required"), "application/json");
    return false;
}

static json config_to_json(const Config& cfg) {
    json j;
    j["port"] = cfg.port;
    j["bind"] = cfg.bind;
    j["thread_pool_size"] = cfg.thread_pool_size;
    j["admin_token"] = mask_secret_value(cfg.admin_token);
    j["providers"] = json::array();
    for (const auto& p : cfg.providers) {
        json provider_json = p.to_json();
        provider_json["api_key"] = mask_secret_value(p.api_key);
        j["providers"].push_back(std::move(provider_json));
    }
    j["models"] = json::object();
    for (const auto& [id, model] : cfg.models) j["models"][id] = model.to_json();
    j["aliases"] = json::object();
    j["model_aliases"] = json::object();
    for (const auto& [alias, model_id] : cfg.aliases) {
        j["aliases"][alias] = model_id;
        const ModelConfig* model = cfg.findModel(model_id);
        if (model) {
            j["model_aliases"][alias] = model->provider + ":" + model->upstream_model;
        }
    }
    return j;
}

static void cancel_stream_buffer(const std::shared_ptr<StreamBuffer>& buf,
                                 bool peer_disconnect = false) {
    std::function<void()> cancel_upstream;
    {
        std::lock_guard<std::mutex> lock(buf->mtx);
        if (buf->cancelled) {
            if (peer_disconnect) {
                buf->client_disconnected = true;
            }
            return;
        }
        buf->cancelled = true;
        if (peer_disconnect) {
            buf->client_disconnected = true;
        }
        cancel_upstream = buf->cancel_upstream;
    }
    buf->cv.notify_all();
    if (cancel_upstream) {
        cancel_upstream();
    }
}

// ─── SSE content provider helper ───────────────────────────────────────────
// Reads from a StreamBuffer and writes SSE events to the httplib sink.
static void sse_provider(std::shared_ptr<StreamBuffer> buf,
                         httplib::DataSink& sink)
{
    std::unique_lock<std::mutex> lock(buf->mtx);
    while (true) {
        if (sink.is_writable && !sink.is_writable()) {
            lock.unlock();
            cancel_stream_buffer(buf, true);
            return;
        }

        // Wait for data, completion, or error
        buf->cv.wait_for(lock, std::chrono::milliseconds(200), [&]() {
            return !buf->chunks.empty() || buf->completed || buf->error || buf->cancelled;
        });

        if (buf->cancelled) {
            return;
        }

        // Drain available chunks
        while (!buf->chunks.empty()) {
            if (sink.is_writable && !sink.is_writable()) {
                lock.unlock();
                cancel_stream_buffer(buf, true);
                return;
            }

            std::string chunk = std::move(buf->chunks.front());
            buf->chunks.pop();
            if (buf->buffered_bytes >= chunk.size()) {
                buf->buffered_bytes -= chunk.size();
            } else {
                buf->buffered_bytes = 0;
            }
            buf->cv.notify_all();

            lock.unlock();
            if (!sink.write(chunk.data(), chunk.size())) {
                // Client disconnected
                cancel_stream_buffer(buf, true);
                return;
            }
            lock.lock();
        }

        if (buf->completed || buf->error) {
            if (buf->error && !buf->error_msg.empty()) {
                // Write error as a final event
                std::string err_event = "event: error\ndata: " +
                    normalize_error_payload(500, buf->error_msg) + "\n\n";
                lock.unlock();
                sink.write(err_event.data(), err_event.size());
                lock.lock();
            }
            break;
        }
    }
    sink.done();
}

// ─── Route handlers ────────────────────────────────────────────────────────

// GET /v1/models — list all available model aliases
static void handle_list_models(const httplib::Request&, httplib::Response& res) {
    Config cfg = get_config_snapshot();
    res.set_content(ProviderRouter::instance().listModels(cfg),
                    "application/json");
}

// POST /v1/messages — Anthropic Messages API compatible endpoint
static void handle_messages(const httplib::Request& req, httplib::Response& res) {
    GatewayRequestContext request_context = extract_request_context(req);
    bool parse_ok;
    json body = parse_json_body(req, parse_ok);
    if (!parse_ok) {
        g_monitor_counters.total_requests.fetch_add(1, std::memory_order_relaxed);
        g_monitor_counters.total_request_errors.fetch_add(1, std::memory_order_relaxed);
        res.status = 400;
        res.set_content(make_error("invalid_request_error", "Invalid JSON body"),
                        "application/json");
        return;
    }

    bool stream = body.value("stream", false);
    const std::string requested_model = body.value("model", std::string());
    log_request_structure_debug(body);
    g_monitor_counters.total_requests.fetch_add(1, std::memory_order_relaxed);
    note_session_request(request_context.session_id, requested_model, stream);
    log_session_event(request_context.session_id,
        std::string("messages model=") + requested_model + (stream ? " stream=true" : " stream=false"));
    if (stream) {
        g_monitor_counters.total_stream_requests.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_monitor_counters.total_nonstream_requests.fetch_add(1, std::memory_order_relaxed);
    }

    Config cfg = get_config_snapshot();

    if (stream) {
        // ─── Streaming response ───────────────────────────────────────────
        auto buf = std::make_shared<StreamBuffer>();
        int status_code = 503;

        // Start backend request in background
        ProviderRouter::instance().chatStream(cfg, req.body, request_context, buf, status_code, [&req]() {
            return req.is_connection_closed && req.is_connection_closed();
        });

        if (status_code >= 400) {
            std::string error_payload;
            {
                std::lock_guard<std::mutex> buf_lock(buf->mtx);
                error_payload = buf->error_msg;
            }
            g_monitor_counters.total_request_errors.fetch_add(1, std::memory_order_relaxed);
            note_session_error(request_context.session_id, requested_model);
            log_session_event(request_context.session_id,
                "messages stream_start_failed model=" + requested_model + " status=" + std::to_string(status_code));
            res.status = status_code;
            res.set_content(normalize_error_payload(status_code, error_payload),
                            "application/json");
            return;
        }

        register_stream_monitor(buf, requested_model, request_context.session_id);

        // SSE response
        res.status = 200;
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");

        res.set_chunked_content_provider("text/event-stream",
            [buf](size_t /*offset*/, httplib::DataSink& sink) {
                sse_provider(buf, sink);
                return true;
            },
            [buf](bool success) {
                if (!success) {
                    cancel_stream_buffer(buf, true);
                }
                finalize_stream_monitor(buf);
            }
        );

    } else {
        // ─── Non-streaming response ──────────────────────────────────────
        int status_code = 503;
        std::string result = ProviderRouter::instance().chat(
            cfg, req.body, request_context, status_code);
        if (status_code >= 400) {
            g_monitor_counters.total_request_errors.fetch_add(1, std::memory_order_relaxed);
            note_session_error(request_context.session_id, requested_model);
        }
        log_session_event(request_context.session_id,
            "messages completed model=" + requested_model + " status=" + std::to_string(status_code));
        res.status = status_code;
        res.set_content(result, "application/json");
    }
}

// POST /v1/messages/count_tokens — Anthropic Messages count_tokens compatible endpoint
static void handle_count_tokens(const httplib::Request& req, httplib::Response& res) {
    GatewayRequestContext request_context = extract_request_context(req);
    bool parse_ok;
    json body = parse_json_body(req, parse_ok);
    if (!parse_ok) {
        g_monitor_counters.total_count_token_requests.fetch_add(1, std::memory_order_relaxed);
        g_monitor_counters.total_request_errors.fetch_add(1, std::memory_order_relaxed);
        res.status = 400;
        res.set_content(make_error("invalid_request_error", "Invalid JSON body"), "application/json");
        return;
    }

    const std::string requested_model = body.value("model", std::string());
    g_monitor_counters.total_count_token_requests.fetch_add(1, std::memory_order_relaxed);
    note_session_request(request_context.session_id, requested_model, false, true);
    log_session_event(request_context.session_id, "count_tokens model=" + requested_model);

    const Config cfg = get_config_snapshot();
    int status_code = 503;
    std::string result = ProviderRouter::instance().countTokens(cfg, req.body, request_context, status_code);
    if (status_code >= 400) {
        g_monitor_counters.total_request_errors.fetch_add(1, std::memory_order_relaxed);
        note_session_error(request_context.session_id, requested_model);
    }
    log_session_event(request_context.session_id,
        "count_tokens completed model=" + requested_model + " status=" + std::to_string(status_code));

    res.status = status_code;
    res.set_content(result, "application/json");
}

// GET / — web UI
static void handle_webui(const httplib::Request&, httplib::Response& res) {
    res.set_content(WEBUI_HTML, "text/html; charset=utf-8");
}

// GET /api/config — get current configuration
static void handle_get_config(const httplib::Request& req, httplib::Response& res) {
    if (!ensure_control_plane_access(req, res)) {
        return;
    }
    res.set_content(config_to_json(get_config_snapshot()).dump(2), "application/json");
}

// GET /api/monitor — lightweight runtime metrics and active stream view
static void handle_get_monitor(const httplib::Request& req, httplib::Response& res) {
    if (!ensure_control_plane_access(req, res)) {
        return;
    }
    const Config cfg = get_config_snapshot();
    const auto now_wall = std::chrono::system_clock::now();
    const auto now_steady = std::chrono::steady_clock::now();

    json streams = json::array();
    json sessions = json::array();
    size_t total_buffered_bytes = 0;

    {
        std::lock_guard<std::mutex> lock(g_active_streams_mutex);
        for (const auto& [stream_id, monitor] : g_active_streams) {
            json stream_json;
            stream_json["id"] = stream_id;
            stream_json["model"] = monitor.model;
            stream_json["session_id"] = monitor.session_id;
            stream_json["started_at_unix_ms"] = unix_ms_since_epoch(monitor.started_at);
            stream_json["age_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(
                now_wall - monitor.started_at).count();

            size_t buffered_bytes = 0;
            size_t queued_chunks = 0;
            bool completed = false;
            bool error = false;
            bool cancelled = false;
            bool client_disconnected = false;

            {
                std::lock_guard<std::mutex> buf_lock(monitor.buffer->mtx);
                buffered_bytes = monitor.buffer->buffered_bytes;
                queued_chunks = monitor.buffer->chunks.size();
                completed = monitor.buffer->completed;
                error = monitor.buffer->error;
                cancelled = monitor.buffer->cancelled;
                client_disconnected = monitor.buffer->client_disconnected;
            }

            total_buffered_bytes += buffered_bytes;
            stream_json["buffered_bytes"] = buffered_bytes;
            stream_json["queued_chunks"] = queued_chunks;
            stream_json["completed"] = completed;
            stream_json["error"] = error;
            stream_json["cancelled"] = cancelled;
            stream_json["client_disconnected"] = client_disconnected;
            streams.push_back(std::move(stream_json));
        }
    }

    {
        std::vector<SessionMonitor> session_rows;
        std::lock_guard<std::mutex> lock(g_sessions_mutex);
        for (const auto& [_, session] : g_sessions) {
            session_rows.push_back(session);
        }
        std::sort(session_rows.begin(), session_rows.end(), [](const SessionMonitor& lhs, const SessionMonitor& rhs) {
            return lhs.last_seen > rhs.last_seen;
        });
        for (const auto& session : session_rows) {
            sessions.push_back({
                {"id", session.id},
                {"last_model", session.last_model},
                {"first_seen_unix_ms", unix_ms_since_epoch(session.first_seen)},
                {"last_seen_unix_ms", unix_ms_since_epoch(session.last_seen)},
                {"active_streams", session.active_streams},
                {"total_requests", session.total_requests},
                {"total_stream_requests", session.total_stream_requests},
                {"total_nonstream_requests", session.total_nonstream_requests},
                {"total_count_token_requests", session.total_count_token_requests},
                {"total_errors", session.total_errors},
                {"total_stream_completions", session.total_stream_completions},
                {"total_stream_cancellations", session.total_stream_cancellations},
                {"total_stream_disconnects", session.total_stream_disconnects}
            });
        }
    }

    json monitor;
    monitor["started_at_unix_ms"] = unix_ms_since_epoch(g_monitor_started_wall);
    monitor["uptime_seconds"] = std::chrono::duration_cast<std::chrono::seconds>(
        now_steady - g_monitor_started_steady).count();
    monitor["runtime"] = {
        {"process_id", GetCurrentProcessId()},
        {"thread_count", get_current_process_thread_count()},
        {"providers", cfg.providers.size()},
        {"models", cfg.models.size()},
        {"aliases", cfg.aliases.size()},
        {"active_streams", streams.size()},
        {"buffered_bytes", total_buffered_bytes},
        {"configured_thread_pool_size", cfg.thread_pool_size},
        {"current_thread_pool_size", ServerThreadPool::currentWorkerCount()},
        {"effective_thread_pool_size", g_effective_thread_pool_size},
        {"thread_pool_mode", cfg.thread_pool_size == 0 ? "unbounded" : "fixed"},
        {"console_visible", g_console_visible},
        {"config_path", redact_local_path(Config::getConfigPath())},
        {"log_path", redact_local_path(get_runtime_log_path())},
        {"listener", std::string("http://") + cfg.bind + ":" + std::to_string(cfg.port)},
        {"webui", std::string("http://") + cfg.bind + ":" + std::to_string(cfg.port) + "/"}
    };
    monitor["counters"] = {
        {"total_requests", g_monitor_counters.total_requests.load(std::memory_order_relaxed)},
        {"total_stream_requests", g_monitor_counters.total_stream_requests.load(std::memory_order_relaxed)},
        {"total_nonstream_requests", g_monitor_counters.total_nonstream_requests.load(std::memory_order_relaxed)},
        {"total_count_token_requests", g_monitor_counters.total_count_token_requests.load(std::memory_order_relaxed)},
        {"total_request_errors", g_monitor_counters.total_request_errors.load(std::memory_order_relaxed)},
        {"total_stream_completions", g_monitor_counters.total_stream_completions.load(std::memory_order_relaxed)},
        {"total_stream_cancellations", g_monitor_counters.total_stream_cancellations.load(std::memory_order_relaxed)},
        {"total_stream_disconnects", g_monitor_counters.total_stream_disconnects.load(std::memory_order_relaxed)},
        {"total_config_saves", g_monitor_counters.total_config_saves.load(std::memory_order_relaxed)},
        {"total_provider_tests", g_monitor_counters.total_provider_tests.load(std::memory_order_relaxed)}
    };
    monitor["sessions"] = std::move(sessions);
    monitor["streams"] = std::move(streams);

    res.set_content(monitor.dump(2), "application/json");
}

// POST /api/config — update configuration
static void handle_post_config(const httplib::Request& req, httplib::Response& res) {
    if (!ensure_control_plane_access(req, res)) {
        return;
    }

    bool ok;
    json j = parse_json_body(req, ok);
    if (!ok) {
        res.status = 400;
        res.set_content(make_error("invalid_request_error", "Invalid JSON"),
                        "application/json");
        return;
    }

    try {
        Config saved;
        {
            std::lock_guard<std::mutex> lock(g_config_mutex);
            Config updated = g_config;

            if (j.contains("port") && j["port"].is_number_integer())
                updated.port = j["port"].get<int>();
            if (j.contains("bind") && j["bind"].is_string())
                updated.bind = j["bind"].get<std::string>();
            if (j.contains("thread_pool_size")) {
                if (!j["thread_pool_size"].is_number_integer()) {
                    res.status = 400;
                    res.set_content(make_error("invalid_request_error", "thread_pool_size must be an integer"),
                                    "application/json");
                    return;
                }
                const int thread_pool_size = j["thread_pool_size"].get<int>();
                if (thread_pool_size < 0) {
                    res.status = 400;
                    res.set_content(make_error("invalid_request_error", "thread_pool_size must be >= 0"),
                                    "application/json");
                    return;
                }
                updated.thread_pool_size = thread_pool_size;
            }
            if (j.contains("admin_token") && j["admin_token"].is_string()) {
                updated.admin_token = resolve_secret_update(j["admin_token"].get<std::string>(), g_config.admin_token);
            }
            if (j.contains("providers") && j["providers"].is_array()) {
                updated.providers.clear();
                for (const auto& p : j["providers"]) {
                    ProviderConfig provider = ProviderConfig::from_json(p);
                    const ProviderConfig* existing_provider = g_config.findProvider(provider.id);
                    if (p.contains("api_key") && p["api_key"].is_string()) {
                        provider.api_key = resolve_secret_update(
                            p["api_key"].get<std::string>(),
                            existing_provider ? existing_provider->api_key : std::string());
                    } else if (existing_provider) {
                        provider.api_key = existing_provider->api_key;
                    }
                    updated.providers.push_back(std::move(provider));
                }
            }
            if (j.contains("models") && j["models"].is_object()) {
                updated.models.clear();
                for (auto& [k, v] : j["models"].items()) {
                    ModelConfig model = ModelConfig::from_json(v, k);
                    if (model.id.empty()) model.id = k;
                    updated.models[model.id] = model;
                }
            }
            if (j.contains("aliases") && j["aliases"].is_object()) {
                updated.aliases.clear();
                for (auto& [k, v] : j["aliases"].items()) {
                    if (v.is_string()) updated.aliases[k] = v.get<std::string>();
                }
            }
            if (j.contains("model_aliases") && j["model_aliases"].is_object()) {
                if (!j.contains("models") && !j.contains("aliases")) {
                    updated.models.clear();
                    updated.aliases.clear();
                    for (auto& [k, v] : j["model_aliases"].items()) {
                        if (!v.is_string()) continue;
                        const std::string target = v.get<std::string>();
                        auto colon = target.find(':');
                        if (colon == std::string::npos) continue;

                        ModelConfig model;
                        model.id = k;
                        model.provider = target.substr(0, colon);
                        model.upstream_model = target.substr(colon + 1);
                        updated.models[model.id] = model;
                        updated.aliases[k] = model.id;
                    }
                }
            }

            updated.save();
            g_config = updated;
            saved = g_config;
        }

        g_monitor_counters.total_config_saves.fetch_add(1, std::memory_order_relaxed);

        res.set_content(config_to_json(saved).dump(2), "application/json");
    } catch (std::exception& e) {
        res.status = 500;
        res.set_content(make_error("api_error", std::string("Save failed: ") + e.what()),
                        "application/json");
    }
}

// GET /api/config/test — test a provider connection
static void handle_test_provider(const httplib::Request& req, httplib::Response& res) {
    if (!ensure_control_plane_access(req, res)) {
        return;
    }

    g_monitor_counters.total_provider_tests.fetch_add(1, std::memory_order_relaxed);
    auto provider_id = req.get_param_value("provider_id");
    if (provider_id.empty()) {
        res.status = 400;
        res.set_content(make_error("invalid_request_error", "provider_id required"),
                        "application/json");
        return;
    }

    ProviderConfig pcfg;
    {
        std::lock_guard<std::mutex> lock(g_config_mutex);
        const ProviderConfig* found = g_config.findProvider(provider_id);
        if (!found) {
            res.status = 404;
            res.set_content(make_error("not_found", "Provider not found: " + provider_id),
                            "application/json");
            return;
        }
        pcfg = *found;
    }

    try {
        std::string url = pcfg.base_url;
        // For Anthropic providers, test with /v1/models
        // For OpenAI, use /v1/models
        std::string test_path;
        if (pcfg.type == "anthropic") {
            test_path = "/v1/models";
        } else {
            test_path = "/models";
        }

        HttpClient client;
        auto api_resp = client.get(url + test_path, {}, 15000);

        json result;
        result["status"] = (api_resp.status_code >= 200 && api_resp.status_code < 300)
                          ? "ok" : "error";
        result["status_code"] = api_resp.status_code;
        result["provider"] = pcfg.id;
        res.set_content(result.dump(), "application/json");
    } catch (std::exception& e) {
        json result;
        result["status"] = "error";
        result["error"] = e.what();
        res.set_content(result.dump(), "application/json");
    }
}

// ─── Main ──────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    StartupOptions options;
    std::string error_message;
    if (!parse_startup_options(argc, argv, options, error_message)) {
        std::cerr << error_message << std::endl;
        print_usage();
        return 2;
    }

    if (options.show_help) {
        print_usage();
        return 0;
    }

    return run_gateway_server(options);
}
