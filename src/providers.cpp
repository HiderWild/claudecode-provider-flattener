#include "providers.h"
#include <sstream>
#include <iostream>
#include <regex>
#include <set>
#include <thread>

namespace {
constexpr size_t kDefaultMaxBufferedStreamBytes = 1024 * 1024;

bool push_stream_chunk(const std::shared_ptr<StreamBuffer>& out_buf, std::string chunk) {
    if (chunk.empty()) {
        return true;
    }

    std::unique_lock<std::mutex> lock(out_buf->mtx);
    out_buf->cv.wait(lock, [&]() {
        return out_buf->cancelled || out_buf->buffered_bytes < out_buf->max_buffered_bytes;
    });

    if (out_buf->cancelled) {
        return false;
    }

    out_buf->buffered_bytes += chunk.size();
    out_buf->chunks.push(std::move(chunk));
    lock.unlock();
    out_buf->cv.notify_all();
    return true;
}

void finish_stream(const std::shared_ptr<StreamBuffer>& out_buf,
                   bool error,
                   std::string error_message = std::string()) {
    {
        std::lock_guard<std::mutex> lock(out_buf->mtx);
        if (error) {
            out_buf->error = true;
            out_buf->error_msg = std::move(error_message);
        }
        out_buf->completed = true;
        out_buf->cancel_upstream = nullptr;
    }
    out_buf->cv.notify_all();
}
}

// ─── Utility ────────────────────────────────────────────────────────────────

std::string generate_message_id() {
    thread_local std::mt19937 rng((std::random_device())());
    static const char hex[] = "0123456789abcdef";
    std::string id = "msg_gateway_";
    for (int i = 0; i < 24; i++) {
        id += hex[rng() % 16];
    }
    return id;
}

std::string json_string_or(const json& j, const std::string& key, const std::string& def) {
    auto it = j.find(key);
    if (it != j.end() && it->is_string()) return *it;
    return def;
}

// Trim whitespace
static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Split SSE data lines
static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream stream(s);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

static bool take_next_sse_event(std::string& buffer, std::string& out_event) {
    size_t delimiter_pos = std::string::npos;
    size_t delimiter_len = 0;

    const size_t lf_pos = buffer.find("\n\n");
    const size_t crlf_pos = buffer.find("\r\n\r\n");

    if (crlf_pos != std::string::npos &&
        (lf_pos == std::string::npos || crlf_pos < lf_pos)) {
        delimiter_pos = crlf_pos;
        delimiter_len = 4;
    } else if (lf_pos != std::string::npos) {
        delimiter_pos = lf_pos;
        delimiter_len = 2;
    }

    if (delimiter_pos == std::string::npos) {
        return false;
    }

    out_event = buffer.substr(0, delimiter_pos + delimiter_len);
    buffer.erase(0, delimiter_pos + delimiter_len);
    return true;
}

static std::string extract_openai_text(const json& value) {
    std::string out;
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (!value.is_array()) {
        return out;
    }

    for (const auto& item : value) {
        if (item.is_string()) {
            out += item.get<std::string>();
            continue;
        }
        if (!item.is_object()) continue;

        std::string type = item.value("type", "");
        if (type == "text" || type == "output_text" || type == "input_text") {
            out += item.value("text", "");
        } else if (item.contains("text") && item["text"].is_string()) {
            out += item["text"].get<std::string>();
        }
    }

    return out;
}

static std::string extract_reasoning_text(const json& value) {
    std::string out;

    if (value.is_string()) {
        return value.get<std::string>();
    }

    if (value.is_array()) {
        for (const auto& item : value) {
            out += extract_reasoning_text(item);
        }
        return out;
    }

    if (!value.is_object()) {
        return out;
    }

    if (value.contains("reasoning_content")) {
        return extract_reasoning_text(value["reasoning_content"]);
    }
    if (value.contains("thinking")) {
        return extract_reasoning_text(value["thinking"]);
    }
    if (value.contains("summary")) {
        return extract_reasoning_text(value["summary"]);
    }
    if (value.contains("text")) {
        return extract_reasoning_text(value["text"]);
    }
    if (value.contains("content")) {
        return extract_reasoning_text(value["content"]);
    }

    return out;
}

static void maybe_add_thinking_block(json& content, const json& source) {
    std::string reasoning;
    if (source.contains("reasoning_content")) {
        reasoning = extract_reasoning_text(source["reasoning_content"]);
    } else if (source.contains("reasoning")) {
        reasoning = extract_reasoning_text(source["reasoning"]);
    }

    if (reasoning.empty()) return;

    json thinking_block;
    thinking_block["type"] = "thinking";
    thinking_block["thinking"] = reasoning;
    thinking_block["signature"] = "";
    content.push_back(thinking_block);
}

// ─── AnthropicProvider ──────────────────────────────────────────────────────

