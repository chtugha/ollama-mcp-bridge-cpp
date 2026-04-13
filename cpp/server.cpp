#include "server.h"
#include "utils.h"

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <stdexcept>

namespace omb {

struct Server::Impl {
    httplib::Server svr;
};

Server::Server(std::shared_ptr<AppState> state,
               std::shared_ptr<LogBuffer> log_buffer,
               std::shared_ptr<MCPManager> mcp_manager)
    : state_(std::move(state))
    , log_buffer_(std::move(log_buffer))
    , mcp_manager_(std::move(mcp_manager))
    , impl_(std::make_unique<Impl>()) {}

Server::~Server() {
    stop();
    join_proxy_thread();
    if (health_poll_thread_.joinable()) {
        health_poll_thread_.join();
    }
}

static void add_cors_headers(const httplib::Request& req, httplib::Response& res,
                              const std::string& cors_origins) {
    if (cors_origins == "*") {
        res.set_header("Access-Control-Allow-Origin", "*");
    } else {
        std::string request_origin = req.get_header_value("Origin");
        if (!request_origin.empty()) {
            std::istringstream ss(cors_origins);
            std::string origin;
            while (std::getline(ss, origin, ',')) {
                while (!origin.empty() && origin.front() == ' ') origin.erase(origin.begin());
                while (!origin.empty() && origin.back() == ' ') origin.pop_back();
                if (origin == request_origin) {
                    res.set_header("Access-Control-Allow-Origin", request_origin);
                    res.set_header("Access-Control-Allow-Credentials", "true");
                    break;
                }
            }
        }
    }
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS, HEAD");
    std::string requested_headers = req.get_header_value("Access-Control-Request-Headers");
    res.set_header("Access-Control-Allow-Headers",
        requested_headers.empty() ? "Content-Type, Authorization, X-Requested-With" : requested_headers);
}

void Server::init_proxy() {
    std::string ollama_url;
    {
        std::lock_guard<std::mutex> lk(state_->mutex_);
        ollama_url = state_->ollama_url;
    }

    bool reachable = check_ollama_health(ollama_url);
    state_->ollama_reachable.store(reachable);

    if (!reachable) {
        if (state_->fail_on_ollama_unavailable.load()) {
            spdlog::error("Ollama is not reachable at {}. Exiting.", ollama_url);
            throw std::runtime_error("Ollama unavailable and --fail-on-ollama-unavailable is set");
        }
        spdlog::warn("Ollama is not reachable at {}. Chat requests will return 503 until Ollama is available.", ollama_url);
    }

    if (!proxy_service_) {
        proxy_service_ = std::make_unique<ProxyService>(*mcp_manager_);
        setup_routes();
    }

    start_health_poll();
}

void Server::start() {
    init_proxy();

    std::string host;
    int port;
    std::string cors_origins;
    {
        std::lock_guard<std::mutex> lk(state_->mutex_);
        host = state_->proxy_host;
        port = state_->proxy_port;
        cors_origins = state_->cors_origins;
    }

    spdlog::info("Startup complete. Total tools available: {}", mcp_manager_->tools_count());
    spdlog::info("API endpoints:");
    spdlog::info("  POST /api/chat - Ollama-compatible chat with MCP tools");
    spdlog::info("  GET /health - Health check and status");
    spdlog::info("  GET /version - Version information");

    if (cors_origins == "*") {
        spdlog::warn("CORS is configured to allow ALL origins (*). This is not recommended for production.");
    } else {
        spdlog::info("CORS configured to allow origins: {}", cors_origins);
    }

    spdlog::info("Starting MCP proxy server on {}:{}", host, port);
    state_->proxy_running.store(true);

    if (!impl_->svr.listen(host, port)) {
        spdlog::error("Failed to start server on {}:{}", host, port);
    }

    state_->proxy_running.store(false);
}

void Server::start_async() {
    init_proxy();

    std::string host;
    int port;
    {
        std::lock_guard<std::mutex> lk(state_->mutex_);
        host = state_->proxy_host;
        port = state_->proxy_port;
    }

    spdlog::info("Startup complete. Total tools available: {}", mcp_manager_->tools_count());
    spdlog::info("Starting MCP proxy server (async) on {}:{}", host, port);

    state_->proxy_running.store(true);
    std::lock_guard<std::mutex> thread_lk(state_->proxy_thread_mutex_);
    state_->proxy_thread = std::thread([this, host, port]() {
        if (!impl_->svr.listen(host, port)) {
            spdlog::error("Failed to start server on {}:{}", host, port);
        }
        state_->proxy_running.store(false);
    });
}

void Server::stop() {
    stop_health_poll();
    if (impl_) {
        impl_->svr.stop();
    }
    state_->proxy_running.store(false);
}

void Server::join_proxy_thread() {
    std::lock_guard<std::mutex> lk(state_->proxy_thread_mutex_);
    if (state_->proxy_thread && state_->proxy_thread->joinable()) {
        state_->proxy_thread->join();
        state_->proxy_thread.reset();
    }
}

void Server::reset() {
    impl_ = std::make_unique<Impl>();
    proxy_service_.reset();
}

void Server::start_health_poll() {
    if (health_poll_thread_.joinable()) {
        health_poll_thread_.join();
    }
    health_poll_stop_.store(false);
    health_poll_thread_ = std::thread([this]() {
        while (!health_poll_stop_.load()) {
            std::unique_lock<std::mutex> lk(health_cv_mutex_);
            health_cv_.wait_for(lk, std::chrono::seconds(30),
                [this]() { return health_poll_stop_.load(); });
            if (health_poll_stop_.load()) break;

            std::string ollama_url;
            {
                std::lock_guard<std::mutex> slk(state_->mutex_);
                ollama_url = state_->ollama_url;
            }

            bool reachable = check_ollama_health(ollama_url);
            bool was_reachable = state_->ollama_reachable.exchange(reachable);
            if (reachable && !was_reachable) {
                spdlog::info("Ollama is now reachable at {}", ollama_url);
            } else if (!reachable && was_reachable) {
                spdlog::warn("Ollama is no longer reachable at {}", ollama_url);
            }
        }
    });
}

void Server::stop_health_poll() {
    health_poll_stop_.store(true);
    health_cv_.notify_all();
}

void Server::setup_routes() {
    auto get_cors = [this]() {
        std::lock_guard<std::mutex> lk(state_->mutex_);
        return state_->cors_origins;
    };

    impl_->svr.Options(R"(.*)", [get_cors](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res, get_cors());
        res.status = 204;
    });

