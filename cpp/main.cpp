#include "server.h"
#include "web_server.h"
#include "log_buffer.h"
#include "config_manager.h"
#include "tls_manager.h"
#include "utils.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/dist_sink.h>

#include <filesystem>
#include <iostream>
#include <csignal>
#include <atomic>
#include <thread>

#ifndef _WIN32
#  include <unistd.h>
#endif

static int g_shutdown_pipe[2] = {-1, -1};
static std::atomic<bool>* g_shutdown_flag = nullptr;

static void signal_handler(int) {
#ifndef _WIN32
    char byte = 1;
    (void)write(g_shutdown_pipe[1], &byte, 1);
#else
    if (g_shutdown_flag) g_shutdown_flag->store(true);
#endif
}

int main(int argc, char* argv[]) {
    CLI::App app{"Ollama MCP Bridge - API proxy with MCP tool integration and web management UI"};

    // Proxy options
    std::string config = "mcp-config.json";
    std::string host = "0.0.0.0";
    int port = 8000;
    std::string ollama_url = "http://localhost:11434";
    int max_tool_rounds = 0;
    std::string system_prompt;
    bool show_version = false;
    // Bool env vars require parse_bool_env; read before CLI11 so CLI flags can override
    bool fail_on_ollama_unavailable =
        omb::parse_bool_env(omb::get_env("FAIL_ON_OLLAMA_UNAVAILABLE", "false"));

    // Web UI options — bool env vars handled manually; strings/ints via CLI11 envname()
    std::string web_host = "0.0.0.0";
    int web_port = 11464;
    bool no_web_ui = !omb::parse_bool_env(omb::get_env("WEB_UI", "true"));
    bool web_tls = omb::parse_bool_env(omb::get_env("WEB_TLS", "false"));
    std::string web_cert;
    std::string web_key;
    std::string web_cert_dir;

    app.add_option("--config", config, "Path to MCP config JSON file")
        ->default_val("mcp-config.json")->envname("MCP_CONFIG");
    app.add_option("--host", host, "Proxy bind host")
        ->default_val("0.0.0.0")->envname("PROXY_HOST");
    app.add_option("--port", port, "Proxy bind port")
        ->default_val(8000)->envname("PROXY_PORT");
    app.add_option("--ollama-url", ollama_url, "Ollama server URL")
        ->default_val("http://localhost:11434")->envname("OLLAMA_URL");
    app.add_option("--max-tool-rounds", max_tool_rounds,
                   "Maximum tool execution rounds (0 = unlimited)")
        ->default_val(0)->envname("MAX_TOOL_ROUNDS");
    app.add_option("--system-prompt", system_prompt, "System prompt to prepend to messages")
        ->envname("SYSTEM_PROMPT");
    app.add_flag("--version", show_version, "Show version information and exit");
    app.add_flag("--fail-on-ollama-unavailable,--no-fail-on-ollama-unavailable{false}",
                 fail_on_ollama_unavailable,
                 "Exit with code 1 if Ollama is not reachable at startup "
                 "(env: FAIL_ON_OLLAMA_UNAVAILABLE; --no-... overrides env)");
    app.add_option("--web-host", web_host, "Web UI bind host")
        ->default_val("0.0.0.0")->envname("WEB_HOST");
    app.add_option("--web-port", web_port, "Web UI bind port")
        ->default_val(11464)->envname("WEB_PORT");
    app.add_flag("--no-web-ui", no_web_ui,
                 "Disable the web management UI (env: WEB_UI=false)");
    app.add_flag("--web-tls", web_tls,
                 "Enable HTTPS for the web UI (env: WEB_TLS=true)");
    app.add_option("--web-cert", web_cert, "Path to TLS certificate file for web UI")
        ->envname("WEB_CERT");
    app.add_option("--web-key", web_key, "Path to TLS private key file for web UI")
        ->envname("WEB_KEY");
    app.add_option("--web-cert-dir", web_cert_dir, "Directory for TLS certificate storage")
        ->envname("WEB_CERT_DIR");

    CLI11_PARSE(app, argc, argv);

    if (show_version) {
        std::cout << "ollama-mcp-bridge v" << PROJECT_VERSION << std::endl;
        return 0;
    }

    // Setup logging
    auto log_buffer = std::make_shared<omb::LogBuffer>(500);
    auto dist_sink = std::make_shared<spdlog::sinks::dist_sink_mt>();
    dist_sink->add_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    dist_sink->add_sink(log_buffer);
    auto logger = std::make_shared<spdlog::logger>("console", dist_sink);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);

    // Validate proxy inputs
    omb::CliInputs inputs;
    inputs.config = config;
    inputs.host = host;
    inputs.port = port;
    inputs.ollama_url = ollama_url;
    if (max_tool_rounds > 0) inputs.max_tool_rounds = max_tool_rounds;
    if (!system_prompt.empty()) inputs.system_prompt = system_prompt;

    auto validation_error = omb::validate_cli_inputs(inputs);
    if (validation_error) {
        spdlog::error("{}", *validation_error);
        return 1;
    }

    auto [port_err, err_msg] = omb::is_port_in_use(host, port);
    if (port_err) {
        spdlog::error("{}", *err_msg);
        return 1;
    }

    if (!no_web_ui) {
        auto [wport_err, werr_msg] = omb::is_port_in_use(web_host, web_port);
        if (wport_err) {
            spdlog::error("Web UI port {}: {}", web_port, *werr_msg);
            return 1;
        }
    }

    // Derive cert dir (default: <config-file directory>/certs)
    if (web_cert_dir.empty()) {
        namespace fs = std::filesystem;
        auto cfg_path = fs::path(config);
        fs::path base = (cfg_path.has_parent_path() && cfg_path.parent_path() != fs::path("."))
            ? cfg_path.parent_path()
            : fs::current_path();
        web_cert_dir = (base / "certs").string();
    }

    // Construct ConfigManager early so we can check for a persisted active cert
    auto cfg_mgr = std::make_shared<omb::ConfigManager>(config);

    // Validate TLS: CLI cert/key OR a persisted active cert must exist
    if (web_tls) {
        bool have_explicit = !web_cert.empty() && !web_key.empty();
        bool have_persisted = !cfg_mgr->get_active_cert().empty();
        if (!have_explicit && !have_persisted) {
            spdlog::error("--web-tls requires either --web-cert/--web-key or an activated "
                          "certificate (use POST /api/tls/activate/:name)");
            return 1;
        }
    }

    // Populate AppState
    auto state = std::make_shared<omb::AppState>();
    {
        std::lock_guard<std::mutex> lk(state->mutex_);
        state->ollama_url    = ollama_url;
        state->proxy_host    = host;
        state->proxy_port    = port;
        state->config_file   = config;
        state->cors_origins  = omb::get_env("CORS_ORIGINS", "*");
        state->web_host      = web_host;
        state->web_port      = web_port;
        state->web_tls       = web_tls;
        state->web_cert_file = web_cert;
        state->web_key_file  = web_key;
        state->cert_dir      = web_cert_dir;
        state->active_cert_name = cfg_mgr->get_active_cert();
        if (max_tool_rounds > 0) state->max_tool_rounds = max_tool_rounds;
        if (!system_prompt.empty()) state->system_prompt = system_prompt;
    }
    state->fail_on_ollama_unavailable.store(fail_on_ollama_unavailable);

    // Construct remaining shared components
    std::optional<std::string> sys_prompt_opt;
    if (!system_prompt.empty()) sys_prompt_opt = system_prompt;

    auto mcp_manager = std::make_shared<omb::MCPManager>(ollama_url, sys_prompt_opt);
    if (max_tool_rounds > 0) mcp_manager->set_max_tool_rounds(max_tool_rounds);
    mcp_manager->load_servers(config);

    auto tls_mgr = std::make_shared<omb::TlsManager>(web_cert_dir, cfg_mgr);

    spdlog::info("Starting MCP proxy on {}:{}", host, port);
    spdlog::info("Using Ollama: {}", ollama_url);
    spdlog::info("Using config: {}", config);

    // Setup self-pipe shutdown (POSIX) / flag (Windows)
