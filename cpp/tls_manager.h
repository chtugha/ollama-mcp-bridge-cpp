#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "config_manager.h"

namespace omb {

struct CertInfo {
    std::string name;
    std::string cert_file;
    std::string key_file;
    std::string common_name;
    std::vector<std::string> sans;
    std::chrono::system_clock::time_point not_after;
    bool is_active{false};
    bool is_expiring_soon{false};
};

struct GenJob {
    std::string job_id;
    enum class Status { running, done, error } status{Status::running};
    std::string cert_name;
    std::string error_msg;
    std::chrono::steady_clock::time_point created_at;
};

class TlsManager {
public:
    TlsManager(const std::string& cert_dir, std::shared_ptr<ConfigManager> cfg_mgr);
    ~TlsManager();

    std::vector<CertInfo> list_certs(const std::string& active_cert_name) const;

    std::string start_generate_job(const std::string& cn, int days,
                                   int key_size, const std::vector<std::string>& sans);

    std::optional<GenJob> get_job(const std::string& job_id);

    void upload_cert(const std::string& name,
                     const std::string& cert_pem, const std::string& key_pem);

    void delete_cert(const std::string& name, bool tls_enabled,
                     const std::string& active_cert);

    void activate_cert(const std::string& name);

private:
    std::string cert_dir_;
    std::shared_ptr<ConfigManager> cfg_mgr_;
    mutable std::mutex jobs_mutex_;
    std::map<std::string, GenJob> jobs_;
    std::map<std::string, std::thread> gen_threads_;

    void prune_expired_jobs();

    void generate_cert_thread(const std::string& job_id, const std::string& cn,
                               int days, int key_size,
                               const std::vector<std::string>& sans);
};

}
