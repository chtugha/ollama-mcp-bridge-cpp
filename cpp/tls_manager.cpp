#include "tls_manager.h"
#include "utils.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>

#ifdef _WIN32
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <spdlog/spdlog.h>

namespace omb {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static void validate_cert_name(const std::string& name) {
    if (name.empty())
        throw std::invalid_argument("Certificate name must not be empty");
    for (char c : name) {
        if (!isalnum(static_cast<unsigned char>(c)) &&
            c != '.' && c != '-' && c != '_')
            throw std::invalid_argument(
                "Certificate name contains invalid characters "
                "(allowed: a-z A-Z 0-9 . - _): " + name);
    }
    if (name.front() == '.')
        throw std::invalid_argument("Certificate name must not start with '.': " + name);
}

static std::string random_hex(size_t n) {
    static const char hex[] = "0123456789abcdef";
    static std::mt19937 gen{std::random_device{}()};
    static std::mutex mtx;
    std::uniform_int_distribution<int> dis(0, 15);
    std::string result(n, '0');
    std::lock_guard<std::mutex> lock(mtx);
    for (auto& c : result) c = hex[dis(gen)];
    return result;
}

static std::string get_cn(X509* cert) {
    X509_NAME* name = X509_get_subject_name(cert);
    if (!name) return "";
    char buf[256] = {};
    int len = X509_NAME_get_text_by_NID(name, NID_commonName, buf, sizeof(buf));
    if (len < 0) return "";
    return std::string(buf, static_cast<size_t>(len));
}

static std::vector<std::string> get_sans(X509* cert) {
    std::vector<std::string> result;
    GENERAL_NAMES* gens = static_cast<GENERAL_NAMES*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    if (!gens) return result;

    for (int i = 0; i < sk_GENERAL_NAME_num(gens); ++i) {
        GENERAL_NAME* gen = sk_GENERAL_NAME_value(gens, i);
        if (gen->type == GEN_DNS) {
            const unsigned char* data = ASN1_STRING_get0_data(gen->d.dNSName);
            int len = ASN1_STRING_length(gen->d.dNSName);
            result.emplace_back(reinterpret_cast<const char*>(data),
                                static_cast<size_t>(len));
        } else if (gen->type == GEN_IPADD) {
            const unsigned char* data = ASN1_STRING_get0_data(gen->d.iPAddress);
            int len = ASN1_STRING_length(gen->d.iPAddress);
            if (len == 4) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, data, ip, sizeof(ip));
                result.push_back(ip);
            } else if (len == 16) {
                char ip[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, data, ip, sizeof(ip));
                result.push_back(ip);
            }
        }
    }
    GENERAL_NAMES_free(gens);
    return result;
}

static std::chrono::system_clock::time_point asn1_time_to_tp(const ASN1_TIME* t) {
    struct tm tm_val = {};
    if (ASN1_TIME_to_tm(t, &tm_val) != 1) {
        return std::chrono::system_clock::time_point{};
    }
#ifdef _WIN32
    time_t tt = _mkgmtime(&tm_val);
#else
    time_t tt = timegm(&tm_val);
#endif
    return std::chrono::system_clock::from_time_t(tt);
}

static bool looks_like_ip(const std::string& s) {
    struct in_addr  a4;
    struct in6_addr a6;
    return inet_pton(AF_INET,  s.c_str(), &a4) == 1 ||
           inet_pton(AF_INET6, s.c_str(), &a6) == 1;
}

static std::string sanitize_cert_name(const std::string& cn) {
    std::string name = cn;
    for (auto& c : name) {
        if (!isalnum(static_cast<unsigned char>(c)) &&
            c != '-' && c != '.' && c != '_')
            c = '_';
    }
    return name;
}

// Write a private key file with mode 0600 before the rename so it is never
// world-readable even for the brief window between creation and chmod.
static void write_private_key_secure(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
#ifndef _WIN32
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        throw std::runtime_error("write_private_key_secure: cannot open " + tmp +
                                 ": " + strerror(errno));
    const char* data = content.data();
    size_t remaining = content.size();
    while (remaining > 0) {
        ssize_t n = ::write(fd, data, remaining);
        if (n < 0) {
            int saved = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            throw std::runtime_error("write_private_key_secure: write failed: " +
                                     std::string(strerror(saved)));
        }
        data += static_cast<size_t>(n);
        remaining -= static_cast<size_t>(n);
    }
    ::close(fd);
#else
    {
        std::ofstream ofs(tmp, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!ofs)
            throw std::runtime_error("write_private_key_secure: cannot open " + tmp);
        ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!ofs) {
            std::error_code ec;
            fs::remove(tmp, ec);
            throw std::runtime_error("write_private_key_secure: write failed for " + tmp);
        }
    }
