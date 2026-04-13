#include "mcp_manager.h"
#include "utils.h"

#include <fstream>
#include <filesystem>
#include <algorithm>
#include <regex>
#include <spdlog/spdlog.h>

namespace omb {

json ToolDefinition::to_json() const {
    return {
        {"type", type},
        {"function", {
            {"name", qualified_name},
            {"description", description},
            {"parameters", parameters}
        }}
    };
}

MCPManager::MCPManager(const std::string& ollama_url,
                       const std::optional<std::string>& system_prompt)
    : ollama_url_(ollama_url), system_prompt_(system_prompt) {}

MCPManager::~MCPManager() {
    cleanup();
}

void MCPManager::load_servers(const std::string& config_path) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    std::string config_dir = std::filesystem::absolute(config_path).parent_path().string();

    std::ifstream file(config_path);
    if (!file.is_open()) {
        spdlog::error("Config file not found: {}", config_path);
        throw std::runtime_error("Config file not found: " + config_path);
    }

    json config;
    try {
        file >> config;
    } catch (const json::parse_error& e) {
        spdlog::error("Failed to parse config file '{}': {}", config_path, e.what());
        throw std::runtime_error("Invalid JSON in config file '" + config_path + "': " + e.what());
    }

    if (!config.contains("mcpServers")) {
        spdlog::error("Config file '{}' missing 'mcpServers' key", config_path);
        throw std::runtime_error("Config file '" + config_path + "' missing 'mcpServers' key");
    }

    config_dir_ = config_dir;
    for (auto& [name, server_config] : config["mcpServers"].items()) {
        server_configs_[name] = server_config;
        json resolved = server_config;
        resolved["cwd"] = config_dir;
        connect_server(name, resolved);
    }
}

// ---------------------------------------------------------------------------
// ConnectResult — returned by make_connection(); holds a ready-to-use client
// and the ToolDefinitions built from it.  All fields are filled lock-free.
// ---------------------------------------------------------------------------

struct ConnectResult {
    std::unique_ptr<McpClient> client; // null on error
    std::vector<ToolDefinition> tools;
    std::string error;                 // non-empty on error
};

