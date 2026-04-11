#include "mcp_manager.h"
#include "utils.h"

#include <fstream>
#include <filesystem>
#include <algorithm>
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
    : ollama_url(ollama_url), system_prompt(system_prompt) {}

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

    for (auto& [name, server_config] : config["mcpServers"].items()) {
        json resolved = server_config;
        resolved["cwd"] = config_dir;
        connect_server(name, resolved);
    }
}

void MCPManager::connect_server(const std::string& name, const json& config) {
    try {
        json tool_filter = config.value("toolFilter", json::object());
        if (!tool_filter.empty()) {
            std::string mode = tool_filter.value("mode", "include");
            if (mode != "include" && mode != "exclude") {
                spdlog::error("Invalid toolFilter mode '{}' for server '{}'. Must be 'include' or 'exclude'.", mode, name);
                return;
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
            spdlog::error("Invalid MCP server config for '{}': must have 'command' or 'url'", name);
            return;
        }

        auto client = std::make_unique<McpClient>(std::move(transport));
        if (!client->initialize()) {
            spdlog::error("Failed to initialize MCP server '{}'", name);
            return;
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
            all_tools_.push_back(td);
            all_tools_json_.push_back(td.to_json());
        }

        clients_[name] = std::move(client);

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
        spdlog::error("Failed to connect to MCP server '{}': {}", name, e.what());
    }
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
    for (auto& [name, client] : clients_) {
        try {
            client->close();
        } catch (const std::exception& e) {
            spdlog::error("Error cleaning up MCP client '{}': {}", name, e.what());
        }
    }
    clients_.clear();
}

}