ProviderConfig AnthropicProvider::defaultConfig() const {
    return {
        "anthropic-default", "anthropic", "Anthropic",
        "", "https://api.anthropic.com",
        {"claude-sonnet-4-20250514", "claude-3.5-haiku-latest"}
    };
}

std::map<std::string, std::string> AnthropicProvider::getHeaders(const ProviderConfig& cfg) const {
    std::map<std::string, std::string> h;
    h["x-api-key"] = cfg.api_key;
    h["anthropic-version"] = "2023-06-01";
    h["Content-Type"] = "application/json";
    return h;
}

std::string AnthropicProvider::transformRequest(const json& anthropic_req,
                                                 const ModelConfig& model_cfg) {
    json req = anthropic_req;
    req["model"] = model_cfg.upstream_model;

    if (model_cfg.thinking == "enabled") {
        req["thinking"] = {{"type", "enabled"}};
        if (!model_cfg.effort.empty()) {
            json output_config = json::object();
            if (req.contains("output_config") && req["output_config"].is_object()) {
                output_config = req["output_config"];
            }
            output_config["effort"] = model_cfg.effort;
            req["output_config"] = output_config;
        }
    } else if (model_cfg.thinking == "disabled") {
        req["thinking"] = {{"type", "disabled"}};
        if (req.contains("output_config") && req["output_config"].is_object()) {
            req["output_config"].erase("effort");
            if (req["output_config"].empty()) {
                req.erase("output_config");
            }
        }
    }

    return req.dump();
}

std::string AnthropicProvider::transformResponse(const std::string& backend_body,
                                                  const std::string& gateway_model,
                                                  const ModelConfig& model_cfg) {
    try {
        json resp = json::parse(backend_body);
        if (resp.contains("model") && resp["model"].is_string()) {
            resp["model"] = gateway_model;
        }
        return resp.dump();
    } catch (...) {
        return backend_body; // passthrough on parse failure
    }
}

std::string AnthropicProvider::transformStreamChunk(const std::string& raw_chunk,
                                                    const std::string& gateway_model,
                                                    const ModelConfig& model_cfg,
                                                    bool& chunk_finished) {
    chunk_finished = false;

    // Anthropic SSE lines look like:
    //   event: content_block_delta
    //   data: {"type":"content_block_delta",...}
    //
    // We need to fix the model name in message_start events.
    std::string result;
    auto lines = split_lines(raw_chunk);
    for (const auto& line : lines) {
        if (line.find("data: ") == 0) {
            std::string data_str = line.substr(6);
            try {
                json data = json::parse(data_str);
                // Fix model name in message_start
                if (data.value("type", "") == "message_start" &&
                    data.contains("message") && data["message"].is_object() &&
                    data["message"].contains("model")) {
                    data["message"]["model"] = gateway_model;
                }
                // Fix model in message.delta if present (non-standard but safe)
                result += "data: " + data.dump() + "\n";
            } catch (...) {
                result += line + "\n";
            }
        } else {
            result += line + "\n";
        }
    }

    return result;
}

// ─── OpenAIProvider ─────────────────────────────────────────────────────────

ProviderConfig OpenAIProvider::defaultConfig() const {
    return {
        "openai-default", "openai", "OpenAI",
        "", "https://api.openai.com/v1",
        {"gpt-4o", "gpt-4o-mini"}
    };
}

std::map<std::string, std::string> OpenAIProvider::getHeaders(const ProviderConfig& cfg) const {
    std::map<std::string, std::string> h;
    h["Authorization"] = "Bearer " + cfg.api_key;
    h["Content-Type"] = "application/json";
    return h;
}

// ─── Anthropic → OpenAI request translation ─────────────────────────────────