// make_connection() does ALL network/process I/O and returns a ConnectResult.
// It must NOT access any MCPManager member — it is called WITHOUT the lock.
static ConnectResult make_connection(const std::string& name, const json& config) {
    ConnectResult out;
    try {
        json tool_filter = config.value("toolFilter", json::object());
        if (!tool_filter.empty()) {
            std::string mode = tool_filter.value("mode", "include");
            if (mode != "include" && mode != "exclude") {
                out.error = "Invalid toolFilter mode '" + mode + "' for server '" + name + "'. Must be 'include' or 'exclude'.";
                spdlog::error("{}", out.error);
                return out;
            }
        }

        std::string cwd = config.value("cwd", std::filesystem::current_path().string());
        json resolved_config = expand_dict_env_vars(config, cwd);

        std::unique_ptr<McpTransport> transport;

        if (resolved_config.contains("command")) {
            std::string command = resolved_config["command"].get<std::string>();
            std::vector<std::string> args;
            if (resolved_config.contains("args") && resolved_config["args"].is_array()) {
                for (auto& a : resolved_config["args"]) {
                    args.push_back(a.get<std::string>());
                }
            }
            std::map<std::string, std::string> env;
            if (resolved_config.contains("env") && resolved_config["env"].is_object()) {
                for (auto& [k, v] : resolved_config["env"].items()) {
                    env[k] = v.get<std::string>();
                }
            }
            std::string server_cwd = resolved_config.value("cwd", "");
            transport = std::make_unique<StdioTransport>(command, args, env, server_cwd);
        } else if (resolved_config.contains("url")) {
            std::string url = resolved_config["url"].get<std::string>();
            std::map<std::string, std::string> headers;
            if (resolved_config.contains("headers") && resolved_config["headers"].is_object()) {
                for (auto& [k, v] : resolved_config["headers"].items()) {
                    headers[k] = v.get<std::string>();
                }
            }

            std::string trimmed_url = url;
            while (!trimmed_url.empty() && trimmed_url.back() == '/') trimmed_url.pop_back();
            std::string path = url_path(trimmed_url);
            auto qs_pos = path.find('?');
            if (qs_pos != std::string::npos) path = path.substr(0, qs_pos);
            if (path.size() >= 4 && path.substr(path.size() - 4) == "/sse") {
                transport = std::make_unique<SSETransport>(url, headers);
            } else {
                transport = std::make_unique<StreamableHTTPTransport>(url, headers);
            }
        } else {
            out.error = "Invalid MCP server config for '" + name + "': must have 'command' or 'url'";
            spdlog::error("{}", out.error);
            return out;
        }

        auto client = std::make_unique<McpClient>(std::move(transport));
        if (!client->initialize()) {
            out.error = "Failed to initialize MCP server '" + name + "'";
            spdlog::error("{}", out.error);
            return out;
        }

        auto tools = client->list_tools();

        std::vector<std::string> filter_tools;
        std::string filter_mode = "include";
        if (!tool_filter.empty()) {
            filter_mode = tool_filter.value("mode", "include");
            if (tool_filter.contains("tools") && tool_filter["tools"].is_array()) {
                for (auto& t : tool_filter["tools"]) {
                    filter_tools.push_back(t.get<std::string>());
                }
            }
        }

        std::vector<McpTool> filtered_tools;
        std::vector<std::string> all_tool_names;
        for (auto& t : tools) all_tool_names.push_back(t.name);

        if (!filter_tools.empty()) {
            if (filter_mode == "include") {
                for (auto& tool : tools) {
                    if (std::find(filter_tools.begin(), filter_tools.end(), tool.name) != filter_tools.end()) {
                        filtered_tools.push_back(tool);
                    }
                }
                std::vector<std::string> missing;
                for (auto& ft : filter_tools) {
                    if (std::find(all_tool_names.begin(), all_tool_names.end(), ft) == all_tool_names.end()) {
                        missing.push_back(ft);
                    }
                }
                if (!missing.empty()) {
                    std::string missing_str;
                    for (size_t i = 0; i < missing.size(); i++) {
                        if (i > 0) missing_str += ", ";
                        missing_str += missing[i];
                    }
                    spdlog::warn("Server '{}': tools not found in filter [{}]", name, missing_str);
                }
            } else {
                for (auto& tool : tools) {
                    if (std::find(filter_tools.begin(), filter_tools.end(), tool.name) == filter_tools.end()) {
                        filtered_tools.push_back(tool);
                    }
                }
            }
        } else {
            filtered_tools = tools;
        }

        for (auto& tool : filtered_tools) {
            ToolDefinition td;
            td.qualified_name = name + "." + tool.name;
            td.description = tool.description;
            td.parameters = tool.input_schema;
            td.server_name = name;
            td.original_name = tool.name;
            out.tools.push_back(td);
        }

        out.client = std::move(client);

        if (!filter_tools.empty()) {
            spdlog::info("Connected to '{}' with {}/{} tools ({})",
                         name, filtered_tools.size(), tools.size(),
                         filter_mode == "include"
                             ? std::to_string(tools.size() - filtered_tools.size()) + " filtered"
                             : std::to_string(tools.size() - filtered_tools.size()) + " excluded");
        } else {
            spdlog::info("Connected to '{}' with {} tools", name, tools.size());
        }

        if (!filtered_tools.empty()) {
            std::string tool_names;
            for (size_t i = 0; i < filtered_tools.size(); i++) {
                if (i > 0) tool_names += ", ";
                tool_names += filtered_tools[i].name;
            }
            spdlog::info("Server '{}': available tools [{}]", name, tool_names);
        }

    } catch (const std::exception& e) {
        out.client.reset();
        out.tools.clear();
        out.error = e.what();
        spdlog::error("Failed to connect to MCP server '{}': {}", name, e.what());
    }
    return out;
}

// connect_server() is called under the write lock from load_servers() only
// (single-threaded startup path).  Management API calls use the
// snapshot→unlock→make_connection→relock pattern instead.
void MCPManager::connect_server(const std::string& name, const json& config) {
    auto result = make_connection(name, config);
    if (!result.error.empty()) {
        server_last_errors_[name] = result.error;
        return;
    }
    clients_[name] = std::move(result.client);
    for (auto& td : result.tools) {
        all_tools_.push_back(td);
        all_tools_json_.push_back(td.to_json());
    }
    server_last_errors_.erase(name);
}

