#pragma once

#include <string>
#include <vector>
#include <map>
#include <nlohmann/json.hpp>

#include "mcp_manager.h"
#include "utils.h"

namespace omb {

using json = nlohmann::json;

struct ChatResponse {
    int status = 200;
    json body;
};

class ProxyService {
public:
    explicit ProxyService(MCPManager& mcp_manager);
    ~ProxyService();

    json health_check();
    ChatResponse proxy_chat_with_tools(const json& payload);
    void proxy_chat_with_tools_streaming(const json& payload, StreamCallback callback);

    struct GenericResponse {
        int status = 200;
        std::string body;
        std::map<std::string, std::string> headers;
    };

    GenericResponse proxy_generic_request(const std::string& method, const std::string& path,
                                          const std::map<std::string, std::string>& headers,
                                          const std::string& body,
                                          const std::string& query_string);

private:
    MCPManager& mcp_manager_;

    std::vector<json> maybe_prepend_system_prompt(const std::vector<json>& messages);
    std::vector<json> extract_tool_calls(const json& result);
    std::vector<json> handle_tool_calls(std::vector<json>& messages, const std::vector<json>& tool_calls);
    json make_final_llm_call(const std::string& endpoint, const json& payload, const std::vector<json>& messages);
};

}