std::string OpenAIProvider::transformRequest(const json& anthropic_req,
                                             const ModelConfig& model_cfg) {
    json oai;
    oai["model"] = model_cfg.upstream_model;
    oai["messages"] = json::array();

    // Handle system prompt: convert to system-role message or use top-level
    bool has_system = false;
    std::string system_text;
    if (anthropic_req.contains("system")) {
        has_system = true;
        const auto& sys = anthropic_req["system"];
        if (sys.is_string()) {
            system_text = sys;
        } else if (sys.is_array()) {
            for (const auto& block : sys) {
                if (block.value("type", "") == "text") {
                    system_text += block.value("text", "");
                }
            }
        }
    }
    if (!system_text.empty()) {
        json sys_msg;
        sys_msg["role"] = "system";
        sys_msg["content"] = system_text;
        oai["messages"].push_back(sys_msg);
    }

    // Copy messages (translate Anthropic content blocks → OpenAI content strings)
    if (anthropic_req.contains("messages") && anthropic_req["messages"].is_array()) {
        for (const auto& msg : anthropic_req["messages"]) {
            json oai_msg;
            oai_msg["role"] = msg.value("role", "user");

            if (!msg.contains("content")) {
                oai_msg["content"] = "";
                oai["messages"].push_back(oai_msg);
                continue;
            }

            const auto& content = msg["content"];

            if (content.is_string()) {
                oai_msg["content"] = content;
            } else if (content.is_array()) {
                std::string text_content;
                std::string reasoning_content;
                bool has_tool_calls = false;
                json tool_calls = json::array();
                for (const auto& block : content) {
                    std::string type = block.value("type", "text");
                    if (type == "text") {
                        text_content += block.value("text", "");
                    } else if (type == "thinking") {
                        reasoning_content += block.value("thinking", "");
                    } else if (type == "tool_use") {
                        has_tool_calls = true;
                        json tc;
                        tc["id"] = block.value("id", "");
                        tc["type"] = "function";
                        tc["function"] = {
                            {"name", block["name"].get<std::string>()},
                            {"arguments", block["input"].dump()}
                        };
                        tool_calls.push_back(tc);
                    } else if (type == "tool_result") {
                        // Tool result → user message with tool role content
                        json tr_msg;
                        tr_msg["role"] = "tool";
                        tr_msg["tool_call_id"] = block.value("tool_use_id", "");
                        const auto& tr_content = block["content"];
                        if (tr_content.is_string()) {
                            tr_msg["content"] = tr_content;
                        } else {
                            std::string tr_text;
                            for (const auto& tc : tr_content) {
                                if (tc.value("type", "") == "text") {
                                    tr_text += tc.value("text", "");
                                }
                            }
                            tr_msg["content"] = tr_text;
                        }
                        oai["messages"].push_back(tr_msg);
                        continue; // skip the normal push below
                    }
                }

                if (!reasoning_content.empty()) {
                    oai_msg["reasoning_content"] = reasoning_content;
                }

                if (!text_content.empty()) {
                    oai_msg["content"] = text_content;
                } else if (has_tool_calls) {
                    oai_msg["content"] = nullptr;
                } else if (!reasoning_content.empty()) {
                    oai_msg["content"] = "";
                }

                if (!text_content.empty() || !reasoning_content.empty() || has_tool_calls) {
                    if (!oai_msg.contains("content")) {
                        oai_msg["content"] = nullptr;
                    }
                    if (has_tool_calls) {
                        oai_msg["tool_calls"] = tool_calls;
                    }
                }
            } else if (content.is_null()) {
                // Assistant messages with tool_calls use null content
                oai_msg["content"] = nullptr;
                if (msg.contains("tool_calls")) {
                    oai_msg["tool_calls"] = msg["tool_calls"];
                }
            }

            oai["messages"].push_back(oai_msg);
        }
    }

    if (anthropic_req.contains("tools") && anthropic_req["tools"].is_array()) {
        oai["tools"] = json::array();
        for (const auto& tool : anthropic_req["tools"]) {
            json oai_tool;
            oai_tool["type"] = "function";
            oai_tool["function"] = {
                {"name", tool.value("name", "")},
                {"description", tool.value("description", "")},
                {"parameters", tool.contains("input_schema") ? tool["input_schema"] : json{{"type", "object"}, {"properties", json::object()}}}
            };
            oai["tools"].push_back(oai_tool);
        }
    }

    if (anthropic_req.contains("tool_choice")) {
        const auto& tool_choice = anthropic_req["tool_choice"];
        if (tool_choice.is_string()) {
            oai["tool_choice"] = tool_choice;
        } else if (tool_choice.is_object()) {
            std::string type = tool_choice.value("type", "auto");
            if (type == "auto" || type == "none") {
                oai["tool_choice"] = type;
            } else if (type == "any") {
                oai["tool_choice"] = "required";
            } else if (type == "tool") {
                oai["tool_choice"] = {
                    {"type", "function"},
                    {"function", {{"name", tool_choice.value("name", "")}}}
                };
            }
        }
    }

    // Copy parameters
    if (anthropic_req.contains("max_tokens")) oai["max_tokens"] = anthropic_req["max_tokens"];
    if (anthropic_req.contains("temperature") && anthropic_req["temperature"].is_number()) {
        oai["temperature"] = anthropic_req["temperature"];
    }
    if (anthropic_req.contains("top_p") && anthropic_req["top_p"].is_number()) {
        oai["top_p"] = anthropic_req["top_p"];
    }
    if (anthropic_req.contains("stop_sequences") && anthropic_req["stop_sequences"].is_array()) {
        oai["stop"] = anthropic_req["stop_sequences"];
    }
    if (anthropic_req.contains("stream") && anthropic_req["stream"].is_boolean()) {
        oai["stream"] = anthropic_req["stream"];
    }

    if (model_cfg.thinking == "enabled") {
        oai["thinking"] = {{"type", "enabled"}};
    } else if (model_cfg.thinking == "disabled") {
        oai["thinking"] = {{"type", "disabled"}};
    }

    return oai.dump();
}

