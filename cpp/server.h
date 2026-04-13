#pragma once

#include <string>
#include <optional>
#include <memory>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "app_state.h"
#include "log_buffer.h"
#include "mcp_manager.h"
#include "proxy_service.h"

namespace omb {

class Server {
public:
    Server(std::shared_ptr<AppState> state,
           std::shared_ptr<LogBuffer> log_buffer,
           std::shared_ptr<MCPManager> mcp_manager);
    ~Server();

    void start();
    void start_async();
    void stop();
    void join_proxy_thread();
    void reset();

private:
    std::shared_ptr<AppState> state_;
    std::shared_ptr<LogBuffer> log_buffer_;
    std::shared_ptr<MCPManager> mcp_manager_;
    std::unique_ptr<ProxyService> proxy_service_;

    std::atomic<bool> health_poll_stop_{false};
    std::thread health_poll_thread_;
    std::condition_variable health_cv_;
    std::mutex health_cv_mutex_;

    void init_proxy();
    void setup_routes();
    void start_health_poll();
    void stop_health_poll();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
