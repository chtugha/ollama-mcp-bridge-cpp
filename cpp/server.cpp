#include "server.h"
#include "utils.h"

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <sstream>

namespace omb {

struct Server::Impl {
    httplib::Server svr;
};

static const std::string& cached_cors_origins() {
    static const std::string origins = get_env("CORS_ORIGINS", "*");
    return origins;
}

Server::Server(const ServerConfig& config)
    : config_(config), impl_(std::make_unique<Impl>()) {}

Server::~Server() {
    stop();
}

void Server::start() {
    mcp_manager_ = std::make_unique<MCPManager>(config_.ollama_url, config_.system_prompt);
    mcp_manager_->max_tool_rounds = config_.max_tool_rounds;
    mcp_manager_->load_servers(config_.config_file);

    proxy_service_ = std::make_unique<ProxyService>(*mcp_manager_);

    spdlog::info("Startup complete. Total tools available: {}", mcp_manager_->tools_count());

    setup_routes();

    spdlog::info("API endpoints:");
    spdlog::info("  POST /api/chat - Ollama-compatible chat with MCP tools");
    spdlog::info("  GET /health - Health check and status");
    spdlog::info("  GET /version - Version information");

    const std::string& cors_origins = cached_cors_origins();
    if (cors_origins == "*") {
        spdlog::warn("CORS is configured to allow ALL origins (*). This is not recommended for production.");
    } else {
        spdlog::info("CORS configured to allow origins: {}", cors_origins);
    }

    spdlog::info("Starting MCP proxy server on {}:{}", config_.host, config_.port);

    if (!impl_->svr.listen(config_.host, config_.port)) {
        spdlog::error("Failed to start server on {}:{}", config_.host, config_.port);
    }
}

void Server::stop() {
    if (impl_) {
        impl_->svr.stop();
    }
    if (proxy_service_) {
        proxy_service_.reset();
    }
    if (mcp_manager_) {
        mcp_manager_->cleanup();
        mcp_manager_.reset();
    }
}

static void add_cors_headers(const httplib::Request& req, httplib::Response& res) {
    const std::string& cors_origins = cached_cors_origins();
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

void Server::setup_routes() {
    impl_->svr.Options(R"(.*)", [](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res);
        res.status = 204;
    });

    impl_->svr.Get("/health", [this](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res);
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

    impl_->svr.Get("/version", [](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res);
        json version_info = {
            {"version", PROJECT_VERSION}
        };
        res.set_content(version_info.dump(), "application/json");
    });

    impl_->svr.Post("/api/chat", [this](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res);
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

    auto proxy_handler = [this](const httplib::Request& req, httplib::Response& res) {
        add_cors_headers(req, res);
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
