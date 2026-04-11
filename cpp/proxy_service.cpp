#include "proxy_service.h"
#include "utils.h"

#include <spdlog/spdlog.h>
#include <sstream>

namespace omb {

ProxyService::ProxyService(MCPManager& mcp_manager)
    : mcp_manager_(mcp_manager) {}

ProxyService::~ProxyService() {
    cleanup();
}

std::vector<json> ProxyService::maybe_prepend_system_prompt(const std::vector<json>& messages) {
    if (!mcp_manager_.system_prompt) return messages;

    if (messages.empty() || messages[0].value("role", "") != "system") {
        std::vector<json> result;
        result.push_back({{"role", "system"}, {"content", *mcp_manager_.system_prompt}});
        result.insert(result.end(), messages.begin(), messages.end());
        return result;
    }
    return messages;
}

json ProxyService::health_check() {
    bool ollama_healthy = check_ollama_health(mcp_manager_.ollama_url);
    return {
        {"status", ollama_healthy ? "healthy" : "degraded"},
        {"ollama_status", ollama_healthy ? "running" : "not accessible"},
        {"tools", static_cast<int>(mcp_manager_.tools_count())}
    };
}

std::vector<json> ProxyService::extract_tool_calls(const json& result) {
    std::vector<json> tool_calls;
    if (result.contains("message") && result["message"].contains("tool_calls")) {
        auto& tc = result["message"]["tool_calls"];
        if (tc.is_array()) {
            for (auto& call : tc) {
                tool_calls.push_back(call);
            }
            if (!tool_calls.empty()) {
                spdlog::debug("Extracted {} tool_calls from response", tool_calls.size());
            }
        }
    }
    return tool_calls;
}

std::vector<json> ProxyService::handle_tool_calls(std::vector<json>& messages,
                                                   const std::vector<json>& tool_calls) {
    for (auto& tool_call : tool_calls) {
        std::string tool_name = tool_call["function"]["name"].get<std::string>();
        json arguments = tool_call["function"]["arguments"];
        std::string tool_result = mcp_manager_.call_tool(tool_name, arguments);
        spdlog::debug("Tool {} called with args {}, result: {}", tool_name, arguments.dump(), tool_result);
        messages.push_back({
            {"role", "tool"},
            {"tool_name", tool_name},
            {"content", tool_result}
        });
    }
    return messages;
}

json ProxyService::make_final_llm_call(const std::string& endpoint, const json& payload,
                                        const std::vector<json>& messages) {
    json final_payload = payload;
    final_payload["stream"] = false;
    final_payload["messages"] = messages;
    final_payload["tools"] = nullptr;

    std::string url = mcp_manager_.ollama_url + endpoint;
    std::string response_body = http_post(url, final_payload.dump());

    if (response_body.empty()) {
        return {{"error", "Failed to get response from Ollama"}};
    }

    try {
        return json::parse(response_body);
    } catch (...) {
        return {{"error", "Failed to parse Ollama response"}};
    }
}

ChatResponse ProxyService::proxy_chat_with_tools(const json& payload) {
    if (!check_ollama_health(mcp_manager_.ollama_url)) {
        return {503, {{"error", "Ollama server not accessible"}}};
    }

    json working_payload = payload;
    working_payload["stream"] = false;
    auto tools_json = mcp_manager_.get_tools_json();
    if (!tools_json.empty()) {
        working_payload["tools"] = tools_json;
    } else {
        working_payload["tools"] = nullptr;
    }

    std::vector<json> messages;
    if (working_payload.contains("messages") && working_payload["messages"].is_array()) {
        for (auto& m : working_payload["messages"]) {
            messages.push_back(m);
        }
    }
    messages = maybe_prepend_system_prompt(messages);

    int max_rounds = mcp_manager_.max_tool_rounds.value_or(0);
    int current_round = 0;

    while (true) {
        json current_payload = working_payload;
        current_payload["messages"] = messages;

        std::string url = mcp_manager_.ollama_url + "/api/chat";
        std::string response_body = http_post(url, current_payload.dump());

        if (response_body.empty()) {
            return {503, {{"error", "Failed to connect to Ollama server"}}};
        }

        json result;
        try {
            result = json::parse(response_body);
        } catch (...) {
            return {500, {{"error", "Failed to parse Ollama response"}}};
        }

        auto tool_calls = extract_tool_calls(result);
        if (tool_calls.empty()) {
            return {200, result};
        }

        std::string response_content;
        if (result.contains("message") && result["message"].contains("content")) {
            response_content = result["message"]["content"].get<std::string>();
        }

        json assistant_msg = {
            {"role", "assistant"},
            {"content", response_content},
            {"tool_calls", tool_calls}
        };
        messages.push_back(assistant_msg);
        handle_tool_calls(messages, tool_calls);

        current_round++;
        if (max_rounds > 0 && current_round >= max_rounds) {
            spdlog::warn("Reached maximum tool execution rounds ({}), making final LLM call with tool results", max_rounds);
            json final_result = make_final_llm_call("/api/chat", working_payload, messages);
            return {200, final_result};
        }
    }
}

void ProxyService::proxy_chat_with_tools_streaming(const json& payload, StreamCallback callback) {
    if (!check_ollama_health(mcp_manager_.ollama_url)) {
        json error = {{"error", "Ollama server not accessible"}};
        callback(error.dump() + "\n");
        return;
    }

    json working_payload = payload;
    auto tools_json = mcp_manager_.get_tools_json();
    if (!tools_json.empty()) {
        working_payload["tools"] = tools_json;
    } else {
        working_payload["tools"] = nullptr;
    }

    std::vector<json> messages;
    if (working_payload.contains("messages") && working_payload["messages"].is_array()) {
        for (auto& m : working_payload["messages"]) {
            messages.push_back(m);
        }
    }
    messages = maybe_prepend_system_prompt(messages);

    int max_rounds = mcp_manager_.max_tool_rounds.value_or(0);
    int current_round = 0;

    while (true) {
        json current_payload = working_payload;
        current_payload["messages"] = messages;

        std::string url = mcp_manager_.ollama_url + "/api/chat";
        std::string accumulated;
        std::vector<json> tool_calls;
        std::string response_text;
        bool done = false;

        http_post_stream(url, current_payload.dump(), [&](const std::string& chunk) {
            accumulated += chunk;
            std::string line;
            while (true) {
                auto pos = accumulated.find('\n');
                if (pos == std::string::npos) break;
                line = accumulated.substr(0, pos);
                accumulated = accumulated.substr(pos + 1);

                if (line.empty()) continue;

                try {
                    json json_obj = json::parse(line);
                    callback(json_obj.dump() + "\n");

                    auto extracted = extract_tool_calls(json_obj);
                    if (!extracted.empty()) {
                        tool_calls = extracted;
                    }

                    if (json_obj.value("done", false)) {
                        if (json_obj.contains("message") && json_obj["message"].contains("content")) {
                            response_text = json_obj["message"]["content"].get<std::string>();
                        }
                        if (!extracted.empty()) {
                            tool_calls = extracted;
                        }
                        done = true;
                    }
                } catch (const json::parse_error& e) {
                    spdlog::debug("Error parsing NDJSON line: {}", e.what());
                }
            }
        });

        if (!accumulated.empty()) {
            try {
                json json_obj = json::parse(accumulated);
                callback(json_obj.dump() + "\n");
                auto extracted = extract_tool_calls(json_obj);
                if (!extracted.empty()) tool_calls = extracted;
            } catch (...) {}
        }

        if (tool_calls.empty()) {
            break;
        }

        json assistant_msg = {
            {"role", "assistant"},
            {"content", response_text},
            {"tool_calls", tool_calls}
        };
        messages.push_back(assistant_msg);
        handle_tool_calls(messages, tool_calls);

        current_round++;
        if (max_rounds > 0 && current_round >= max_rounds) {
            spdlog::warn("Reached maximum tool execution rounds ({}), making final LLM call with tool results", max_rounds);

            json final_payload = working_payload;
            final_payload["messages"] = messages;
            final_payload["tools"] = nullptr;

            std::string final_url = mcp_manager_.ollama_url + "/api/chat";
            http_post_stream(final_url, final_payload.dump(), [&](const std::string& chunk) {
                callback(chunk);
            });
            break;
        }
    }
}

ProxyService::GenericResponse ProxyService::proxy_generic_request(
    const std::string& method, const std::string& path,
    const std::map<std::string, std::string>& headers,
    const std::string& body, const std::string& query_string) {

    std::string url = mcp_manager_.ollama_url + "/" + path;
    if (!query_string.empty()) {
        url += "?" + query_string;
    }

    std::map<std::string, std::string> forward_headers;
    for (auto& [k, v] : headers) {
        std::string lower_k = k;
        std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(), ::tolower);
        if (lower_k != "host") {
            forward_headers[k] = v;
        }
    }

    try {
        auto response = http_request(method, url, forward_headers, body);
        return {response.status, response.body, response.headers};
    } catch (const std::exception& e) {
        spdlog::error("Proxy failed for {}: {}", path, e.what());
        return {503, json({{"error", std::string("Could not connect to target server: ") + e.what()}}).dump(), {}};
    }
}

void ProxyService::cleanup() {
}

}
