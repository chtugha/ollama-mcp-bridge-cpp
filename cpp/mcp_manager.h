#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <nlohmann/json.hpp>

#include "mcp_client.h"

namespace omb {

using json = nlohmann::json;

struct ToolDefinition {
    std::string type = "function";
    std::string qualified_name;
    std::string description;
    json parameters;
    std::string server_name;
    std::string original_name;

    json to_json() const;
};

class MCPManager {
public:
    MCPManager(const std::string& ollama_url = "http://localhost:11434",
               const std::optional<std::string>& system_prompt = std::nullopt);
    ~MCPManager();

    void load_servers(const std::string& config_path);
    std::string call_tool(const std::string& tool_name, const json& arguments);
    void cleanup();

    std::string ollama_url;
    std::optional<std::string> system_prompt;
    std::optional<int> max_tool_rounds;

    std::vector<json> get_tools_json() const;
    size_t tools_count() const;

private:
    void connect_server(const std::string& name, const json& config);
    std::map<std::string, std::unique_ptr<McpClient>> clients_;
    std::vector<ToolDefinition> all_tools_;
    std::vector<json> all_tools_json_;
    mutable std::shared_mutex mutex_;
};

}