// ─── OpenAI → Anthropic non-streaming response translation ──────────────────

std::string OpenAIProvider::transformResponse(const std::string& backend_body,
                                               const std::string& gateway_model,
                                               const ModelConfig& model_cfg) {
    try {
        json oai_resp = json::parse(backend_body);
        json anthropic_resp;

        anthropic_resp["id"] = generate_message_id();
        anthropic_resp["type"] = "message";
        anthropic_resp["role"] = "assistant";
        anthropic_resp["model"] = gateway_model;
        anthropic_resp["content"] = json::array();
        anthropic_resp["stop_reason"] = nullptr;
        anthropic_resp["stop_sequence"] = nullptr;
        anthropic_resp["usage"] = json::object();

        if (oai_resp.contains("choices") && oai_resp["choices"].is_array() && !oai_resp["choices"].empty()) {
            auto& choice = oai_resp["choices"][0];
            auto& msg = choice["message"];

            // Map finish_reason
            std::string fr = choice.value("finish_reason", "");
            if (fr == "stop") anthropic_resp["stop_reason"] = "end_turn";
            else if (fr == "length") anthropic_resp["stop_reason"] = "max_tokens";
            else if (fr == "tool_calls") anthropic_resp["stop_reason"] = "tool_use";
            else if (fr == "content_filter") anthropic_resp["stop_reason"] = "content_filter";
            else anthropic_resp["stop_reason"] = "end_turn";

            maybe_add_thinking_block(anthropic_resp["content"], msg);

            if (msg.contains("content") && !msg["content"].is_null()) {
                std::string text = extract_openai_text(msg["content"]);
                json text_block;
                text_block["type"] = "text";
                text_block["text"] = text;
                if (!text.empty()) {
                    anthropic_resp["content"].push_back(text_block);
                }
            }

            // Tool calls
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                for (const auto& tc : msg["tool_calls"]) {
                    json tool_block;
                    tool_block["type"] = "tool_use";
                    tool_block["id"] = tc.value("id", "");
                    tool_block["name"] = tc["function"]["name"];
                    try {
                        tool_block["input"] = json::parse(tc["function"]["arguments"].get<std::string>());
                    } catch (...) {
                        tool_block["input"] = tc["function"]["arguments"];
                    }
                    anthropic_resp["content"].push_back(tool_block);
                }
            }
        }

        // Usage
        if (oai_resp.contains("usage")) {
            anthropic_resp["usage"]["input_tokens"] = oai_resp["usage"].value("prompt_tokens", 0);
            anthropic_resp["usage"]["output_tokens"] = oai_resp["usage"].value("completion_tokens", 0);
        }

        return anthropic_resp.dump();
    } catch (std::exception& e) {
        return "{\"type\":\"error\",\"error\":{\"type\":\"api_error\",\"message\":\""
               + std::string(e.what()) + "\"}}";
    }
}

// ─── OpenAI → Anthropic streaming translation ───────────────────────────────

void OpenAIProvider::resetStreamState() {
    stream_state_ = StreamState{};
}

std::string OpenAIProvider::emitMessageStart(const std::string& gateway_model) {
    json ev;
    ev["type"] = "message_start";
    ev["message"] = {
        {"id", stream_state_.message_id},
        {"type", "message"},
        {"role", "assistant"},
        {"content", json::array()},
        {"model", gateway_model},
        {"stop_reason", nullptr},
        {"stop_sequence", nullptr},
        {"usage", {{"input_tokens", stream_state_.usage_prompt}, {"output_tokens", 0}}}
    };
    return "event: message_start\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitTextBlockStart(int index) {
    json ev;
    ev["type"] = "content_block_start";
    ev["index"] = index;
    ev["content_block"] = {{"type", "text"}, {"text", ""}};
    return "event: content_block_start\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitTextBlockDelta(int index, const std::string& text) {
    json ev;
    ev["type"] = "content_block_delta";
    ev["index"] = index;
    ev["delta"] = {{"type", "text_delta"}, {"text", text}};
    return "event: content_block_delta\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitThinkingBlockStart(int index) {
    json ev;
    ev["type"] = "content_block_start";
    ev["index"] = index;
    ev["content_block"] = {{"type", "thinking"}, {"thinking", ""}, {"signature", ""}};
    return "event: content_block_start\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitThinkingBlockDelta(int index, const std::string& thinking) {
    json ev;
    ev["type"] = "content_block_delta";
    ev["index"] = index;
    ev["delta"] = {{"type", "thinking_delta"}, {"thinking", thinking}};
    return "event: content_block_delta\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitSignatureDelta(int index, const std::string& signature) {
    json ev;
    ev["type"] = "content_block_delta";
    ev["index"] = index;
    ev["delta"] = {{"type", "signature_delta"}, {"signature", signature}};
    return "event: content_block_delta\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitToolUseBlockStart(int index, const ToolCallState& tool_state) {
    json ev;
    ev["type"] = "content_block_start";
    ev["index"] = index;
    ev["content_block"] = {
        {"type", "tool_use"},
        {"id", tool_state.id},
        {"name", tool_state.name},
        {"input", json::object()}
    };
    return "event: content_block_start\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitInputJsonDelta(int index, const std::string& partial_json) {
    json ev;
    ev["type"] = "content_block_delta";
    ev["index"] = index;
    ev["delta"] = {{"type", "input_json_delta"}, {"partial_json", partial_json}};
    return "event: content_block_delta\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitContentBlockStop(int index) {
    json ev;
    ev["type"] = "content_block_stop";
    ev["index"] = index;
    return "event: content_block_stop\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitMessageDelta() {
    json ev;
    ev["type"] = "message_delta";
    ev["delta"] = {{"stop_reason", stream_state_.stop_reason.empty() ? nullptr : json(stream_state_.stop_reason)},
                   {"stop_sequence", nullptr}};
    ev["usage"] = {{"output_tokens", stream_state_.usage_completion}};
    return "event: message_delta\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::emitMessageStop() {
    json ev;
    ev["type"] = "message_stop";
    return "event: message_stop\ndata: " + ev.dump() + "\n\n";
}

