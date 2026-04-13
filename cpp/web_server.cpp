#include "web_server.h"
#include "server.h"
#include "utils.h"
#include "web_ui.h"

#include <httplib.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>
#include <string>

namespace omb {

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Impl — holds the httplib server polymorphically via base-class pointer
// ---------------------------------------------------------------------------

struct WebServer::Impl {
    std::unique_ptr<httplib::Server> svr;
};

// ---------------------------------------------------------------------------
// Factory: plain or TLS server
// ---------------------------------------------------------------------------

static std::unique_ptr<httplib::Server> make_web_svr(
    bool tls, const std::string& cert_path, const std::string& key_path)
{
    if (!tls) {
        return std::make_unique<httplib::Server>();
    }

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    if (cert_path.empty() || key_path.empty()) {
        throw std::runtime_error("TLS enabled but cert/key paths are empty");
    }
    return std::make_unique<httplib::SSLServer>(cert_path.c_str(), key_path.c_str());
#else
    static_assert(false, "OpenSSL support required for TLS web server — link OpenSSL::SSL");
    (void)cert_path; (void)key_path;
    throw std::runtime_error("OpenSSL not compiled in");
#endif
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

WebServer::WebServer(std::shared_ptr<AppState>      state,
                     std::shared_ptr<LogBuffer>     log_buf,
                     std::shared_ptr<ConfigManager> cfg_mgr,
                     std::shared_ptr<MCPManager>    mcp_mgr,
                     std::shared_ptr<TlsManager>    tls_mgr,
                     std::shared_ptr<Server>        proxy_server)
    : state_(std::move(state))
    , log_buf_(std::move(log_buf))
    , cfg_mgr_(std::move(cfg_mgr))
    , mcp_mgr_(std::move(mcp_mgr))
    , tls_mgr_(std::move(tls_mgr))
    , proxy_server_(std::move(proxy_server))
    , impl_(std::make_unique<Impl>())
{
    bool tls;
    std::string active_cert, cert_dir, explicit_cert, explicit_key;
    {
        std::lock_guard<std::mutex> lk(state_->mutex_);
        tls          = state_->web_tls;
        active_cert  = state_->active_cert_name;
        cert_dir     = state_->cert_dir;
        explicit_cert = state_->web_cert_file;
        explicit_key  = state_->web_key_file;
    }

    std::string cert_file, key_file;
    if (tls) {
        if (!active_cert.empty()) {
            cert_file = cert_dir + "/" + active_cert + ".crt";
            key_file  = cert_dir + "/" + active_cert + ".key";
        } else if (!explicit_cert.empty() && !explicit_key.empty()) {
            cert_file = explicit_cert;
            key_file  = explicit_key;
        }
    }

    impl_->svr = make_web_svr(tls, cert_file, key_file);
    impl_->svr->set_payload_max_length(10 * 1024 * 1024);

    setup_routes();
}

WebServer::~WebServer() {
    stop();
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

void WebServer::start() {
    std::string host;
    int port;
    {
        std::lock_guard<std::mutex> lk(state_->mutex_);
        host = state_->web_host;
        port = state_->web_port;
    }

    if (!impl_->svr->listen(host, port)) {
        spdlog::error("Web management server failed to start on {}:{}", host, port);
    }
}

void WebServer::stop() {
    if (impl_ && impl_->svr) {
        impl_->svr->stop();
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void json_response(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

static void ok(httplib::Response& res, const json& body) {
    json_response(res, 200, body);
}

static void err(httplib::Response& res, int status, const std::string& msg) {
    json_response(res, status, {{"error", msg}});
}

// ---------------------------------------------------------------------------
// Route registration
// ---------------------------------------------------------------------------

void WebServer::setup_routes() {
    auto& svr = *impl_->svr;

    // ------------------------------------------------------------------
    // Static SPA
    // ------------------------------------------------------------------

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(WEB_UI_HTML, "text/html; charset=utf-8");
    });

    svr.Get("/favicon.ico", [](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
    });

    // ------------------------------------------------------------------
    // GET /api/status
    // ------------------------------------------------------------------

    svr.Get("/api/status", [this](const httplib::Request&, httplib::Response& res) {
        try {
            std::string ollama_url, proxy_host, web_host;
            int proxy_port, web_port;
            bool web_tls;
            {
                std::lock_guard<std::mutex> lk(state_->mutex_);
                ollama_url = state_->ollama_url;
                proxy_host = state_->proxy_host;
                proxy_port = state_->proxy_port;
                web_host   = state_->web_host;
                web_port   = state_->web_port;
                web_tls    = state_->web_tls;
            }

            auto statuses = mcp_mgr_->get_all_server_status();
            size_t total_connected = 0;
            size_t total_tools = 0;
            for (auto& s : statuses) {
                if (s.connected) ++total_connected;
                total_tools += s.tool_count;
            }

            auto now = std::chrono::steady_clock::now();
            auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                now - state_->start_time).count();

            json body = {
                {"proxy_running",        state_->proxy_running.load()},
                {"proxy_port",           proxy_port},
                {"proxy_host",           proxy_host},
                {"ollama_url",           ollama_url},
                {"ollama_reachable",     state_->ollama_reachable.load()},
                {"mcp_servers_total",    statuses.size()},
                {"mcp_servers_connected", total_connected},
                {"tools_total",          total_tools},
                {"web_port",             web_port},
                {"web_tls",              web_tls},
                {"uptime_seconds",       uptime},
                {"version",              PROJECT_VERSION}
            };
            ok(res, body);
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // GET /api/config
    // ------------------------------------------------------------------

    svr.Get("/api/config", [this](const httplib::Request&, httplib::Response& res) {
        try {
            std::string ollama_url, proxy_host, web_host, cors_origins, config_file,
                        active_cert, cert_dir;
            int proxy_port, web_port;
            bool web_tls;
            std::optional<int> max_tool_rounds;
            std::optional<std::string> system_prompt;
            {
                std::lock_guard<std::mutex> lk(state_->mutex_);
                ollama_url     = state_->ollama_url;
                proxy_host     = state_->proxy_host;
                proxy_port     = state_->proxy_port;
                web_host       = state_->web_host;
                web_port       = state_->web_port;
                web_tls        = state_->web_tls;
                cors_origins   = state_->cors_origins;
                config_file    = state_->config_file;
                active_cert    = state_->active_cert_name;
                cert_dir       = state_->cert_dir;
                max_tool_rounds = state_->max_tool_rounds;
                system_prompt  = state_->system_prompt;
            }

            json body = {
                {"ollama_url",          ollama_url},
                {"proxy_port",          proxy_port},
                {"proxy_host",          proxy_host},
                {"web_port",            web_port},
                {"web_host",            web_host},
                {"web_tls",             web_tls},
                {"max_tool_rounds",     max_tool_rounds.value_or(0)},
                {"system_prompt",       system_prompt.value_or("")},
                {"cors_origins",        cors_origins},
                {"config_file",         config_file},
                {"web_tls_active_cert", active_cert},
                {"cert_dir",            cert_dir}
            };
            ok(res, body);
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // POST /api/config
    // ------------------------------------------------------------------

    svr.Post("/api/config", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            err(res, 400, "Invalid JSON");
            return;
        }

        bool requires_restart = false;
        static const std::vector<std::string> restart_fields = {
            "proxy_port", "proxy_host", "web_port", "web_host", "web_tls"
        };

        std::optional<std::string> new_ollama_url;
        std::optional<std::optional<int>> new_max_tool_rounds;
        std::optional<std::optional<std::string>> new_system_prompt;

        try {
            std::lock_guard<std::mutex> lk(state_->mutex_);

            if (body.contains("ollama_url") && body["ollama_url"].is_string()) {
                state_->ollama_url = body["ollama_url"].get<std::string>();
                new_ollama_url = state_->ollama_url;
            }
            if (body.contains("max_tool_rounds")) {
                int v = body["max_tool_rounds"].get<int>();
                state_->max_tool_rounds = (v > 0) ? std::optional<int>(v) : std::nullopt;
                new_max_tool_rounds = state_->max_tool_rounds;
            }
            if (body.contains("system_prompt") && body["system_prompt"].is_string()) {
                std::string sp = body["system_prompt"].get<std::string>();
                state_->system_prompt = sp.empty() ? std::nullopt : std::optional<std::string>(sp);
                new_system_prompt = state_->system_prompt;
            }
            if (body.contains("cors_origins") && body["cors_origins"].is_string()) {
                state_->cors_origins = body["cors_origins"].get<std::string>();
            }

            for (auto& rf : restart_fields) {
                if (body.contains(rf)) {
                    requires_restart = true;
                    break;
                }
            }

            if (body.contains("proxy_port") && body["proxy_port"].is_number_integer()) {
                state_->proxy_port = body["proxy_port"].get<int>();
            }
            if (body.contains("proxy_host") && body["proxy_host"].is_string()) {
                state_->proxy_host = body["proxy_host"].get<std::string>();
            }
            if (body.contains("web_port") && body["web_port"].is_number_integer()) {
                state_->web_port = body["web_port"].get<int>();
            }
            if (body.contains("web_host") && body["web_host"].is_string()) {
                state_->web_host = body["web_host"].get<std::string>();
            }
            if (body.contains("web_tls") && body["web_tls"].is_boolean()) {
                state_->web_tls = body["web_tls"].get<bool>();
            }
        } catch (const std::exception& e) {
            err(res, 500, e.what());
            return;
        }

        if (new_ollama_url)      mcp_mgr_->set_ollama_url(*new_ollama_url);
        if (new_max_tool_rounds) mcp_mgr_->set_max_tool_rounds(*new_max_tool_rounds);
        if (new_system_prompt)   mcp_mgr_->set_system_prompt(*new_system_prompt);

        ok(res, {{"ok", true}, {"requires_restart", requires_restart}});
    });

    // ------------------------------------------------------------------
    // GET /api/mcp-servers
    // ------------------------------------------------------------------

    svr.Get("/api/mcp-servers", [this](const httplib::Request&, httplib::Response& res) {
        try {
            auto statuses = mcp_mgr_->get_all_server_status();
            json servers = json::array();
            for (auto& s : statuses) {
                json srv = {
                    {"name",       s.name},
                    {"transport",  s.transport_type},
                    {"connected",  s.connected},
                    {"tool_count", s.tool_count},
                    {"last_error", s.last_error.empty() ? json(nullptr) : json(s.last_error)}
                };
                try {
                    srv["config"] = mcp_mgr_->get_server_config(s.name);
                } catch (...) {
                    srv["config"] = nullptr;
                }
                servers.push_back(std::move(srv));
            }
            ok(res, {{"servers", servers}});
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // POST /api/mcp-servers  — add new server
    // ------------------------------------------------------------------

    svr.Post("/api/mcp-servers", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            err(res, 400, "Invalid JSON");
            return;
        }

        if (!body.contains("name") || !body["name"].is_string()) {
            err(res, 400, "Missing required field: name");
            return;
        }
        if (!body.contains("transport") || !body["transport"].is_string()) {
            err(res, 400, "Missing required field: transport (stdio|http|sse)");
            return;
        }

        std::string name = body["name"].get<std::string>();
        std::string transport = body["transport"].get<std::string>();
        if (transport != "stdio" && transport != "http" && transport != "sse") {
            err(res, 400, "Invalid transport value; must be stdio, http, or sse");
            return;
        }

        try {
            mcp_mgr_->add_server(name, body);

            json cfg = cfg_mgr_->get_mcp_config();
            cfg["mcpServers"][name] = body;
            cfg_mgr_->save_mcp_config(cfg);

            ok(res, {{"ok", true}, {"name", name}});
        } catch (const std::exception& e) {
            std::string what = e.what();
            if (what.find("duplicate") != std::string::npos ||
                what.find("already") != std::string::npos) {
                err(res, 409, what);
            } else if (what.find("Invalid name") != std::string::npos ||
                       what.find("invalid") != std::string::npos) {
                err(res, 400, what);
            } else {
                err(res, 500, what);
            }
        }
    });

    // ------------------------------------------------------------------
    // PUT /api/mcp-servers/:name  — update server
    // ------------------------------------------------------------------

    svr.Put(R"(/api/mcp-servers/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1];

        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            err(res, 400, "Invalid JSON");
            return;
        }

        if (body.contains("transport")) {
            std::string t = body["transport"].get<std::string>();
            if (t != "stdio" && t != "http" && t != "sse") {
                err(res, 400, "Invalid transport value; must be stdio, http, or sse");
                return;
            }
        }

        try {
            mcp_mgr_->update_server(name, body);

            json cfg = cfg_mgr_->get_mcp_config();
            cfg["mcpServers"][name] = body;
            cfg_mgr_->save_mcp_config(cfg);

            ok(res, {{"ok", true}, {"name", name}});
        } catch (const std::exception& e) {
            std::string what = e.what();
            if (what.find("not found") != std::string::npos) {
                err(res, 404, what);
            } else {
                err(res, 500, what);
            }
        }
    });

    // ------------------------------------------------------------------
    // DELETE /api/mcp-servers/:name
    // ------------------------------------------------------------------

    svr.Delete(R"(/api/mcp-servers/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1];
        try {
            mcp_mgr_->remove_server(name);

            json cfg = cfg_mgr_->get_mcp_config();
            if (cfg["mcpServers"].contains(name)) {
                cfg["mcpServers"].erase(name);
            }
            cfg_mgr_->save_mcp_config(cfg);

            ok(res, {{"ok", true}});
        } catch (const std::exception& e) {
            std::string what = e.what();
            if (what.find("not found") != std::string::npos) {
                err(res, 404, what);
            } else {
                err(res, 500, what);
            }
        }
    });

    // ------------------------------------------------------------------
    // POST /api/mcp-servers/:name/reconnect
    // ------------------------------------------------------------------

    svr.Post(R"(/api/mcp-servers/([^/]+)/reconnect)", [this](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1];
        try {
            mcp_mgr_->reconnect_server(name);
            ok(res, {{"ok", true}});
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // GET /api/tools
    // ------------------------------------------------------------------

    svr.Get("/api/tools", [this](const httplib::Request&, httplib::Response& res) {
        try {
            json all_tools = json::array();
            for (auto& t : mcp_mgr_->get_tools_json()) {
                if (t.contains("function")) {
                    auto& fn = t["function"];
                    std::string qname = fn.value("name", "");
                    std::string server_name;
                    auto dot = qname.find('.');
                    if (dot != std::string::npos) {
                        server_name = qname.substr(0, dot);
                    }
                    all_tools.push_back({
                        {"name",        qname},
                        {"server",      server_name},
                        {"description", fn.value("description", "")}
                    });
                }
            }

            ok(res, {{"tools", all_tools}});
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // GET /api/proxy/status
    // ------------------------------------------------------------------

    svr.Get("/api/proxy/status", [this](const httplib::Request&, httplib::Response& res) {
        std::string proxy_host, ollama_url;
        int proxy_port;
        {
            std::lock_guard<std::mutex> lk(state_->mutex_);
            proxy_host = state_->proxy_host;
            proxy_port = state_->proxy_port;
            ollama_url = state_->ollama_url;
        }

        bool running = state_->proxy_running.load();
        auto now = std::chrono::steady_clock::now();
        auto uptime = running
            ? std::chrono::duration_cast<std::chrono::seconds>(now - state_->start_time).count()
            : 0;

        ok(res, {
            {"running",          running},
            {"host",             proxy_host},
            {"port",             proxy_port},
            {"ollama_url",       ollama_url},
            {"ollama_reachable", state_->ollama_reachable.load()},
            {"uptime_seconds",   uptime}
        });
    });

    // ------------------------------------------------------------------
    // POST /api/proxy/stop
    // ------------------------------------------------------------------

    svr.Post("/api/proxy/stop", [this](const httplib::Request&, httplib::Response& res) {
        if (!state_->proxy_running.load()) {
            err(res, 409, "Proxy is not running");
            return;
        }

        try {
            proxy_server_->stop();
            proxy_server_->join_proxy_thread();
            ok(res, {{"ok", true}});
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // POST /api/proxy/start
    // ------------------------------------------------------------------

    svr.Post("/api/proxy/start", [this](const httplib::Request&, httplib::Response& res) {
        bool expected = false;
        if (!state_->proxy_running.compare_exchange_strong(expected, true)) {
            err(res, 409, "Proxy already running");
            return;
        }

        try {
            proxy_server_->reset();
            proxy_server_->start_async();
            ok(res, {{"ok", true}});
        } catch (const std::exception& e) {
            state_->proxy_running.store(false);
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // GET /api/logs?n=200
    // ------------------------------------------------------------------

    svr.Get("/api/logs", [this](const httplib::Request& req, httplib::Response& res) {
        int n = 200;
        if (req.has_param("n")) {
            try {
                n = std::stoi(req.get_param_value("n"));
                if (n < 1) n = 1;
                if (n > 500) n = 500;
            } catch (...) {}
        }

        auto lines = log_buf_->get_lines(n);
        json arr = json::array();
        for (auto& l : lines) arr.push_back(l);

        ok(res, {{"lines", arr}, {"total", arr.size()}});
    });

    // ------------------------------------------------------------------
    // POST /api/ollama/test
    // ------------------------------------------------------------------

    svr.Post("/api/ollama/test", [](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            err(res, 400, "Invalid JSON");
            return;
        }

        if (!body.contains("url") || !body["url"].is_string()) {
            err(res, 400, "Missing required field: url");
            return;
        }

        std::string url = body["url"].get<std::string>();
        std::string test_url = url;
        if (!test_url.empty() && test_url.back() != '/') test_url += '/';
        test_url += "api/tags";

        auto t0 = std::chrono::steady_clock::now();
        std::string resp = http_get(test_url, 5);
        auto t1 = std::chrono::steady_clock::now();
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        bool reachable = !resp.empty();
        ok(res, {{"reachable", reachable}, {"latency_ms", reachable ? ms : 0LL}});
    });

    // ------------------------------------------------------------------
    // GET /api/tls/certificates
    // ------------------------------------------------------------------

    svr.Get("/api/tls/certificates", [this](const httplib::Request&, httplib::Response& res) {
        try {
            std::string active_cert;
            {
                std::lock_guard<std::mutex> lk(state_->mutex_);
                active_cert = state_->active_cert_name;
            }

            auto certs = tls_mgr_->list_certs(active_cert);
            json arr = json::array();
            for (auto& c : certs) {
                auto tp = c.not_after;
                auto tt = std::chrono::system_clock::to_time_t(tp);
                char buf[32];
                struct tm tm_buf{};
                gmtime_r(&tt, &tm_buf);
                strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_buf);

                json sans = json::array();
                for (auto& s : c.sans) sans.push_back(s);

                arr.push_back({
                    {"name",            c.name},
                    {"cert_file",       c.cert_file},
                    {"key_file",        c.key_file},
                    {"common_name",     c.common_name},
                    {"sans",            sans},
                    {"not_after",       std::string(buf)},
                    {"is_active",       c.is_active},
                    {"is_expiring_soon", c.is_expiring_soon}
                });
            }
            ok(res, {{"certificates", arr}});
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // POST /api/tls/generate-self-signed
    // ------------------------------------------------------------------

    svr.Post("/api/tls/generate-self-signed", [this](const httplib::Request& req, httplib::Response& res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (...) {
            err(res, 400, "Invalid JSON");
            return;
        }

        std::string cn   = body.value("cn", "localhost");
        int days         = body.value("days", 365);
        int key_size     = body.value("key_size", 2048);

        std::vector<std::string> sans;
        if (body.contains("sans") && body["sans"].is_array()) {
            for (auto& s : body["sans"]) {
                if (s.is_string()) sans.push_back(s.get<std::string>());
            }
        }

        try {
            std::string job_id = tls_mgr_->start_generate_job(cn, days, key_size, sans);
            json_response(res, 202, {{"job_id", job_id}});
        } catch (const std::exception& e) {
            std::string what = e.what();
            if (what.find("429") != std::string::npos ||
                what.find("Too many") != std::string::npos) {
                err(res, 429, what);
            } else {
                err(res, 500, what);
            }
        }
    });

    // ------------------------------------------------------------------
    // GET /api/tls/jobs/:job_id
    // ------------------------------------------------------------------

    svr.Get(R"(/api/tls/jobs/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string job_id = req.matches[1];
        try {
            auto job_opt = tls_mgr_->get_job(job_id);
            if (!job_opt) {
                err(res, 404, "Job not found or expired");
                return;
            }
            auto& job = *job_opt;
            json body;
            switch (job.status) {
                case GenJob::Status::running:
                    body = {{"status", "running"}};
                    break;
                case GenJob::Status::done:
                    body = {{"status", "done"}, {"cert_name", job.cert_name}, {"error", nullptr}};
                    break;
                case GenJob::Status::error:
                    body = {{"status", "error"}, {"error", job.error_msg}};
                    break;
            }
            ok(res, body);
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });

    // ------------------------------------------------------------------
    // POST /api/tls/upload
    // ------------------------------------------------------------------

    svr.Post("/api/tls/upload", [this](const httplib::Request& req, httplib::Response& res) {
        std::string name, cert_pem, key_pem;

        if (req.is_multipart_form_data()) {
            auto name_it = req.files.find("name");
            auto cert_it = req.files.find("cert");
            auto key_it  = req.files.find("key");
            if (name_it == req.files.end() || cert_it == req.files.end() || key_it == req.files.end()) {
                err(res, 400, "Multipart form must contain fields: name, cert, key");
                return;
            }
            name     = name_it->second.content;
            cert_pem = cert_it->second.content;
            key_pem  = key_it->second.content;
        } else {
            json body;
            try {
                body = json::parse(req.body);
            } catch (...) {
                err(res, 400, "Expected multipart/form-data or JSON body with fields: name, cert, key");
                return;
            }
            if (!body.contains("name") || !body["name"].is_string() ||
                !body.contains("cert") || !body["cert"].is_string() ||
                !body.contains("key")  || !body["key"].is_string()) {
                err(res, 400, "JSON body must contain string fields: name, cert, key");
                return;
            }
            name     = body["name"].get<std::string>();
            cert_pem = body["cert"].get<std::string>();
            key_pem  = body["key"].get<std::string>();
        }

        if (name.empty() || cert_pem.empty() || key_pem.empty()) {
            err(res, 400, "Fields name, cert, key must not be empty");
            return;
        }

        try {
            tls_mgr_->upload_cert(name, cert_pem, key_pem);
            ok(res, {{"ok", true}, {"name", name}});
        } catch (const std::exception& e) {
            err(res, 400, e.what());
        }
    });

    // ------------------------------------------------------------------
    // DELETE /api/tls/certificates/:name
    // ------------------------------------------------------------------

    svr.Delete(R"(/api/tls/certificates/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1];
        try {
            bool tls_enabled;
            std::string active_cert;
            {
                std::lock_guard<std::mutex> lk(state_->mutex_);
                tls_enabled = state_->web_tls;
                active_cert = state_->active_cert_name;
            }
            tls_mgr_->delete_cert(name, tls_enabled, active_cert);
            ok(res, {{"ok", true}});
        } catch (const std::exception& e) {
            std::string what = e.what();
            if (what.find("active") != std::string::npos) {
                err(res, 409, what);
            } else {
                err(res, 500, what);
            }
        }
    });

    // ------------------------------------------------------------------
    // POST /api/tls/activate/:name
    // ------------------------------------------------------------------

    svr.Post(R"(/api/tls/activate/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.matches[1];
        try {
            tls_mgr_->activate_cert(name);
            {
                std::lock_guard<std::mutex> lk(state_->mutex_);
                state_->active_cert_name = name;
            }
            ok(res, {{"ok", true}, {"active_cert", name}});
        } catch (const std::exception& e) {
            err(res, 500, e.what());
        }
    });
}

}