#endif
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        int saved = errno;
        std::error_code ec;
        fs::remove(tmp, ec);
        throw std::runtime_error("write_private_key_secure: rename failed: " +
                                 std::string(strerror(saved)));
    }
}

// RAII guard: removes a file unless dismissed
struct ScopedRemove {
    std::string path;
    bool dismiss{false};
    ~ScopedRemove() {
        if (!dismiss) {
            std::error_code ec;
            fs::remove(path, ec);
        }
    }
};

// ---------------------------------------------------------------------------
// TlsManager
// ---------------------------------------------------------------------------

TlsManager::TlsManager(const std::string& cert_dir,
                       std::shared_ptr<ConfigManager> cfg_mgr)
    : cert_dir_(cert_dir), cfg_mgr_(std::move(cfg_mgr)) {
    if (!cert_dir_.empty()) {
        std::error_code ec;
        fs::create_directories(cert_dir_, ec);
    }
}

TlsManager::~TlsManager() {
    std::map<std::string, std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        to_join = std::move(gen_threads_);
        gen_threads_.clear();
    }
    for (auto& [id, t] : to_join) {
        if (t.joinable()) t.join();
    }
}

std::vector<CertInfo> TlsManager::list_certs(const std::string& active_cert_name) const {
    std::vector<CertInfo> result;
    if (cert_dir_.empty() || !fs::exists(cert_dir_)) return result;

    auto now            = std::chrono::system_clock::now();
    auto soon_threshold = now + std::chrono::hours(24 * 30);

    for (const auto& entry : fs::directory_iterator(cert_dir_)) {
        if (!entry.is_regular_file()) continue;
        auto ext = entry.path().extension().string();
        if (ext != ".pem" && ext != ".crt") continue;

        auto stem     = entry.path().stem().string();
        auto key_path = fs::path(cert_dir_) / (stem + ".key");
        if (!fs::exists(key_path)) continue;

        auto cert_path_str = entry.path().string();
        FILE* fp = fopen(cert_path_str.c_str(), "r");
        if (!fp) continue;

        X509* cert = PEM_read_X509(fp, nullptr, nullptr, nullptr);
        fclose(fp);
        if (!cert) continue;

        CertInfo info;
        info.name         = stem;
        info.cert_file    = cert_path_str;
        info.key_file     = key_path.string();
        info.common_name  = get_cn(cert);
        info.sans         = get_sans(cert);
        info.not_after    = asn1_time_to_tp(X509_get0_notAfter(cert));
        info.is_active    = (stem == active_cert_name);

        bool parsed_ok = (info.not_after != std::chrono::system_clock::time_point{});
        info.is_expiring_soon = !parsed_ok || (info.not_after < soon_threshold);

        X509_free(cert);
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(),
              [](const CertInfo& a, const CertInfo& b) { return a.name < b.name; });
    return result;
}

// Called with jobs_mutex_ held.
void TlsManager::prune_expired_jobs() {
    auto now = std::chrono::steady_clock::now();
    constexpr auto cutoff = std::chrono::minutes(5);

    std::vector<std::string> to_erase;
    for (auto& [id, job] : jobs_) {
        bool finished = (job.status == GenJob::Status::done ||
                         job.status == GenJob::Status::error);
        if (finished) {
            // Join eagerly — safe because the thread already released jobs_mutex_
            // before we see status == done/error.
            auto it = gen_threads_.find(id);
            if (it != gen_threads_.end() && it->second.joinable()) {
                it->second.join();
                gen_threads_.erase(it);
            }
            if (now - job.created_at > cutoff) {
                to_erase.push_back(id);
            }
        }
    }
    for (const auto& id : to_erase) {
        jobs_.erase(id);
    }
}