std::string OpenAIProvider::closeActiveBlock() {
    std::string result;
    if (stream_state_.active_block_type.empty()) {
        return result;
    }

    if (stream_state_.active_block_type == "thinking") {
        result += emitSignatureDelta(stream_state_.active_block_index, "");
        result += emitContentBlockStop(stream_state_.active_block_index);
    } else if (stream_state_.active_block_type == "text") {
        result += emitContentBlockStop(stream_state_.active_block_index);
    } else if (stream_state_.active_block_type == "tool") {
        auto it = stream_state_.tool_calls.find(stream_state_.active_tool_index);
        if (it != stream_state_.tool_calls.end() && it->second.started && !it->second.closed) {
            result += emitContentBlockStop(it->second.anthropic_index);
            it->second.closed = true;
        }
    }

    stream_state_.active_block_type.clear();
    stream_state_.active_block_index = -1;
    stream_state_.active_tool_index = -1;
    return result;
}

std::string OpenAIProvider::closeAllToolBlocks() {
    std::string result;
    for (auto& [tool_index, tool_state] : stream_state_.tool_calls) {
        if (tool_state.started && !tool_state.closed) {
            result += emitContentBlockStop(tool_state.anthropic_index);
            tool_state.closed = true;
        }
    }
    return result;
}