void MCPManager::rebuild_tools_json_cache_() {
    all_tools_json_.clear();
    for (auto& td : all_tools_) {
        all_tools_json_.push_back(td.to_json());
    }
}

void MCPManager::remove_server_tools_(const std::string& name) {
    auto it = std::remove_if(all_tools_.begin(), all_tools_.end(),
        [&name](const ToolDefinition& td) { return td.server_name == name; });
    all_tools_.erase(it, all_tools_.end());
    rebuild_tools_json_cache_();
}

void MCPManager::disconnect_server_(const std::string& name) {
    auto it = clients_.find(name);
    if (it != clients_.end()) {
        try { it->second->close(); } catch (...) {}
        clients_.erase(it);
    }
    remove_server_tools_(name);
}

std::vector<MCPManager::ServerStatus> MCPManager::get_all_server_status() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<ServerStatus> result;

    for (auto& [name, config] : server_configs_) {
        ServerStatus status;
        status.name = name;

        if (config.contains("command")) {
            status.transport_type = "stdio";
        } else if (config.contains("url")) {
            std::string trimmed = config["url"].get<std::string>();
            while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
            std::string path = url_path(trimmed);
            auto qs_pos = path.find('?');
            if (qs_pos != std::string::npos) path = path.substr(0, qs_pos);
            if (path.size() >= 4 && path.substr(path.size() - 4) == "/sse") {
                status.transport_type = "sse";
            } else {
                status.transport_type = "http";
            }
        } else {
            status.transport_type = "unknown";
        }

        auto client_it = clients_.find(name);
        status.connected = (client_it != clients_.end()) && client_it->second->is_connected();

        status.tool_count = 0;
        for (auto& td : all_tools_) {
            if (td.server_name == name) status.tool_count++;
        }

        auto err_it = server_last_errors_.find(name);
        if (err_it != server_last_errors_.end()) {
            status.last_error = err_it->second;
        }

        result.push_back(status);
    }

    return result;
}

void MCPManager::add_server(const std::string& name, const json& config) {
    static const std::regex name_regex("^[a-zA-Z0-9._-]+$");
    if (!std::regex_match(name, name_regex)) {
        throw std::invalid_argument("Invalid server name '" + name + "': must match ^[a-zA-Z0-9._-]+$");
    }

    json resolved;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (server_configs_.count(name)) {
            throw std::runtime_error("Server '" + name + "' already exists");
        }
        server_configs_[name] = config;
        resolved = config;
        if (!config_dir_.empty() && !resolved.contains("cwd")) {
            resolved["cwd"] = config_dir_;
        }
    }

    auto result = make_connection(name, resolved);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!server_configs_.count(name)) {
        return;
    }
    if (!result.error.empty()) {
        server_last_errors_[name] = result.error;
        return;
    }
    clients_[name] = std::move(result.client);
    for (auto& td : result.tools) {
        all_tools_.push_back(td);
        all_tools_json_.push_back(td.to_json());
    }
    server_last_errors_.erase(name);
}

void MCPManager::remove_server(const std::string& name) {
    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (!server_configs_.count(name)) {
        throw std::runtime_error("Server '" + name + "' not found");
    }

    disconnect_server_(name);
    server_configs_.erase(name);
    server_last_errors_.erase(name);
}

void MCPManager::update_server(const std::string& name, const json& config) {
    json resolved;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (!server_configs_.count(name)) {
            throw std::runtime_error("Server '" + name + "' not found");
        }
        disconnect_server_(name);
        server_configs_[name] = config;
        resolved = config;
        if (!config_dir_.empty() && !resolved.contains("cwd")) {
            resolved["cwd"] = config_dir_;
        }
    }

    auto result = make_connection(name, resolved);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!server_configs_.count(name)) {
        return;
    }
    if (!result.error.empty()) {
        server_last_errors_[name] = result.error;
        return;
    }
    clients_[name] = std::move(result.client);
    auto it = std::remove_if(all_tools_.begin(), all_tools_.end(),
        [&name](const ToolDefinition& td) { return td.server_name == name; });
    all_tools_.erase(it, all_tools_.end());
    for (auto& td : result.tools) {
        all_tools_.push_back(td);
    }
    rebuild_tools_json_cache_();
    server_last_errors_.erase(name);
}

