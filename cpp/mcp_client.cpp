#include "mcp_client.h"
#include "utils.h"

#include <spdlog/spdlog.h>
#include <httplib.h>

#include <sstream>
#include <algorithm>
#include <cstring>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#endif

namespace omb {

// --- StdioTransport ---

StdioTransport::StdioTransport(const std::string& command,
                               const std::vector<std::string>& args,
                               const std::map<std::string, std::string>& env,
                               const std::string& cwd)
    : command_(command), args_(args), env_(env), cwd_(cwd) {}

StdioTransport::~StdioTransport() {
    disconnect();
}

bool StdioTransport::connect() {
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0) {
        spdlog::error("Failed to create stdin pipe");
        return false;
    }
    if (pipe(stdout_pipe) != 0) {
        spdlog::error("Failed to create stdout pipe");
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        spdlog::error("Failed to fork process");
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }

        if (!cwd_.empty()) {
            if (chdir(cwd_.c_str()) != 0) {
                _exit(1);
            }
        }

        for (auto& [k, v] : env_) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        std::vector<const char*> argv;
        argv.push_back(command_.c_str());
        for (auto& arg : args_) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(command_.c_str(), const_cast<char* const*>(argv.data()));
        _exit(1);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);

    child_pid_ = pid;
    stdin_fd_ = stdin_pipe[1];
    stdout_fd_ = stdout_pipe[0];

    connected_ = true;
    return true;
}

void StdioTransport::disconnect() {
    if (!connected_.exchange(false)) return;

    {
        std::lock_guard<std::mutex> lock(io_mutex_);
        if (stdin_fd_ >= 0) { close(stdin_fd_); stdin_fd_ = -1; }
        if (stdout_fd_ >= 0) { close(stdout_fd_); stdout_fd_ = -1; }
    }

    if (child_pid_ > 0) {
        kill(child_pid_, SIGTERM);
        int status;
        int waited = 0;
        while (waitpid(child_pid_, &status, WNOHANG) == 0 && waited < 50) {
            usleep(100000);
            waited++;
        }
        if (waited >= 50) {
            kill(child_pid_, SIGKILL);
            waitpid(child_pid_, &status, 0);
        }
        child_pid_ = -1;
    }
}

std::string StdioTransport::read_line() {
    std::string line;
    char c;

    struct pollfd pfd;
    pfd.fd = stdout_fd_;
    pfd.events = POLLIN;

    while (true) {
        int ret = poll(&pfd, 1, 30000);
        if (ret <= 0) break;

        ssize_t n = read(stdout_fd_, &c, 1);
        if (n <= 0) break;
        if (c == '\n') break;
        line += c;
    }
    return line;
}

void StdioTransport::write_message_locked(const json& msg) {
    std::string data = msg.dump() + "\n";
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = write(stdin_fd_, data.c_str() + total, data.size() - total);
        if (n <= 0) {
            spdlog::error("Failed to write to MCP server stdin");
            break;
        }
        total += n;
    }
}

json StdioTransport::send_request(const std::string& method, const json& params) {
    std::lock_guard<std::mutex> lock(io_mutex_);

    int id = ++request_id_;

    json request = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    write_message_locked(request);

    while (connected_) {
        std::string line = read_line();
        if (line.empty()) continue;

        try {
            json response = json::parse(line);
            if (response.contains("id") && response["id"] == id) {
                if (response.contains("error")) {
                    spdlog::error("MCP error: {}", response["error"].dump());
                    return json::object();
                }
                return response.value("result", json::object());
            }
            if (response.contains("method")) {
                continue;
            }
        } catch (const json::parse_error& e) {
            spdlog::debug("Failed to parse MCP response: {}", e.what());
        }
    }

    return json::object();
}

bool StdioTransport::is_connected() const {
    return connected_;
}

// --- SSETransport ---

SSETransport::SSETransport(const std::string& url,
                           const std::map<std::string, std::string>& headers)
    : url_(url), headers_(headers) {}

SSETransport::~SSETransport() {
    disconnect();
}