std::string OpenAIProvider::transformStreamChunk(const std::string& raw_chunk,
                                                 const std::string& gateway_model,
                                                 const ModelConfig& model_cfg,
                                                 bool& chunk_finished) {
    chunk_finished = false;
    std::string result;

    // Handle [DONE] sentinel
    std::string trimmed = trim(raw_chunk);
    if (trimmed == "[DONE]") {
        result += closeActiveBlock();
        result += closeAllToolBlocks();
        result += emitMessageDelta();
        result += emitMessageStop();
        chunk_finished = true;
        resetStreamState();
        return result;
    }

    // Try to parse as SSE data
    // OpenAI SSE format: data: {json}\n\n
    for (const auto& line : split_lines(raw_chunk)) {
        if (line.find("data: ") != 0) continue;
        std::string data_str = line.substr(6);
        data_str = trim(data_str);
        if (data_str.empty() || data_str == "[DONE]") continue;

        try {
            json chunk = json::parse(data_str);
            if (!chunk.contains("choices") || chunk["choices"].empty()) continue;

            auto& choice = chunk["choices"][0];
            auto& delta = choice["delta"];

            if (chunk.contains("usage") && !chunk["usage"].is_null()) {
                stream_state_.usage_prompt = chunk["usage"].value("prompt_tokens", stream_state_.usage_prompt);
                stream_state_.usage_completion = chunk["usage"].value("completion_tokens", stream_state_.usage_completion);
            }

            if (!stream_state_.message_started) {
                stream_state_.message_id = chunk.value("id", generate_message_id());
                result += emitMessageStart(gateway_model);
                stream_state_.message_started = true;
            }

            if (delta.contains("role") && delta["role"] == "assistant") {
                continue;
            }

            std::string thinking_delta;
            if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null()) {
                thinking_delta = extract_reasoning_text(delta["reasoning_content"]);
            } else if (delta.contains("reasoning") && !delta["reasoning"].is_null()) {
                thinking_delta = extract_reasoning_text(delta["reasoning"]);
            }

            if (!thinking_delta.empty()) {
                if (stream_state_.active_block_type != "thinking") {
                    result += closeActiveBlock();
                    stream_state_.active_block_type = "thinking";
                    stream_state_.active_block_index = stream_state_.next_content_index++;
                    result += emitThinkingBlockStart(stream_state_.active_block_index);
                }
                result += emitThinkingBlockDelta(stream_state_.active_block_index, thinking_delta);
            }

            if (delta.contains("content") && !delta["content"].is_null()) {
                std::string text = extract_openai_text(delta["content"]);
                if (!text.empty()) {
                    if (stream_state_.active_block_type != "text") {
                        result += closeActiveBlock();
                        stream_state_.active_block_type = "text";
                        stream_state_.active_block_index = stream_state_.next_content_index++;
                        result += emitTextBlockStart(stream_state_.active_block_index);
                    }
                    result += emitTextBlockDelta(stream_state_.active_block_index, text);
                }
            }

            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                int fallback_index = 0;
                for (const auto& tc : delta["tool_calls"]) {
                    int tool_index = tc.value("index", fallback_index++);
                    ToolCallState& tool_state = stream_state_.tool_calls[tool_index];
                    if (tool_state.anthropic_index < 0) {
                        tool_state.anthropic_index = stream_state_.next_content_index++;
                    }

                    if (tc.contains("id") && tc["id"].is_string() && !tc["id"].get<std::string>().empty()) {
                        tool_state.id = tc["id"].get<std::string>();
                    }

                    std::string partial_json;
                    if (tc.contains("function") && tc["function"].is_object()) {
                        const auto& fn = tc["function"];
                        if (fn.contains("name") && fn["name"].is_string() && !fn["name"].get<std::string>().empty()) {
                            tool_state.name = fn["name"].get<std::string>();
                        }
                        if (fn.contains("arguments") && fn["arguments"].is_string()) {
                            partial_json = fn["arguments"].get<std::string>();
                        }
                    }

                    if (!tool_state.started) {
                        result += closeActiveBlock();
                        result += emitToolUseBlockStart(tool_state.anthropic_index, tool_state);
                        tool_state.started = true;
                        tool_state.closed = false;
                        stream_state_.active_block_type = "tool";
                        stream_state_.active_block_index = tool_state.anthropic_index;
                        stream_state_.active_tool_index = tool_index;
                    } else if (stream_state_.active_block_type != "tool" || stream_state_.active_tool_index != tool_index) {
                        result += closeActiveBlock();
                        stream_state_.active_block_type = "tool";
                        stream_state_.active_block_index = tool_state.anthropic_index;
                        stream_state_.active_tool_index = tool_index;
                    }

                    if (!partial_json.empty()) {
                        result += emitInputJsonDelta(tool_state.anthropic_index, partial_json);
                    }
                }
            }

            if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                std::string fr = choice["finish_reason"];
                if (fr == "stop") stream_state_.stop_reason = "end_turn";
                else if (fr == "length") stream_state_.stop_reason = "max_tokens";
                else if (fr == "tool_calls") stream_state_.stop_reason = "tool_use";
                else if (fr == "content_filter") stream_state_.stop_reason = "content_filter";
                else stream_state_.stop_reason = fr;

                if (chunk.contains("usage") && !chunk["usage"].is_null()) {
                    stream_state_.usage_prompt = chunk["usage"].value("prompt_tokens", 0);
                    stream_state_.usage_completion = chunk["usage"].value("completion_tokens", 0);
                }
            }

        } catch (json::parse_error&) {
            // Skip unparseable chunks silently
        }
    }

    return result;
}

// ─── ProviderRouter ─────────────────────────────────────────────────────────

ProviderRouter& ProviderRouter::instance() {
    static ProviderRouter router;
    return router;
}

std::unique_ptr<ProviderBase> ProviderRouter::createProvider(const std::string& type) {
    if (type == "anthropic") return std::make_unique<AnthropicProvider>();
    if (type == "openai") return std::make_unique<OpenAIProvider>();
    return nullptr;
}

std::string ProviderRouter::listModels(const Config& cfg) {
    json resp;
    resp["object"] = "list";
    resp["data"] = json::array();

    time_t now = time(nullptr);
    std::set<std::string> exposed_models;
    for (const auto& [model_id, _] : cfg.models) exposed_models.insert(model_id);
    for (const auto& [alias, _] : cfg.aliases) exposed_models.insert(alias);

    for (const auto& exposed_model : exposed_models) {
        json m;
        m["id"] = exposed_model;
        m["object"] = "model";
        m["created"] = (int)now;
        m["owned_by"] = "gateway";
        resp["data"].push_back(m);
    }

    return resp.dump();
}

