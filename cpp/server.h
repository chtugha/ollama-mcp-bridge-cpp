#pragma once

#include <string>
#include <optional>
#include <memory>

#include "mcp_manager.h"
#include "proxy_service.h"

namespace omb {

struct ServerConfig {
    std::string config_file = "mcp-config.json";
    std::string host = "0.0.0.0";
    int port = 8000;
    std::string ollama_url = "http://localhost:11434";
    std::optional<int> max_tool_rounds;
    std::optional<std::string> system_prompt;
};

class Server {
public:
    Server(const ServerConfig& config);
    ~Server();

    void start();
    void stop();

private:
    ServerConfig config_;
    std::unique_ptr<MCPManager> mcp_manager_;
    std::unique_ptr<ProxyService> proxy_service_;

    void setup_routes();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
