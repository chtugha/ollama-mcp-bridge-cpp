#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace omb {

struct AppState {
    std::atomic<bool> proxy_running{false};
    std::atomic<bool> ollama_reachable{false};
    std::atomic<bool> shutdown_requested{false};
    std::atomic<bool> fail_on_ollama_unavailable{false};

    const std::chrono::steady_clock::time_point start_time{std::chrono::steady_clock::now()};

    // All fields below must be read and written while holding mutex_.
    mutable std::mutex mutex_;

    // Separate mutex for proxy_thread join/swap — decouples potentially-long
    // blocking joins from the broader config/state lock above.
    mutable std::mutex proxy_thread_mutex_;

    std::string ollama_url;
    std::optional<int> max_tool_rounds;
    std::optional<std::string> system_prompt;
    std::string cors_origins;

    std::string web_host;
    int web_port{11464};
    bool web_tls{false};
    std::string web_cert_file;
    std::string web_key_file;

    std::string proxy_host;
    int proxy_port{8000};
    std::string config_file;

    std::string active_cert_name;
    std::string cert_dir;

    // Protected by proxy_thread_mutex_ (not mutex_) to avoid blocking all
    // state readers during a potentially-long thread join.
    std::optional<std::thread> proxy_thread;
};

}