std::string ProviderRouter::chat(const Config& cfg,
                                  const std::string& anthropic_body,
                                  int& out_status_code)
{
    out_status_code = 503;
    try {
        json req = json::parse(anthropic_body);
        std::string requested_model = req.value("model", "");
        bool stream = req.value("stream", false);

        ModelConfig model_cfg;
        std::string gateway_model;
        if (!cfg.resolveModel(requested_model, model_cfg, gateway_model)) {
            out_status_code = 400;
            json err;
            err["type"] = "error";
            err["error"] = {{"type", "invalid_request_error"},
                            {"message", "Unknown model: " + requested_model}};
            return err.dump();
        }

        const ProviderConfig* pcfg = cfg.findProvider(model_cfg.provider);
        if (!pcfg) {
            out_status_code = 400;
            json err;
            err["type"] = "error";
            err["error"] = {{"type", "invalid_request_error"},
                            {"message", "Provider not found: " + model_cfg.provider}};
            return err.dump();
        }

        if (pcfg->api_key.empty()) {
            out_status_code = 401;
            json err;
            err["type"] = "error";
            err["error"] = {{"type", "authentication_error"},
                            {"message", "API key not configured for provider: " + pcfg->name}};
            return err.dump();
        }

        const std::string provider_type = model_cfg.protocol.empty() ? pcfg->type : model_cfg.protocol;
        auto provider = createProvider(provider_type);
        if (!provider) {
            out_status_code = 400;
            json err;
            err["type"] = "error";
            err["error"] = {{"type", "invalid_request_error"},
                            {"message", "Unsupported provider type: " + provider_type}};
            return err.dump();
        }

        // Transform request
        req["stream"] = false; // Ensure non-streaming for this path
        std::string backend_body = provider->transformRequest(req, model_cfg);
        std::string url = pcfg->base_url + provider->getEndpoint();
        auto headers = provider->getHeaders(*pcfg);

        // Send to backend
        HttpClient client;
        auto resp = client.post(url, headers, backend_body, "application/json", 120000);
        out_status_code = resp.status_code;

        // Transform response back to Anthropic format
        if (resp.status_code >= 200 && resp.status_code < 300) {
            return provider->transformResponse(resp.body, gateway_model, model_cfg);
        } else {
            // Forward error
            json err;
            err["type"] = "error";
            std::string err_type = "api_error";
            if (resp.status_code == 401) err_type = "authentication_error";
            else if (resp.status_code == 429) err_type = "rate_limit_error";
            else if (resp.status_code == 400) err_type = "invalid_request_error";
            err["error"] = {{"type", err_type},
                            {"message", "Backend error (" + std::to_string(resp.status_code) + ")"}};
            return err.dump();
        }
    } catch (json::parse_error& e) {
        out_status_code = 400;
        json err;
        err["type"] = "error";
        err["error"] = {{"type", "invalid_request_error"},
                        {"message", std::string("Invalid JSON: ") + e.what()}};
        return err.dump();
    } catch (std::exception& e) {
        out_status_code = 500;
        json err;
        err["type"] = "error";
        err["error"] = {{"type", "api_error"},
                        {"message", std::string("Internal error: ") + e.what()}};
        return err.dump();
    }
}

