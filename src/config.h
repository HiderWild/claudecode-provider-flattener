#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <json.hpp>

using json = nlohmann::json;

struct ProviderConfig {
    std::string id;
    std::string type;       // "anthropic" or "openai"
    std::string name;
    std::string api_key;
    std::string base_url;
    std::vector<std::string> models;

    json to_json() const {
        json j;
        j["id"] = id;
        j["type"] = type;
        j["name"] = name;
        j["api_key"] = api_key;
        j["base_url"] = base_url;
        j["models"] = models;
        return j;
    }

    static ProviderConfig from_json(const json& j) {
        ProviderConfig c;
        c.id = j.value("id", "");
        c.type = j.value("type", "anthropic");
        c.name = j.value("name", "");
        c.api_key = j.value("api_key", "");
        c.base_url = j.value("base_url", "https://api.anthropic.com");
        if (j.contains("models") && j["models"].is_array()) {
            for (const auto& m : j["models"]) c.models.push_back(m);
        }
        return c;
    }
};

struct ModelCapabilities {
    bool tools = false;
    bool streaming = false;
    bool thinking = false;
    bool adaptive_thinking = false;
    bool interleaved_thinking = false;
    bool count_tokens = false;

    bool anyEnabled() const {
        return tools || streaming || thinking || adaptive_thinking ||
               interleaved_thinking || count_tokens;
    }

    json to_json() const {
        json j;
        j["tools"] = tools;
        j["streaming"] = streaming;
        j["thinking"] = thinking;
        j["adaptive_thinking"] = adaptive_thinking;
        j["interleaved_thinking"] = interleaved_thinking;
        j["count_tokens"] = count_tokens;
        return j;
    }

    static ModelCapabilities from_json(const json& j) {
        ModelCapabilities c;
        c.tools = j.value("tools", false);
        c.streaming = j.value("streaming", false);
        c.thinking = j.value("thinking", false);
        c.adaptive_thinking = j.value("adaptive_thinking", false);
        c.interleaved_thinking = j.value("interleaved_thinking", false);
        c.count_tokens = j.value("count_tokens", false);
        return c;
    }
};

struct ModelRequestOverrides {
    std::string thinking_type;
    std::string effort;

    bool empty() const {
        return thinking_type.empty() && effort.empty();
    }

    json to_json() const {
        json j = json::object();
        if (!thinking_type.empty()) {
            j["thinking"] = {{"type", thinking_type}};
        }
        if (!effort.empty()) {
            j["effort"] = effort;
        }
        return j;
    }

    static ModelRequestOverrides from_json(const json& j) {
        ModelRequestOverrides c;
        if (j.contains("thinking") && j["thinking"].is_object()) {
            c.thinking_type = j["thinking"].value("type", "");
        }
        c.effort = j.value("effort", "");
        return c;
    }
};

struct ModelConfig {
    std::string id;
    std::string provider;
    std::string upstream_model;
    std::string protocol;
    std::string thinking;
    std::string effort;
    std::string role;
    ModelCapabilities capabilities;
    ModelRequestOverrides request_overrides;

    json to_json() const {
        json j;
        j["id"] = id;
        j["provider"] = provider;
        j["upstream_model"] = upstream_model;
        if (!protocol.empty()) j["protocol"] = protocol;
        if (!thinking.empty()) j["thinking"] = thinking;
        if (!effort.empty()) j["effort"] = effort;
        if (!role.empty()) j["role"] = role;
        if (capabilities.anyEnabled()) j["capabilities"] = capabilities.to_json();
        if (!request_overrides.empty()) j["request_overrides"] = request_overrides.to_json();
        return j;
    }

    static ModelConfig from_json(const json& j, const std::string& default_id = "") {
        ModelConfig c;
        c.id = j.value("id", default_id);
        c.provider = j.value("provider", "");
        c.upstream_model = j.value("upstream_model", "");
        c.protocol = j.value("protocol", "");
        c.thinking = j.value("thinking", "");
        c.effort = j.value("effort", "");
        c.role = j.value("role", "");
        if (j.contains("capabilities") && j["capabilities"].is_object()) {
            c.capabilities = ModelCapabilities::from_json(j["capabilities"]);
        }
        if (j.contains("request_overrides") && j["request_overrides"].is_object()) {
            c.request_overrides = ModelRequestOverrides::from_json(j["request_overrides"]);
        }
        if (c.request_overrides.thinking_type.empty()) {
            c.request_overrides.thinking_type = c.thinking;
        }
        if (c.request_overrides.effort.empty()) {
            c.request_overrides.effort = c.effort;
        }
        return c;
    }
};

struct Config {
    int port = 8080;
    std::string bind = "127.0.0.1";
    int thread_pool_size = 8;
    std::string admin_token;
    std::vector<ProviderConfig> providers;
    std::map<std::string, ModelConfig> models;
    std::map<std::string, std::string> aliases;

    static std::string getConfigDir() {
        const char* home = getenv("USERPROFILE");
        if (!home) home = getenv("HOME");
        if (!home) home = ".";
        return std::string(home) + "/.claude/model-gateway";
    }

    static std::string getConfigPath() {
        return getConfigDir() + "/config.json";
    }