void MCPManager::reconnect_server(const std::string& name) {
    json resolved;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto cfg_it = server_configs_.find(name);
        if (cfg_it == server_configs_.end()) {
            throw std::runtime_error("Server '" + name + "' not found");
        }
        resolved = cfg_it->second;
        if (!config_dir_.empty() && !resolved.contains("cwd")) {
            resolved["cwd"] = config_dir_;
        }
        disconnect_server_(name);
    }

    auto result = make_connection(name, resolved);

    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (!server_configs_.count(name)) {
        return;
    }
    if (!result.error.empty()) {
        server_last_errors_[name] = result.error;
        return;
    }
    clients_[name] = std::move(result.client);
    for (auto& td : result.tools) {
        all_tools_.push_back(td);
    }
    rebuild_tools_json_cache_();
    server_last_errors_.erase(name);
}

void MCPManager::reconnect_all() {
    std::vector<std::pair<std::string, json>> entries;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto& [n, raw] : server_configs_) {
            json resolved = raw;
            if (!config_dir_.empty() && !resolved.contains("cwd")) {
                resolved["cwd"] = config_dir_;
            }
            disconnect_server_(n);
            entries.emplace_back(n, std::move(resolved));
        }
        all_tools_.clear();
        all_tools_json_.clear();
    }

    std::vector<std::pair<std::string, ConnectResult>> results;
    for (auto& [n, cfg] : entries) {
        results.emplace_back(n, make_connection(n, cfg));
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto& [n, result] : results) {
        if (!server_configs_.count(n)) {
            continue;
        }
        if (!result.error.empty()) {
            server_last_errors_[n] = result.error;
            continue;
        }
        clients_[n] = std::move(result.client);
        for (auto& td : result.tools) {
            all_tools_.push_back(td);
        }
        server_last_errors_.erase(n);
    }
    rebuild_tools_json_cache_();
}

json MCPManager::get_server_config(const std::string& name) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = server_configs_.find(name);
    if (it == server_configs_.end()) {
        throw std::runtime_error("Server '" + name + "' not found");
    }
    return it->second;
}

std::string MCPManager::call_tool(const std::string& tool_name, const json& arguments) {
    std::shared_lock<std::shared_mutex> lock(mutex_);

    ToolDefinition* tool_def = nullptr;
    for (auto& td : all_tools_) {
        if (td.qualified_name == tool_name) {
            tool_def = &td;
            break;
        }
    }

    if (!tool_def) {
        throw std::runtime_error("Tool " + tool_name + " not found");
    }

    auto it = clients_.find(tool_def->server_name);
    if (it == clients_.end()) {
        throw std::runtime_error("Server " + tool_def->server_name + " not connected");
    }

    try {
        auto result = it->second->call_tool(tool_def->original_name, arguments);
        return result.text;
    } catch (const std::exception& e) {
        std::string error_msg = std::string(e.what());
        spdlog::error("Tool {} execution failed: {}", tool_name, error_msg);
        return "Error executing tool: " + error_msg;
    }
}

std::vector<json> MCPManager::get_tools_json() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return all_tools_json_;
}

size_t MCPManager::tools_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return all_tools_.size();
}

void MCPManager::cleanup() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    for (auto& [name, client] : clients_) {
        try {
            client->close();
        } catch (const std::exception& e) {
            spdlog::error("Error cleaning up MCP client '{}': {}", name, e.what());
        }
    }
    clients_.clear();
}

std::string MCPManager::get_ollama_url() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return ollama_url_;
}

std::optional<std::string> MCPManager::get_system_prompt() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return system_prompt_;
}

std::optional<int> MCPManager::get_max_tool_rounds() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return max_tool_rounds_;
}

void MCPManager::set_ollama_url(const std::string& url) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    ollama_url_ = url;
}

void MCPManager::set_system_prompt(const std::optional<std::string>& prompt) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    system_prompt_ = prompt;
}

void MCPManager::set_max_tool_rounds(const std::optional<int>& rounds) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    max_tool_rounds_ = rounds;
}

}