void ProviderRouter::chatStream(const Config& cfg,
                                 const std::string& anthropic_body,
                                 std::shared_ptr<StreamBuffer> out_buf,
                                 int& out_status_code)
{
    out_status_code = 503;
    try {
        json req = json::parse(anthropic_body);
        std::string requested_model = req.value("model", "");
        req["stream"] = true;

        ModelConfig model_cfg;
        std::string gateway_model;
        if (!cfg.resolveModel(requested_model, model_cfg, gateway_model)) {
            out_status_code = 400;
            json err;
            err["type"] = "error";
            err["error"] = {{"type", "invalid_request_error"},
                            {"message", "Unknown model: " + requested_model}};
            std::lock_guard<std::mutex> lock(out_buf->mtx);
            out_buf->error_msg = err.dump();
            out_buf->chunks.push("data: " + err.dump() + "\n\n");
            out_buf->completed = true;
            out_buf->error = true;
            out_buf->cv.notify_one();
            return;
        }

        const ProviderConfig* pcfg = cfg.findProvider(model_cfg.provider);
        if (!pcfg) {
            out_status_code = 400;
            json err;
            err["type"] = "error";
            err["error"] = {{"type", "invalid_request_error"},
                            {"message", "Provider not found: " + model_cfg.provider}};
            std::lock_guard<std::mutex> lock(out_buf->mtx);
            out_buf->error_msg = err.dump();
            out_buf->chunks.push("data: " + err.dump() + "\n\n");
            out_buf->completed = true;
            out_buf->error = true;
            out_buf->cv.notify_one();
            return;
        }

        if (pcfg->api_key.empty()) {
            out_status_code = 401;
            json err;
            err["type"] = "error";
            err["error"] = {{"type", "authentication_error"},
                            {"message", "API key not configured for provider: " + pcfg->name}};
            std::lock_guard<std::mutex> lock(out_buf->mtx);
            out_buf->error_msg = err.dump();
            out_buf->chunks.push("data: " + err.dump() + "\n\n");
            out_buf->completed = true;
            out_buf->error = true;
            out_buf->cv.notify_one();
            return;
        }

        const std::string provider_type = model_cfg.protocol.empty() ? pcfg->type : model_cfg.protocol;
        auto provider = createProvider(provider_type);
        if (!provider) {
            out_status_code = 400;
            json err;
            err["type"] = "error";
            err["error"] = {{"type", "invalid_request_error"},
                            {"message", "Unsupported provider type: " + provider_type}};
            std::lock_guard<std::mutex> lock(out_buf->mtx);
            out_buf->error_msg = err.dump();
            out_buf->chunks.push("data: " + err.dump() + "\n\n");
            out_buf->completed = true;
            out_buf->error = true;
            out_buf->cv.notify_one();
            return;
        }

        const ProviderConfig provider_cfg = *pcfg;
        const std::string backend_body = provider->transformRequest(req, model_cfg);
        const std::string url = provider_cfg.base_url + provider->getEndpoint();
        const auto headers = provider->getHeaders(provider_cfg);

        struct StreamStartState {
            std::mutex mtx;
            std::condition_variable cv;
            bool ready = false;
            int status_code = 503;
        };

        auto start_state = std::make_shared<StreamStartState>();
        auto stream_control = std::make_shared<HttpClient::StreamControl>();
        {
            std::lock_guard<std::mutex> lock(out_buf->mtx);
            out_buf->max_buffered_bytes = kDefaultMaxBufferedStreamBytes;
            out_buf->cancel_upstream = [stream_control]() {
                stream_control->cancel();
            };
        }

        std::thread([
            out_buf,
            start_state,
            stream_control,
            provider = std::move(provider),
            model_cfg,
            gateway_model,
            url,
            headers,
            backend_body
        ]() mutable {
            auto notify_started = [start_state](int status_code) {
                std::lock_guard<std::mutex> lock(start_state->mtx);
                if (!start_state->ready) {
                    start_state->ready = true;
                    start_state->status_code = status_code;
                    start_state->cv.notify_one();
                }
            };

            try {
                std::string raw_response;
                std::string pending_sse_data;

                HttpClient client;
                auto resp = client.postStream(
                    url,
                    headers,
                    backend_body,
                    "application/json",
                    [&](const char* data, size_t len) -> bool {
                        {
                            std::lock_guard<std::mutex> lock(out_buf->mtx);
                            if (out_buf->cancelled) {
                                return false;
                            }
                        }

                        raw_response.append(data, len);
                        pending_sse_data.append(data, len);

                        std::string sse_event;
                        while (take_next_sse_event(pending_sse_data, sse_event)) {
                            bool finished = false;
                            std::string translated = provider->transformStreamChunk(
                                sse_event, gateway_model, model_cfg, finished);
                            if (!push_stream_chunk(out_buf, std::move(translated))) {
                                return false;
                            }
                        }

                        return true;
                    },
                    300000,
                    notify_started,
                    stream_control);

                notify_started(resp.status_code);

                if (resp.status_code < 400 && !trim(pending_sse_data).empty()) {
                    bool finished = false;
                    std::string translated = provider->transformStreamChunk(
                        pending_sse_data, gateway_model, model_cfg, finished);
                    push_stream_chunk(out_buf, std::move(translated));
                }

                bool cancelled = false;
                {
                    std::lock_guard<std::mutex> lock(out_buf->mtx);
                    cancelled = out_buf->cancelled;
                }

                if (resp.status_code >= 400) {
                    finish_stream(out_buf, true, raw_response.empty() ? resp.body : raw_response);
                } else if (cancelled || stream_control->is_cancelled()) {
                    finish_stream(out_buf, false);
                } else {
                    finish_stream(out_buf, false);
                }
            } catch (std::exception& e) {
                notify_started(500);
                json err;
                err["type"] = "error";
                err["error"] = {{"type", "api_error"},
                                {"message", std::string("Stream error: ") + e.what()}};
                finish_stream(out_buf, true, err.dump());
            }
        }).detach();

        {
            std::unique_lock<std::mutex> lock(start_state->mtx);
            start_state->cv.wait(lock, [&]() {
                return start_state->ready;
            });
            out_status_code = start_state->status_code;
        }

        if (out_status_code >= 400) {
            std::unique_lock<std::mutex> lock(out_buf->mtx);
            out_buf->cv.wait(lock, [&]() {
                return out_buf->completed;
            });
        }

    } catch (std::exception& e) {
        out_status_code = 500;
        std::lock_guard<std::mutex> lock(out_buf->mtx);
        json err;
        err["type"] = "error";
        err["error"] = {{"type", "api_error"},
                        {"message", std::string("Stream error: ") + e.what()}};
        out_buf->error_msg = err.dump();
        out_buf->chunks.push("data: " + err.dump() + "\n\n");
        out_buf->completed = true;
        out_buf->error = true;
        out_buf->cv.notify_one();
    }
}