#ifndef _WIN32
    if (pipe(g_shutdown_pipe) != 0) {
        spdlog::error("Failed to create shutdown pipe");
        return 1;
    }
#else
    g_shutdown_flag = &state->shutdown_requested;
#endif
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Start proxy
    auto proxy_server = std::make_shared<omb::Server>(state, log_buffer, mcp_manager);
    proxy_server->start_async();

    // Start web UI
    std::unique_ptr<omb::WebServer> web_server;
    std::thread web_thread;
    if (!no_web_ui) {
        try {
            web_server = std::make_unique<omb::WebServer>(
                state, log_buffer, cfg_mgr, mcp_manager, tls_mgr, proxy_server);

            std::string scheme = web_tls ? "https" : "http";
            spdlog::info("Web UI available at: {}://{}:{}", scheme, web_host, web_port);
            if (web_host == "0.0.0.0") {
                spdlog::warn("Web UI is bound to 0.0.0.0 — accessible from all interfaces. "
                             "Use --web-host 127.0.0.1 to restrict to localhost only.");
            }

            web_thread = std::thread([&]() {
                try {
                    web_server->start();
                } catch (const std::exception& ex) {
                    spdlog::error("Web UI error: {}", ex.what());
                }
            });
        } catch (const std::exception& ex) {
            spdlog::error("Failed to start Web UI: {}", ex.what());
        }
    }

    // Wait for shutdown signal
#ifndef _WIN32
    {
        char byte = 0;
        (void)read(g_shutdown_pipe[0], &byte, 1);
    }
    close(g_shutdown_pipe[0]);
    close(g_shutdown_pipe[1]);
#else
    while (!state->shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
#endif

    spdlog::info("Shutdown signal received — stopping services...");

    if (web_server) {
        web_server->stop();
    }
    if (web_thread.joinable()) {
        web_thread.join();
    }

    proxy_server->stop();
    proxy_server->join_proxy_thread();

    spdlog::info("Shutdown complete");
    return 0;
}