std::string TlsManager::start_generate_job(const std::string& cn, int days,
                                            int key_size,
                                            const std::vector<std::string>& sans) {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    prune_expired_jobs();

    size_t running = 0;
    for (const auto& [id, job] : jobs_) {
        if (job.status == GenJob::Status::running) ++running;
    }
    if (running >= 20) {
        throw std::runtime_error("Too many concurrent generation jobs");
    }

    std::string job_id = random_hex(8);
    while (jobs_.count(job_id)) job_id = random_hex(8);

    GenJob job;
    job.job_id     = job_id;
    job.status     = GenJob::Status::running;
    job.cert_name  = sanitize_cert_name(cn);
    job.created_at = std::chrono::steady_clock::now();
    jobs_[job_id]  = job;

    gen_threads_[job_id] = std::thread(
        [this, job_id, cn, days, key_size, sans]() {
            generate_cert_thread(job_id, cn, days, key_size, sans);
        });

    return job_id;
}

std::optional<GenJob> TlsManager::get_job(const std::string& job_id) {
    std::lock_guard<std::mutex> lock(jobs_mutex_);
    prune_expired_jobs();

    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) return std::nullopt;

    const auto& job     = it->second;
    bool finished       = (job.status == GenJob::Status::done ||
                           job.status == GenJob::Status::error);
    auto age            = std::chrono::steady_clock::now() - job.created_at;
    if (finished && age > std::chrono::minutes(5)) {
        return std::nullopt;
    }
    return job;
}

void TlsManager::upload_cert(const std::string& name,
                              const std::string& cert_pem,
                              const std::string& key_pem) {
    validate_cert_name(name);

    X509*     cert = nullptr;
    EVP_PKEY* pkey = nullptr;

    {
        BIO* bio = BIO_new_mem_buf(cert_pem.data(), static_cast<int>(cert_pem.size()));
        if (!bio) throw std::runtime_error("BIO_new_mem_buf failed");
        cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!cert) throw std::runtime_error("Invalid certificate PEM");
    }
    {
        BIO* bio = BIO_new_mem_buf(key_pem.data(), static_cast<int>(key_pem.size()));
        if (!bio) { X509_free(cert); throw std::runtime_error("BIO_new_mem_buf failed"); }
        pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) {
            X509_free(cert);
            throw std::runtime_error("Invalid private key PEM");
        }
    }

    int match = X509_check_private_key(cert, pkey);
    X509_free(cert);
    EVP_PKEY_free(pkey);
    if (match != 1)
        throw std::runtime_error(
            "Certificate and private key do not match");

    std::error_code ec;
    fs::create_directories(cert_dir_, ec);

    auto cert_path = (fs::path(cert_dir_) / (name + ".crt")).string();
    auto key_path  = (fs::path(cert_dir_) / (name + ".key")).string();

    atomic_write_file(cert_path, cert_pem);
    try {
        write_private_key_secure(key_path, key_pem);
    } catch (...) {
        std::error_code ec;
        fs::remove(cert_path, ec);
        throw;
    }
}

void TlsManager::delete_cert(const std::string& name, bool tls_enabled,
                              const std::string& active_cert) {
    validate_cert_name(name);

    if (tls_enabled && name == active_cert) {
        throw std::runtime_error(
            "Cannot delete the active TLS certificate while TLS is enabled");
    }

    std::error_code ec;
    for (const char* ext : {".crt", ".pem"}) {
        auto p = fs::path(cert_dir_) / (name + ext);
        if (fs::exists(p, ec)) fs::remove(p, ec);
    }
    auto key_p = fs::path(cert_dir_) / (name + ".key");
    if (fs::exists(key_p, ec)) fs::remove(key_p, ec);
}

void TlsManager::activate_cert(const std::string& name) {
    validate_cert_name(name);
    cfg_mgr_->set_active_cert(name);
}