bool SSETransport::connect() {
    std::string host = url_host(url_);
    int port = url_port(url_);
    std::string path = url_path(url_);
    bool is_https = url_is_https(url_);

    auto cli = std::make_unique<httplib::Client>(
        (is_https ? "https://" : "http://") + host + ":" + std::to_string(port));
    cli->set_read_timeout(60, 0);

    httplib::Headers hdrs;
    for (auto& [k, v] : headers_) hdrs.emplace(k, v);
    hdrs.emplace("Accept", "text/event-stream");

    std::string base_url = (is_https ? "https://" : "http://") + host + ":" + std::to_string(port);
    bool endpoint_found = false;

    auto res = cli->Get(path, hdrs,
        [&](const char* data, size_t len) -> bool {
            std::string chunk(data, len);
            std::istringstream stream(chunk);
            std::string line;
            while (std::getline(stream, line)) {
                if (line.empty() || line == "\r") continue;
                if (line.back() == '\r') line.pop_back();

                if (line.substr(0, 5) == "data:") {
                    std::string value = line.substr(5);
                    if (!value.empty() && value[0] == ' ') value = value.substr(1);

                    if (!endpoint_found && value.find("/") != std::string::npos) {
                        if (value[0] == '/') {
                            messages_endpoint_ = base_url + value;
                        } else {
                            messages_endpoint_ = value;
                        }
                        endpoint_found = true;
                        connected_ = true;
                        return false;
                    }

                    try {
                        json msg = json::parse(value);
                        if (msg.contains("id")) {
                            std::lock_guard<std::mutex> lock(responses_mutex_);
                            responses_[msg["id"].get<int>()] = msg;
                            responses_cv_.notify_all();
                        }
                    } catch (...) {}
                }
            }
            return true;
        });

    if (!endpoint_found) {
        spdlog::error("SSE: Failed to discover messages endpoint from {}", url_);
        return false;
    }

    sse_thread_ = std::thread([this, base_url, path, is_https, host, port]() {
        auto thread_cli = std::make_unique<httplib::Client>(
            (is_https ? "https://" : "http://") + host + ":" + std::to_string(port));
        thread_cli->set_read_timeout(0, 0);

        httplib::Headers hdrs;
        for (auto& [k, v] : headers_) hdrs.emplace(k, v);
        hdrs.emplace("Accept", "text/event-stream");

        thread_cli->Get(path, hdrs,
            [&](const char* data, size_t len) -> bool {
                if (!connected_) return false;
                std::string chunk(data, len);
                std::istringstream stream(chunk);
                std::string line;
                while (std::getline(stream, line)) {
                    if (line.empty() || line == "\r") continue;
                    if (line.back() == '\r') line.pop_back();
                    if (line.substr(0, 5) == "data:") {
                        std::string value = line.substr(5);
                        if (!value.empty() && value[0] == ' ') value = value.substr(1);
                        try {
                            json msg = json::parse(value);
                            if (msg.contains("id")) {
                                std::lock_guard<std::mutex> lock(responses_mutex_);
                                responses_[msg["id"].get<int>()] = msg;
                                responses_cv_.notify_all();
                            }
                        } catch (...) {}
                    }
                }
                return connected_.load();
            });
    });

    return true;
}

void SSETransport::disconnect() {
    connected_ = false;
    responses_cv_.notify_all();
    if (sse_thread_.joinable()) {
        sse_thread_.join();
    }
}

json SSETransport::send_request(const std::string& method, const json& params) {
    if (messages_endpoint_.empty()) {
        spdlog::error("SSE: No messages endpoint available");
        return json::object();
    }

    int id = ++request_id_;
    json request = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    std::string host = url_host(messages_endpoint_);
    int port = url_port(messages_endpoint_);
    std::string path = url_path(messages_endpoint_);
    bool is_https = url_is_https(messages_endpoint_);

    auto cli = std::make_unique<httplib::Client>(
        (is_https ? "https://" : "http://") + host + ":" + std::to_string(port));
    cli->set_read_timeout(30, 0);

    httplib::Headers hdrs;
    for (auto& [k, v] : headers_) hdrs.emplace(k, v);
    hdrs.emplace("Content-Type", "application/json");

    auto res = cli->Post(path, hdrs, request.dump(), "application/json");
    if (res && res->status == 200) {
        try {
            return json::parse(res->body).value("result", json::object());
        } catch (...) {}
    }

    std::unique_lock<std::mutex> lock(responses_mutex_);
    responses_cv_.wait_for(lock, std::chrono::seconds(30), [&]() {
        return responses_.count(id) > 0;
    });

    if (responses_.count(id)) {
        json resp = responses_[id];
        responses_.erase(id);
        if (resp.contains("error")) {
            spdlog::error("SSE MCP error: {}", resp["error"].dump());
            return json::object();
        }
        return resp.value("result", json::object());
    }

    spdlog::error("SSE: Timeout waiting for response to request {}", id);
    return json::object();
}

bool SSETransport::is_connected() const {
    return connected_;
}

// --- StreamableHTTPTransport ---

StreamableHTTPTransport::StreamableHTTPTransport(const std::string& url,
                                                 const std::map<std::string, std::string>& headers)
    : url_(url), headers_(headers) {}

StreamableHTTPTransport::~StreamableHTTPTransport() {
    disconnect();
}

bool StreamableHTTPTransport::connect() {
    connected_ = true;
    return true;
}

void StreamableHTTPTransport::disconnect() {
    connected_ = false;
}

