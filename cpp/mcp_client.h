#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <nlohmann/json.hpp>

namespace omb {

using json = nlohmann::json;

struct McpTool {
    std::string name;
    std::string description;
    json input_schema;
};

struct McpToolResult {
    bool success = true;
    std::string text;
};

enum class McpTransportType {
    Stdio,
    SSE,
    StreamableHTTP
};

class McpTransport {
public:
    virtual ~McpTransport() = default;
    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual json send_request(const std::string& method, const json& params = json::object()) = 0;
    virtual bool is_connected() const = 0;
};

class StdioTransport : public McpTransport {
public:
    StdioTransport(const std::string& command, const std::vector<std::string>& args = {},
                   const std::map<std::string, std::string>& env = {},
                   const std::string& cwd = "");
    ~StdioTransport() override;

    bool connect() override;
    void disconnect() override;
    json send_request(const std::string& method, const json& params = json::object()) override;
    bool is_connected() const override;

private:
    std::string command_;
    std::vector<std::string> args_;
    std::map<std::string, std::string> env_;
    std::string cwd_;
    pid_t child_pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    std::atomic<bool> connected_{false};
    std::atomic<int> request_id_{0};
    std::mutex io_mutex_;

    std::string read_line();
    void write_message_locked(const json& msg);
};

class SSETransport : public McpTransport {
public:
    SSETransport(const std::string& url, const std::map<std::string, std::string>& headers = {});
    ~SSETransport() override;

    bool connect() override;
    void disconnect() override;
    json send_request(const std::string& method, const json& params = json::object()) override;
    bool is_connected() const override;

private:
    std::string url_;
    std::map<std::string, std::string> headers_;
    std::string messages_endpoint_;
    std::atomic<bool> connected_{false};
    std::atomic<int> request_id_{0};
    std::mutex mutex_;

    std::thread sse_thread_;
    std::map<int, json> responses_;
    std::mutex responses_mutex_;
    std::condition_variable responses_cv_;

    void sse_listen_loop();
};

class StreamableHTTPTransport : public McpTransport {
public:
    StreamableHTTPTransport(const std::string& url, const std::map<std::string, std::string>& headers = {});
    ~StreamableHTTPTransport() override;

    bool connect() override;
    void disconnect() override;
    json send_request(const std::string& method, const json& params = json::object()) override;
    bool is_connected() const override;

private:
    std::string url_;
    std::map<std::string, std::string> headers_;
    std::atomic<bool> connected_{false};
    std::atomic<int> request_id_{0};
    std::optional<std::string> session_id_;
};

class McpClient {
public:
    McpClient(std::unique_ptr<McpTransport> transport);
    ~McpClient();

    bool initialize();
    std::vector<McpTool> list_tools();
    McpToolResult call_tool(const std::string& name, const json& arguments);
    void close();
    bool is_connected() const;

private:
    std::unique_ptr<McpTransport> transport_;
};

}