void TlsManager::generate_cert_thread(const std::string& job_id,
                                       const std::string& cn, int days,
                                       int key_size,
                                       const std::vector<std::string>& sans) {
    auto set_error = [&](const std::string& msg) {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        auto it = jobs_.find(job_id);
        if (it != jobs_.end()) {
            it->second.status    = GenJob::Status::error;
            it->second.error_msg = msg;
        }
    };

    try {
        // -- Key generation --
        EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!pctx) throw std::runtime_error("EVP_PKEY_CTX_new_id failed");

        if (EVP_PKEY_keygen_init(pctx) <= 0) {
            EVP_PKEY_CTX_free(pctx);
            throw std::runtime_error("EVP_PKEY_keygen_init failed");
        }
        if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, key_size) <= 0) {
            EVP_PKEY_CTX_free(pctx);
            throw std::runtime_error("EVP_PKEY_CTX_set_rsa_keygen_bits failed");
        }

        EVP_PKEY* pkey = nullptr;
        if (EVP_PKEY_keygen(pctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(pctx);
            throw std::runtime_error("EVP_PKEY_keygen failed");
        }
        EVP_PKEY_CTX_free(pctx);

        // -- Certificate --
        X509* cert = X509_new();
        if (!cert) {
            EVP_PKEY_free(pkey);
            throw std::runtime_error("X509_new failed");
        }

        X509_set_version(cert, 2);

        // Serial number
        BIGNUM* bn = BN_new();
        if (!bn || BN_rand(bn, 64, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY) != 1) {
            BN_free(bn);
            X509_free(cert);
            EVP_PKEY_free(pkey);
            throw std::runtime_error("BN_rand failed");
        }
        ASN1_INTEGER* serial = BN_to_ASN1_INTEGER(bn, nullptr);
        BN_free(bn);
        if (!serial) {
            X509_free(cert);
            EVP_PKEY_free(pkey);
            throw std::runtime_error("BN_to_ASN1_INTEGER failed");
        }
        X509_set_serialNumber(cert, serial);
        ASN1_INTEGER_free(serial);

        X509_gmtime_adj(X509_getm_notBefore(cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(cert), static_cast<long>(days) * 86400L);

        X509_NAME* subj = X509_get_subject_name(cert);
        X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(cn.c_str()), -1, -1, 0);
        X509_set_issuer_name(cert, subj);

        X509_set_pubkey(cert, pkey);

        // SANs
        if (!sans.empty()) {
            std::string san_str;
            for (size_t i = 0; i < sans.size(); ++i) {
                if (i) san_str += ',';
                san_str += (looks_like_ip(sans[i]) ? "IP:" : "DNS:");
                san_str += sans[i];
            }
            X509V3_CTX ctx;
            X509V3_set_ctx_nodb(&ctx);
            X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
            X509_EXTENSION* ext = X509V3_EXT_conf_nid(
                nullptr, &ctx, NID_subject_alt_name, san_str.c_str());
            if (ext) {
                X509_add_ext(cert, ext, -1);
                X509_EXTENSION_free(ext);
            }
        }

        if (X509_sign(cert, pkey, EVP_sha256()) == 0) {
            X509_free(cert);
            EVP_PKEY_free(pkey);
            throw std::runtime_error("X509_sign failed");
        }

        // -- Serialize to PEM strings --
        auto pem_to_string = [](auto writer_fn) -> std::string {
            BIO* bio = BIO_new(BIO_s_mem());
            if (!bio) throw std::runtime_error("BIO_new failed");
            try { writer_fn(bio); } catch (...) { BIO_free(bio); throw; }
            BUF_MEM* bptr = nullptr;
            BIO_get_mem_ptr(bio, &bptr);
            std::string result(bptr->data, bptr->length);
            BIO_free(bio);
            return result;
        };

        std::string cert_pem = pem_to_string([&](BIO* bio) {
            if (PEM_write_bio_X509(bio, cert) != 1)
                throw std::runtime_error("PEM_write_bio_X509 failed");
        });

        std::string key_pem = pem_to_string([&](BIO* bio) {
            if (PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1)
                throw std::runtime_error("PEM_write_bio_PrivateKey failed");
        });

        // -- Write to disk atomically --
        std::error_code fserr;
        fs::create_directories(cert_dir_, fserr);

        std::string cert_name = sanitize_cert_name(cn);
        auto cert_path = (fs::path(cert_dir_) / (cert_name + ".crt")).string();
        auto key_path  = (fs::path(cert_dir_) / (cert_name + ".key")).string();

        omb::atomic_write_file(cert_path, cert_pem);
        try {
            write_private_key_secure(key_path, key_pem);
        } catch (...) {
            std::error_code ec2;
            fs::remove(cert_path, ec2);
            X509_free(cert);
            EVP_PKEY_free(pkey);
            throw;
        }

        X509_free(cert);
        EVP_PKEY_free(pkey);

        // -- Mark done --
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            auto it = jobs_.find(job_id);
            if (it != jobs_.end()) {
                it->second.status    = GenJob::Status::done;
                it->second.cert_name = cert_name;
            }
        }
    } catch (const std::exception& e) {
        set_error(e.what());
    } catch (...) {
        set_error("Unknown error during certificate generation");
    }
}

}
