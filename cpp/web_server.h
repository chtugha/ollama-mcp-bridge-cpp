#pragma once

#include <memory>
#include <string>

#include "app_state.h"
#include "log_buffer.h"
#include "config_manager.h"
#include "mcp_manager.h"
#include "tls_manager.h"

namespace omb {

class Server;

class WebServer {
public:
    WebServer(std::shared_ptr<AppState> state,
              std::shared_ptr<LogBuffer> log_buf,
              std::shared_ptr<ConfigManager> cfg_mgr,
              std::shared_ptr<MCPManager> mcp_mgr,
              std::shared_ptr<TlsManager> tls_mgr,
              std::shared_ptr<Server> proxy_server);
    ~WebServer();

    void start();
    void stop();

private:
    void setup_routes();

    std::shared_ptr<AppState>      state_;
    std::shared_ptr<LogBuffer>     log_buf_;
    std::shared_ptr<ConfigManager> cfg_mgr_;
    std::shared_ptr<MCPManager>    mcp_mgr_;
    std::shared_ptr<TlsManager>    tls_mgr_;
    std::shared_ptr<Server>        proxy_server_;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
