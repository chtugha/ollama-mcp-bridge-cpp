#include "utils.h"

#include <atomic>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <filesystem>
#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

#include <httplib.h>
#include <spdlog/spdlog.h>

namespace omb {

static std::atomic<bool> ollama_proxy_timeout_warned{false};

std::string get_env(const std::string& name, const std::string& default_val) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : default_val;
}

std::tuple<bool, std::optional<double>> get_ollama_proxy_timeout_config() {
    auto raw = get_env("OLLAMA_PROXY_TIMEOUT");
    if (raw.empty()) return {false, std::nullopt};

    try {
        int timeout_ms = std::stoi(raw);
        if (timeout_ms < 0) {
            spdlog::warn("Ignoring OLLAMA_PROXY_TIMEOUT={}: must be >= 0 (milliseconds).", timeout_ms);
            return {false, std::nullopt};
        }
        if (timeout_ms == 0) {
            if (!ollama_proxy_timeout_warned.exchange(true)) {
                spdlog::warn("OLLAMA_PROXY_TIMEOUT=0 disables HTTP timeouts for Ollama requests. "
                             "This may cause requests to hang indefinitely if Ollama stops responding.");
            }
            return {true, std::nullopt};
        }
        return {true, timeout_ms / 1000.0};
    } catch (...) {
        spdlog::warn("Ignoring OLLAMA_PROXY_TIMEOUT='{}': expected an integer number of milliseconds.", raw);
        return {false, std::nullopt};
    }
}

std::tuple<bool, std::optional<std::string>> is_port_in_use(const std::string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return {true, "Failed to create socket: " + std::string(strerror(errno))};
    }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        struct hostent* he = gethostbyname(host.c_str());
        if (!he) {
            close(sock);
            return {true, "Cannot bind to host '" + host + "': address not available. Please check the --host value."};
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    int result = bind(sock, (struct sockaddr*)&addr, sizeof(addr));
    int err = errno;
    close(sock);

    if (result < 0) {
        if (err == EADDRINUSE) {
            return {true, "Port " + std::to_string(port) + " is already in use on " + host + ". Please use a different port with --port."};
        } else if (err == EADDRNOTAVAIL) {
            return {true, "Cannot bind to host '" + host + "': address not available. Please check the --host value."};
        } else {
            return {true, "Cannot bind to " + host + ":" + std::to_string(port) + ": " + strerror(err)};
        }
    }
    return {false, std::nullopt};
}

bool url_is_https(const std::string& url) {
    return url.substr(0, 8) == "https://";
}

std::string url_host(const std::string& url) {
    std::string s = url;
    auto pos = s.find("://");
    if (pos != std::string::npos) s = s.substr(pos + 3);
    pos = s.find('/');
    if (pos != std::string::npos) s = s.substr(0, pos);
    pos = s.find(':');
    if (pos != std::string::npos) s = s.substr(0, pos);
    return s;
}

int url_port(const std::string& url) {
    std::string s = url;
    auto pos = s.find("://");
    if (pos != std::string::npos) s = s.substr(pos + 3);
    pos = s.find('/');
    if (pos != std::string::npos) s = s.substr(0, pos);
    pos = s.find(':');
    if (pos != std::string::npos) {
        try { return std::stoi(s.substr(pos + 1)); } catch (...) {}
    }
    return url_is_https(url) ? 443 : 80;
}

std::string url_path(const std::string& url) {
    std::string s = url;
    auto pos = s.find("://");
    if (pos != std::string::npos) s = s.substr(pos + 3);
    pos = s.find('/');
    if (pos != std::string::npos) return s.substr(pos);
    return "/";
}

static constexpr int kDefaultConnectionTimeoutSec = 30;

static std::unique_ptr<httplib::Client> make_client(const std::string& url, int timeout_sec) {
    std::string host = url_host(url);
    int port = url_port(url);
    std::string scheme = url_is_https(url) ? "https" : "http";
    auto cli = std::make_unique<httplib::Client>(scheme + "://" + host + ":" + std::to_string(port));
    if (timeout_sec > 0) {
        cli->set_read_timeout(timeout_sec, 0);
        cli->set_connection_timeout(timeout_sec, 0);
    } else {
        cli->set_connection_timeout(kDefaultConnectionTimeoutSec, 0);
        cli->set_read_timeout(0, 0);
    }
    return cli;
}

std::string http_get(const std::string& url, int timeout_sec) {
    auto cli = make_client(url, timeout_sec);
    std::string path = url_path(url);
    auto res = cli->Get(path);
    if (res && res->status == 200) return res->body;
    return "";
}

std::string http_post(const std::string& url, const std::string& body, int timeout_sec) {
    auto cli = make_client(url, timeout_sec);
    std::string path = url_path(url);
    auto res = cli->Post(path, body, "application/json");
    if (res) return res->body;
    return "";
}