    static Config load() {
        Config cfg;
        std::string path = getConfigPath();
        std::ifstream f(path);
        if (!f.good()) {
            cfg.providers.push_back({
                "anthropic-default", "anthropic", "Anthropic",
                "", "https://api.anthropic.com",
                {"claude-sonnet-4-20250514", "claude-3.5-haiku-latest"}
            });
            cfg.models["claude-sonnet-4-20250514"] = {
                "claude-sonnet-4-20250514",
                "anthropic-default",
                "claude-sonnet-4-20250514",
                "anthropic",
                "",
                "",
                "best"
            };
            cfg.models["claude-3.5-haiku-latest"] = {
                "claude-3.5-haiku-latest",
                "anthropic-default",
                "claude-3.5-haiku-latest",
                "anthropic",
                "",
                "",
                "fast"
            };
            cfg.aliases["claude-sonnet"] = "claude-sonnet-4-20250514";
            cfg.aliases["claude-haiku"] = "claude-3.5-haiku-latest";
            cfg.save();
            return cfg;
        }
        try {
            json j;
            f >> j;
            cfg.port = j.value("port", 8080);
            cfg.bind = j.value("bind", "127.0.0.1");
            cfg.thread_pool_size = j.value("thread_pool_size", 8);
            cfg.admin_token = j.value("admin_token", "");
            if (cfg.thread_pool_size < 0) {
                std::cerr << "[config] invalid thread_pool_size: " << cfg.thread_pool_size
                          << " — using default 8" << std::endl;
                cfg.thread_pool_size = 8;
            }
            if (j.contains("providers") && j["providers"].is_array()) {
                for (const auto& p : j["providers"]) {
                    cfg.providers.push_back(ProviderConfig::from_json(p));
                }
            }

            if (j.contains("models") && j["models"].is_object()) {
                for (auto& [k, v] : j["models"].items()) {
                    ModelConfig model = ModelConfig::from_json(v, k);
                    if (model.id.empty()) model.id = k;
                    cfg.models[model.id] = model;
                }
            }

            if (j.contains("aliases") && j["aliases"].is_object()) {
                for (auto& [k, v] : j["aliases"].items()) {
                    if (v.is_string()) cfg.aliases[k] = v.get<std::string>();
                }
            }

            if (cfg.models.empty() && j.contains("model_aliases") && j["model_aliases"].is_object()) {
                for (auto& [k, v] : j["model_aliases"].items()) {
                    if (!v.is_string()) continue;
                    const std::string target = v.get<std::string>();
                    auto colon = target.find(':');
                    if (colon == std::string::npos) continue;

                    ModelConfig model;
                    model.id = k;
                    model.provider = target.substr(0, colon);
                    model.upstream_model = target.substr(colon + 1);
                    cfg.models[model.id] = model;
                    cfg.aliases[k] = model.id;
                }
            }

            if (cfg.aliases.empty()) {
                for (const auto& [model_id, _] : cfg.models) {
                    cfg.aliases[model_id] = model_id;
                }
            }
        } catch (std::exception& e) {
            std::cerr << "[config] parse error: " << e.what()
                      << " — using defaults" << std::endl;
        }
        return cfg;
    }

    void save() const {
        std::filesystem::create_directories(getConfigDir());
        json j;
        j["port"] = port;
        j["bind"] = bind;
        j["thread_pool_size"] = thread_pool_size;
        if (!admin_token.empty()) j["admin_token"] = admin_token;
        j["providers"] = json::array();
        for (const auto& p : providers) j["providers"].push_back(p.to_json());
        j["models"] = json::object();
        for (const auto& [id, model] : models) j["models"][id] = model.to_json();
        j["aliases"] = json::object();
        for (const auto& [alias, model_id] : aliases) j["aliases"][alias] = model_id;
        j["model_aliases"] = json::object();
        for (const auto& [alias, model_id] : aliases) {
            const ModelConfig* model = findModel(model_id);
            if (!model) continue;
            j["model_aliases"][alias] = model->provider + ":" + model->upstream_model;
        }
        std::ofstream f(getConfigPath());
        f << j.dump(2) << std::endl;
    }

    bool resolveModel(const std::string& requested_model,
                      ModelConfig& out_model,
                      std::string& out_gateway_model) const {
        auto model_it = models.find(requested_model);
        if (model_it != models.end()) {
            out_model = model_it->second;
            out_gateway_model = requested_model;
            return true;
        }

        auto alias_it = aliases.find(requested_model);
        if (alias_it == aliases.end()) return false;

        model_it = models.find(alias_it->second);
        if (model_it == models.end()) return false;

        out_model = model_it->second;
        out_gateway_model = requested_model;
        return true;
    }

    const ModelConfig* findModel(const std::string& id) const {
        auto it = models.find(id);
        return it == models.end() ? nullptr : &it->second;
    }

    ModelConfig* findModelMut(const std::string& id) {
        auto it = models.find(id);
        return it == models.end() ? nullptr : &it->second;
    }

    // Find provider by id
    const ProviderConfig* findProvider(const std::string& id) const {
        for (const auto& p : providers) {
            if (p.id == id) return &p;
        }
        return nullptr;
    }

    ProviderConfig* findProviderMut(const std::string& id) {
        for (auto& p : providers) {
            if (p.id == id) return &p;
        }
        return nullptr;
    }
};
