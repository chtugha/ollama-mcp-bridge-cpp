#include "server.h"
#include "utils.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <iostream>
#include <csignal>
#include <atomic>

static std::atomic<bool> g_running{true};
static std::atomic<omb::Server*> g_server{nullptr};

static void signal_handler(int) {
    g_running = false;
    auto* srv = g_server.load(std::memory_order_acquire);
    if (srv) srv->stop();
}

int main(int argc, char* argv[]) {
    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_default_logger(console);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    CLI::App app{"Ollama MCP Bridge - API proxy server with Ollama REST API compatibility and MCP tool integration"};

    std::string config = "mcp-config.json";
    std::string host = "0.0.0.0";
    int port = 8000;
    std::string ollama_url = omb::get_env("OLLAMA_URL", "http://localhost:11434");
    int max_tool_rounds = 0;
    std::string system_prompt;
    bool show_version = false;

    app.add_option("--config", config, "Path to MCP config JSON file")->default_val("mcp-config.json");
    app.add_option("--host", host, "Host to bind to")->default_val("0.0.0.0");
    app.add_option("--port", port, "Port to bind to")->default_val(8000);
    app.add_option("--ollama-url", ollama_url, "Ollama server URL");
    app.add_option("--max-tool-rounds", max_tool_rounds, "Maximum tool execution rounds (0 = unlimited)")->default_val(0);
    app.add_option("--system-prompt", system_prompt, "System prompt to prepend to messages");
    app.add_flag("--version", show_version, "Show version information and exit");

    CLI11_PARSE(app, argc, argv);

    if (show_version) {
        std::cout << "ollama-mcp-bridge v" << PROJECT_VERSION << std::endl;
        return 0;
    }

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

    spdlog::info("Starting MCP proxy server on {}:{}", host, port);
    spdlog::info("Using Ollama server: {}", ollama_url);
    spdlog::info("Using config file: {}", config);

    if (!omb::check_ollama_health(ollama_url)) {
        spdlog::info("Please ensure Ollama is running with: ollama serve");
        return 1;
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    omb::ServerConfig server_config;
    server_config.config_file = config;
    server_config.host = host;
    server_config.port = port;
    server_config.ollama_url = ollama_url;
    if (max_tool_rounds > 0) server_config.max_tool_rounds = max_tool_rounds;
    if (!system_prompt.empty()) server_config.system_prompt = system_prompt;

    try {
        omb::Server server(server_config);
        g_server.store(&server, std::memory_order_release);
        server.start();
        g_server.store(nullptr, std::memory_order_release);
    } catch (const std::exception& e) {
        spdlog::error("Server error: {}", e.what());
        return 1;
    }

    spdlog::info("Shutdown complete");
    return 0;
}
