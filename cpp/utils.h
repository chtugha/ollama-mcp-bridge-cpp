#pragma once

#include <string>
#include <map>
#include <vector>
#include <tuple>
#include <optional>
#include <functional>
#include <nlohmann/json.hpp>

namespace omb {

using json = nlohmann::json;

std::tuple<bool, std::optional<double>> get_ollama_proxy_timeout_config();

std::tuple<bool, std::optional<std::string>> is_port_in_use(const std::string& host, int port);

bool check_ollama_health(const std::string& ollama_url, int timeout_sec = 3);

std::string expand_env_vars(const std::string& value, const std::string& cwd = "");

json expand_dict_env_vars(const json& data, const std::string& cwd = "");

struct CliInputs {
    std::string config;
    std::string host;
    int port;
    std::string ollama_url;
    std::optional<int> max_tool_rounds;
    std::optional<std::string> system_prompt;
};

std::optional<std::string> validate_cli_inputs(const CliInputs& inputs);

std::string http_get(const std::string& url, int timeout_sec = 5);
std::string http_post(const std::string& url, const std::string& body, int timeout_sec = 0);

struct HttpResponse {
    int status;
    std::string body;
    std::map<std::string, std::string> headers;
};

HttpResponse http_request(const std::string& method, const std::string& url,
                          const std::map<std::string, std::string>& headers = {},
                          const std::string& body = "",
                          int timeout_sec = 0);

using StreamCallback = std::function<void(const std::string& chunk)>;
void http_post_stream(const std::string& url, const std::string& body,
                      StreamCallback callback, int timeout_sec = 0);

std::string get_env(const std::string& name, const std::string& default_val = "");

void atomic_write_file(const std::string& path, const std::string& content);
bool parse_bool_env(const std::string& val);
std::string url_encode(const std::string& s);

std::string url_host(const std::string& url);
int url_port(const std::string& url);
std::string url_path(const std::string& url);
bool url_is_https(const std::string& url);

}