json StreamableHTTPTransport::send_request(const std::string& method, const json& params) {
    int id = ++request_id_;
    json request = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", params}
    };

    std::string host = url_host(url_);
    int port = url_port(url_);
    std::string path = url_path(url_);
    bool is_https = url_is_https(url_);

    auto cli = std::make_unique<httplib::Client>(
        (is_https ? "https://" : "http://") + host + ":" + std::to_string(port));
    cli->set_read_timeout(60, 0);

    httplib::Headers hdrs;
    for (auto& [k, v] : headers_) hdrs.emplace(k, v);
    hdrs.emplace("Content-Type", "application/json");
    hdrs.emplace("Accept", "application/json, text/event-stream");
    if (session_id_) {
        hdrs.emplace("Mcp-Session-Id", *session_id_);
    }

    auto res = cli->Post(path, hdrs, request.dump(), "application/json");
    if (!res) {
        spdlog::error("StreamableHTTP: Request failed for method '{}'", method);
        return json::object();
    }

    auto session_it = res->headers.find("Mcp-Session-Id");
    if (session_it != res->headers.end()) {
        session_id_ = session_it->second;
    }

    if (res->status != 200) {
        spdlog::error("StreamableHTTP: HTTP {} for method '{}'", res->status, method);
        return json::object();
    }

    std::string content_type;
    auto ct_it = res->headers.find("Content-Type");
    if (ct_it != res->headers.end()) {
        content_type = ct_it->second;
    }

    if (content_type.find("text/event-stream") != std::string::npos) {
        std::istringstream stream(res->body);
        std::string line;
        json result = json::object();
        while (std::getline(stream, line)) {
            if (line.empty() || line == "\r") continue;
            if (line.back() == '\r') line.pop_back();
            if (line.substr(0, 5) == "data:") {
                std::string value = line.substr(5);
                if (!value.empty() && value[0] == ' ') value = value.substr(1);
                try {
                    json msg = json::parse(value);
                    if (msg.contains("id") && msg["id"] == id) {
                        if (msg.contains("error")) {
                            spdlog::error("StreamableHTTP MCP error: {}", msg["error"].dump());
                            return json::object();
                        }
                        result = msg.value("result", json::object());
                    }
                } catch (...) {}
            }
        }
        return result;
    }

    try {
        json resp = json::parse(res->body);
        if (resp.contains("error")) {
            spdlog::error("StreamableHTTP MCP error: {}", resp["error"].dump());
            return json::object();
        }
        return resp.value("result", json::object());
    } catch (const json::parse_error& e) {
        spdlog::error("StreamableHTTP: Failed to parse response: {}", e.what());
        return json::object();
    }
}

bool StreamableHTTPTransport::is_connected() const {
    return connected_;
}

// --- McpClient ---

McpClient::McpClient(std::unique_ptr<McpTransport> transport)
    : transport_(std::move(transport)) {}

McpClient::~McpClient() {
    close();
}

bool McpClient::initialize() {
    if (!transport_->connect()) return false;

    json params = {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", json::object()},
        {"clientInfo", {
            {"name", "ollama-mcp-bridge-cpp"},
            {"version", "0.1.0"}
        }}
    };

    json result = transport_->send_request("initialize", params);
    if (result.empty()) {
        spdlog::error("MCP initialization failed");
        return false;
    }

    transport_->send_request("notifications/initialized", json::object());
    return true;
}

std::vector<McpTool> McpClient::list_tools() {
    std::vector<McpTool> tools;
    json result = transport_->send_request("tools/list", json::object());

    if (result.contains("tools") && result["tools"].is_array()) {
        for (auto& t : result["tools"]) {
            McpTool tool;
            tool.name = t.value("name", "");
            tool.description = t.value("description", "");
            tool.input_schema = t.value("inputSchema", json::object());
            tools.push_back(std::move(tool));
        }
    }
    return tools;
}

McpToolResult McpClient::call_tool(const std::string& name, const json& arguments) {
    json params = {
        {"name", name},
        {"arguments", arguments}
    };

    json result = transport_->send_request("tools/call", params);
    McpToolResult tool_result;

    if (result.contains("content") && result["content"].is_array() && !result["content"].empty()) {
        auto& first = result["content"][0];
        if (first.contains("text")) {
            tool_result.text = first["text"].get<std::string>();
        } else if (first.contains("data")) {
            auto& data = first["data"];
            tool_result.text = data.is_string() ? data.get<std::string>() : data.dump();
        } else if (first.contains("value")) {
            auto& val = first["value"];
            tool_result.text = val.is_string() ? val.get<std::string>() : val.dump();
        } else {
            tool_result.text = first.dump();
        }
    } else if (result.contains("isError") && result["isError"].get<bool>()) {
        tool_result.success = false;
        tool_result.text = "Tool returned error";
    } else {
        tool_result.text = "Tool returned no content";
    }

    return tool_result;
}

void McpClient::close() {
    if (transport_) {
        transport_->disconnect();
    }
}

bool McpClient::is_connected() const {
    return transport_ && transport_->is_connected();
}

}