HttpResponse http_request(const std::string& method, const std::string& url,
                          const std::map<std::string, std::string>& headers,
                          const std::string& body, int timeout_sec) {
    auto cli = make_client(url, timeout_sec);
    std::string path = url_path(url);

    httplib::Headers hdrs;
    for (auto& [k, v] : headers) hdrs.emplace(k, v);

    auto do_request = [&]() -> httplib::Result {
        if (method == "GET") return cli->Get(path, hdrs);
        if (method == "POST") return cli->Post(path, hdrs, body, "application/json");
        if (method == "PUT") return cli->Put(path, hdrs, body, "application/json");
        if (method == "DELETE") return cli->Delete(path, hdrs, body, "application/json");
        if (method == "PATCH") return cli->Patch(path, hdrs, body, "application/json");
        if (method == "OPTIONS") return cli->Options(path, hdrs);
        if (method == "HEAD") return cli->Head(path, hdrs);
        spdlog::warn("Unsupported HTTP method '{}', falling back to GET", method);
        return cli->Get(path, hdrs);
    };

    auto res = do_request();

    HttpResponse resp;
    if (res) {
        resp.status = res->status;
        resp.body = res->body;
        for (auto& [k, v] : res->headers) resp.headers[k] = v;
    } else {
        resp.status = 503;
        resp.body = "Connection failed";
    }
    return resp;
}

void http_post_stream(const std::string& url, const std::string& body,
                      StreamCallback callback, int timeout_sec) {
    auto cli = make_client(url, timeout_sec);
    std::string path = url_path(url);

    httplib::Request req;
    req.method = "POST";
    req.path = path;
    req.set_header("Content-Type", "application/json");
    req.body = body;
    req.content_receiver =
        [&callback](const char* data, size_t data_length,
                    uint64_t, uint64_t) -> bool {
            callback(std::string(data, data_length));
            return true;
        };

    auto result = cli->send(req);
    if (!result) {
        spdlog::error("http_post_stream failed: {}", httplib::to_string(result.error()));
    }
}

bool check_ollama_health(const std::string& ollama_url, int timeout_sec) {
    try {
        auto [is_set, timeout_override] = get_ollama_proxy_timeout_config();
        int effective_timeout = is_set && timeout_override ? static_cast<int>(*timeout_override) : timeout_sec;
        std::string result = http_get(ollama_url + "/api/tags", effective_timeout);
        if (!result.empty()) {
            spdlog::info("Ollama server is accessible");
            return true;
        }
        spdlog::error("Ollama server not accessible at {}", ollama_url);
        return false;
    } catch (const std::exception& e) {
        spdlog::error("Failed to connect to Ollama: {}", e.what());
        return false;
    }
}

std::string expand_env_vars(const std::string& value, const std::string& cwd) {
    std::string result = value;
    std::string effective_cwd = cwd.empty() ? std::filesystem::current_path().string() : cwd;

    size_t pos;
    while ((pos = result.find("${workspaceFolder}")) != std::string::npos) {
        result.replace(pos, 18, effective_cwd);
    }

    std::regex env_pattern(R"(\$\{env:([^}]+)\})");
    std::smatch match;
    while (std::regex_search(result, match, env_pattern)) {
        std::string var_name = match[1].str();
        std::string env_value = get_env(var_name);
        result = match.prefix().str() + env_value + match.suffix().str();
    }

    return result;
}

json expand_dict_env_vars(const json& data, const std::string& cwd) {
    json result = json::object();
    for (auto& [key, value] : data.items()) {
        if (value.is_string()) {
            result[key] = expand_env_vars(value.get<std::string>(), cwd);
        } else if (value.is_object()) {
            result[key] = expand_dict_env_vars(value, cwd);
        } else if (value.is_array()) {
            json arr = json::array();
            for (auto& item : value) {
                if (item.is_string()) {
                    arr.push_back(expand_env_vars(item.get<std::string>(), cwd));
                } else if (item.is_object()) {
                    arr.push_back(expand_dict_env_vars(item, cwd));
                } else {
                    arr.push_back(item);
                }
            }
            result[key] = arr;
        } else {
            result[key] = value;
        }
    }
    return result;
}

std::optional<std::string> validate_cli_inputs(const CliInputs& inputs) {
    if (!std::filesystem::exists(inputs.config)) {
        return "Config file not found: " + inputs.config;
    }
    if (inputs.port < 1 || inputs.port > 65535) {
        return "Port must be between 1 and 65535, got " + std::to_string(inputs.port);
    }
    if (inputs.host.empty()) {
        return "Host must be a non-empty string";
    }
    std::regex url_pattern(R"(^https?://[\w\.\-]+(:\d+)?)");
    if (!std::regex_search(inputs.ollama_url, url_pattern)) {
        return "Invalid Ollama URL: " + inputs.ollama_url;
    }
    if (inputs.max_tool_rounds && *inputs.max_tool_rounds < 1) {
        return "max_tool_rounds must be at least 1, got " + std::to_string(*inputs.max_tool_rounds);
    }
    static constexpr size_t kMaxSystemPromptLength = 10000;
    if (inputs.system_prompt) {
        if (inputs.system_prompt->empty()) {
            return "system_prompt must be a non-empty string";
        }
        if (inputs.system_prompt->size() > kMaxSystemPromptLength) {
            return "system_prompt is too long (max " + std::to_string(kMaxSystemPromptLength) + " characters)";
        }
    }
    return std::nullopt;
}

}