    impl_->svr.Get("/health", [this, get_cors](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res, get_cors());
        if (!proxy_service_) {
            res.status = 503;
            res.set_content(R"({"detail":"Services not initialized"})", "application/json");
            return;
        }

        json health_info = proxy_service_->health_check();
        int status = health_info["status"] == "healthy" ? 200 : 503;
        res.status = status;
        res.set_content(health_info.dump(), "application/json");
    });

    impl_->svr.Get("/version", [get_cors](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res, get_cors());
        json version_info = {
            {"version", PROJECT_VERSION}
        };
        res.set_content(version_info.dump(), "application/json");
    });

    impl_->svr.Post("/api/chat", [this, get_cors](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res, get_cors());

        if (!state_->ollama_reachable.load()) {
            res.status = 503;
            res.set_content(
                json({{"error", "Ollama is not reachable. Please check your Ollama server and connection."},
                      {"detail", "ollama_unreachable"}}).dump(),
                "application/json");
            return;
        }

        if (!proxy_service_) {
            res.status = 503;
            res.set_content(R"({"detail":"Services not initialized"})", "application/json");
            return;
        }

        json body;
        try {
            body = json::parse(req.body);
        } catch (const json::parse_error& e) {
            res.status = 400;
            res.set_content(json({{"detail", std::string("Invalid JSON: ") + e.what()}}).dump(), "application/json");
            return;
        }

        bool stream = body.value("stream", false);

        try {
            if (stream) {
                res.set_header("Content-Type", "application/json");
                res.set_header("Transfer-Encoding", "chunked");

                res.set_chunked_content_provider(
                    "application/json",
                    [this, body](size_t, httplib::DataSink& sink) -> bool {
                        proxy_service_->proxy_chat_with_tools_streaming(body,
                            [&sink](const std::string& chunk) {
                                sink.write(chunk.data(), chunk.size());
                            });
                        sink.done();
                        return true;
                    });
            } else {
                auto result = proxy_service_->proxy_chat_with_tools(body);
                res.status = result.status;
                res.set_content(result.body.dump(), "application/json");
            }
        } catch (const std::exception& e) {
            spdlog::error("/api/chat failed: {}", e.what());
            res.status = 500;
            res.set_content(json({{"detail", std::string("/api/chat failed: ") + e.what()}}).dump(), "application/json");
        }
    });

    auto proxy_handler = [this, get_cors](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res, get_cors());
        if (!proxy_service_) {
            res.status = 503;
            res.set_content(R"({"detail":"Services not initialized"})", "application/json");
            return;
        }

        std::string path = req.path;
        if (!path.empty() && path[0] == '/') path = path.substr(1);

        std::map<std::string, std::string> headers;
        for (auto& [k, v] : req.headers) {
            headers[k] = v;
        }

        std::string query;
        if (!req.params.empty()) {
            for (auto& [k, v] : req.params) {
                if (!query.empty()) query += "&";
                query += httplib::detail::encode_query_param(k) + "=" + httplib::detail::encode_query_param(v);
            }
        }

        try {
            auto result = proxy_service_->proxy_generic_request(
                req.method, path, headers, req.body, query);

            res.status = result.status;
            for (auto& [k, v] : result.headers) {
                std::string lower_k = k;
                std::transform(lower_k.begin(), lower_k.end(), lower_k.begin(), ::tolower);
                if (lower_k != "transfer-encoding" && lower_k != "content-length") {
                    res.set_header(k, v);
                }
            }
            std::string content_type = "application/json";
            for (auto& [hk, hv] : result.headers) {
                std::string lk = hk;
                std::transform(lk.begin(), lk.end(), lk.begin(), ::tolower);
                if (lk == "content-type") { content_type = hv; break; }
            }
            res.set_content(result.body, content_type);
        } catch (const std::exception& e) {
            spdlog::error("Proxy failed for {}: {}", path, e.what());
            res.status = 500;
            res.set_content(json({{"detail", std::string("Proxy failed: ") + e.what()}}).dump(), "application/json");
        }
    };

    impl_->svr.Get(R"(/(?!health|version|api/chat)(.*))", proxy_handler);
    impl_->svr.Post(R"(/(?!health|version|api/chat)(.*))", proxy_handler);
    impl_->svr.Put(R"(/(?!health|version|api/chat)(.*))", proxy_handler);
    impl_->svr.Delete(R"(/(?!health|version|api/chat)(.*))", proxy_handler);
    impl_->svr.Patch(R"(/(?!health|version|api/chat)(.*))", proxy_handler);
}

}
