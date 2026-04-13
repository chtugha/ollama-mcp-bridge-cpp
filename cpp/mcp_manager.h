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
    struct ServerStatus {
        std::string name;
        std::string transport_type;
        bool connected;
        size_t tool_count;
        std::string last_error;
    };

    MCPManager(const std::string& ollama_url = "http://localhost:11434",
               const std::optional<std::string>& system_prompt = std::nullopt);
    ~MCPManager();

    void load_servers(const std::string& config_path);
    std::string call_tool(const std::string& tool_name, const json& arguments);
    void cleanup();

    std::string get_ollama_url() const;
    std::optional<std::string> get_system_prompt() const;
    std::optional<int> get_max_tool_rounds() const;

    void set_ollama_url(const std::string& url);
    void set_system_prompt(const std::optional<std::string>& prompt);
    void set_max_tool_rounds(const std::optional<int>& rounds);

    std::vector<json> get_tools_json() const;
    size_t tools_count() const;

    std::vector<ServerStatus> get_all_server_status() const;
    void add_server(const std::string& name, const json& config);
    void remove_server(const std::string& name);
    void update_server(const std::string& name, const json& config);
    void reconnect_server(const std::string& name);
    void reconnect_all();
    json get_server_config(const std::string& name) const;

private:
    void connect_server(const std::string& name, const json& config);
    void disconnect_server_(const std::string& name);
    void remove_server_tools_(const std::string& name);
    void rebuild_tools_json_cache_();

    std::string ollama_url_;
    std::optional<std::string> system_prompt_;
    std::optional<int> max_tool_rounds_;

    std::map<std::string, std::unique_ptr<McpClient>> clients_;
    std::vector<ToolDefinition> all_tools_;
    std::vector<json> all_tools_json_;
    std::map<std::string, json> server_configs_;
    std::map<std::string, std::string> server_last_errors_;
    std::string config_dir_;
    mutable std::shared_mutex mutex_;
};

}
