#pragma once
#include "config.h"
#include "http_client.h"
#include <json.hpp>
#include <functional>
#include <memory>
#include <random>

using json = nlohmann::json;

struct GatewayRequestContext {
    std::map<std::string, std::string> forwarded_headers;
    std::string session_id;
};

// ─── Utility ────────────────────────────────────────────────────────────────
std::string generate_message_id();
std::string json_string_or(const json& j, const std::string& key, const std::string& def);

// ─── Provider base ──────────────────────────────────────────────────────────
class ProviderBase {
public:
    virtual ~ProviderBase() = default;
    virtual std::string getType() const = 0;

    // Get API endpoint path for this provider
    virtual std::string getEndpoint() const = 0;

    // Build headers for the backend API call
    virtual std::map<std::string, std::string> getHeaders(const ProviderConfig& cfg) const = 0;

    // Transform incoming Anthropic-format body → backend body
    virtual std::string transformRequest(const json& anthropic_req,
                                                        const ModelConfig& model_cfg) = 0;

    // Transform backend response → Anthropic format (non-streaming)
    virtual std::string transformResponse(const std::string& backend_body,
                                           const std::string& gateway_model,
                                                         const ModelConfig& model_cfg) = 0;

    // Transform Streaming: process a raw chunk from the backend and
    // return Anthropic-format SSE text to forward to the client.
    // Returns empty string if chunk should be skipped internally.
    // chunk_finished signals no more chunks (e.g. [DONE] sentinel).
    virtual std::string transformStreamChunk(const std::string& raw_chunk,
                                              const std::string& gateway_model,
                                                             const ModelConfig& model_cfg,
                                              bool& chunk_finished) = 0;

    // Create default provider config with empty api_key
    virtual ProviderConfig defaultConfig() const = 0;
};

// ─── Anthropic Provider (near-passthrough) ──────────────────────────────────
class AnthropicProvider : public ProviderBase {
public:
    std::string getType() const override { return "anthropic"; }
    std::string getEndpoint() const override { return "/v1/messages"; }

    std::map<std::string, std::string> getHeaders(const ProviderConfig& cfg) const override;

    std::string transformRequest(const json& anthropic_req,
                                             const ModelConfig& model_cfg) override;

    std::string transformResponse(const std::string& backend_body,
                                   const std::string& gateway_model,
                                              const ModelConfig& model_cfg) override;

    std::string transformStreamChunk(const std::string& raw_chunk,
                                      const std::string& gateway_model,
                                                  const ModelConfig& model_cfg,
                                      bool& chunk_finished) override;

    ProviderConfig defaultConfig() const override;
};

// ─── OpenAI Provider (protocol translation) ─────────────────────────────────
class OpenAIProvider : public ProviderBase {
public:
    std::string getType() const override { return "openai"; }
    std::string getEndpoint() const override { return "/chat/completions"; }

    std::map<std::string, std::string> getHeaders(const ProviderConfig& cfg) const override;

    std::string transformRequest(const json& anthropic_req,
                                             const ModelConfig& model_cfg) override;

    std::string transformResponse(const std::string& backend_body,
                                   const std::string& gateway_model,
                                              const ModelConfig& model_cfg) override;

    std::string transformStreamChunk(const std::string& raw_chunk,
                                      const std::string& gateway_model,
                                                  const ModelConfig& model_cfg,
                                      bool& chunk_finished) override;

    ProviderConfig defaultConfig() const override;

private:
    struct ToolCallState {
        bool started = false;
        bool closed = false;
        int anthropic_index = -1;
        std::string id;
        std::string name;
    };

    // Streaming state — tracks where we are in the OpenAI → Anthropic translation
    struct StreamState {
        bool message_started = false;
        std::string message_id;
        std::string stop_reason;
        int usage_prompt = 0;
        int usage_completion = 0;
        int next_content_index = 0;
        std::string active_block_type;
        int active_block_index = -1;
        int active_tool_index = -1;
        std::map<int, ToolCallState> tool_calls;
    };
    StreamState stream_state_;
    void resetStreamState();

    std::string emitMessageStart(const std::string& gateway_model);
    std::string emitTextBlockStart(int index);
    std::string emitTextBlockDelta(int index, const std::string& text);
    std::string emitThinkingBlockStart(int index);
    std::string emitThinkingBlockDelta(int index, const std::string& thinking);
    std::string emitSignatureDelta(int index, const std::string& signature);
    std::string emitToolUseBlockStart(int index, const ToolCallState& tool_state);
    std::string emitInputJsonDelta(int index, const std::string& partial_json);
    std::string emitContentBlockStop(int index);
    std::string emitMessageDelta();
    std::string emitMessageStop();
    std::string closeActiveBlock();
    std::string closeAllToolBlocks();
};

// ─── Provider Router ────────────────────────────────────────────────────────
class ProviderRouter {
public:
    static ProviderRouter& instance();

    // Send a non-streaming chat request
    // anthropic_body: the raw JSON body in Anthropic Messages API format
    // Returns: Anthropic-format response JSON as string
    std::string chat(const Config& cfg,
                     const std::string& anthropic_body,
                     const GatewayRequestContext& request_context,
                     int& out_status_code);

    // Count input tokens for an Anthropic-format request.
    std::string countTokens(const Config& cfg,
                            const std::string& anthropic_body,
                            const GatewayRequestContext& request_context,
                            int& out_status_code);

    // Stream a chat request via shared StreamBuffer
    // Each chunk in the buffer is Anthropic-format SSE text
    void chatStream(const Config& cfg,
                    const std::string& anthropic_body,
                    const GatewayRequestContext& request_context,
                    std::shared_ptr<StreamBuffer> out_buf,
                    int& out_status_code);

    // List available models from configured aliases and internal models
    std::string listModels(const Config& cfg);

private:
    ProviderRouter() = default;

    std::unique_ptr<ProviderBase> createProvider(const std::string& type);
};
