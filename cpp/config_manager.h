#pragma once

#include <filesystem>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace omb {

class ConfigManager {
public:
    explicit ConfigManager(const std::string& config_file_path);

    nlohmann::json get_mcp_config() const;
    void save_mcp_config(const nlohmann::json& cfg);

    std::string get_active_cert() const;
    void set_active_cert(const std::string& name);

    std::filesystem::path config_dir() const;
    std::filesystem::path state_file_path() const;

private:
    std::string config_path_;
    nlohmann::json mcp_config_;
    nlohmann::json bridge_state_;
    mutable std::mutex mutex_;
};

}
