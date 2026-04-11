# Technical Specification
## Ollama MCP Bridge — Unified Binary + Web Management Frontend + Documentation

---

## 1. Technical Context

### Language & Standard
- **C++17** throughout (`std::filesystem`, `std::optional`, `std::shared_mutex`, structured bindings)

### Existing Dependencies (via CMake FetchContent)
| Library | Version | Role |
|---------|---------|------|
| `nlohmann/json` | v3.11.3 | JSON serialization/deserialization |
| `yhirose/cpp-httplib` | v0.15.3 | HTTP/HTTPS server & client |
| `CLIUtils/CLI11` | v2.4.1 | CLI argument parsing |
| `gabime/spdlog` | v1.13.0 | Structured logging |
| `Threads` (CMake) | system | `std::thread`, `std::mutex`, etc. |

### New Dependencies
| Library | Source | Role |
|---------|--------|------|
| **OpenSSL** (`libssl-dev`) | System package | TLS for web management server; enables `httplib::SSLServer` |

### Build System
CMake 3.14+. The web frontend assets (single HTML file containing all CSS and JS) are embedded into a generated C++ header at configure time using `file(READ ...)` and `file(WRITE ...)` with direct CMake variable interpolation — no external tools (`xxd`, `bin2c`) required. `configure_file` is **not** used for the HTML asset because `@ONLY` still substitutes any `@IDENTIFIER@` tokens, which collides with common CSS at-rules (`@media`, `@charset`, `@keyframes`).

---

## 2. Architecture Overview

```
Process: ollama-mcp-bridge
│
├── Thread: Web Management Server  (port 11464, httplib::Server or httplib::SSLServer)
│   ├── Static SPA: GET /  →  embedded index.html
│   ├── Management API: /api/*  →  WebApiHandler
│   └── Reads/writes: AppState, LogBuffer, ConfigManager, TlsManager
│
├── Thread: Proxy API Server  (port 8000, httplib::Server)
│   ├── POST /api/chat  →  ProxyService
│   ├── GET /health, /version
│   └── * (proxy handler)  →  ProxyService
│
├── MCPManager          (shared, mutex-protected)
├── AppState            (shared atomic state, mutex-protected)
├── LogBuffer           (ring buffer, last 500 lines, mutex-protected)
├── ConfigManager       (live config + bridge-state.json persistence)
└── TlsManager          (cert directory management, self-signed generation)
```

The proxy server runs in its own thread (blocking on `httplib::Server::listen`). The web server runs in its own thread. Both share `AppState`, `MCPManager`, `LogBuffer`, and `ConfigManager` via `std::shared_ptr` with appropriate locking.

The proxy server thread can be **stopped and restarted** independently via `AppState::proxy_running` flag while the web server thread stays alive — satisfying FR-2.4 / A-3.

---

## 3. Source Code Structure Changes

### New Files

| File | Purpose |
|------|---------|
| `cpp/app_state.h` | Shared mutable runtime state (proxy running, log buffer, etc.) |
| `cpp/config_manager.h` / `.cpp` | Manages live runtime config + `bridge-state.json` atomic writes |
| `cpp/log_buffer.h` / `.cpp` | Thread-safe ring buffer of last 500 log lines; custom spdlog sink |
| `cpp/tls_manager.h` / `.cpp` | Cert directory scanner, self-signed generation (via OpenSSL API), upload/delete |
| `cpp/web_server.h` / `.cpp` | Web management HTTP(S) server: static SPA + management API routes |
| `cpp/web_ui.h` | **Generated** at configure time — embeds SPA HTML as `const char* WEB_UI_HTML` |
| `web/index.html` | Source SPA (HTML + `<style>` + `<script>` — single file, no bundler) |

### Modified Files

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add OpenSSL `find_package`, flip `HTTPLIB_REQUIRE_OPENSSL ON`, add `file(READ/WRITE)` asset embedding step for `web_ui.h`, add new `.cpp` sources to the target |
| `cpp/main.cpp` | Add new CLI flags, start `WebServer` thread alongside `Server`, implement `--no-web-ui`, graceful dual-thread shutdown |
| `cpp/server.h` / `.cpp` | Accept `std::shared_ptr<AppState>`, accept `std::shared_ptr<LogBuffer>`; change `start()` to allow stop-without-exit; change Ollama-unavailable behaviour (return 503 instead of exit); add `--fail-on-ollama-unavailable` path |
| `cpp/mcp_manager.h` / `.cpp` | Add `add_server()`, `remove_server()`, `reconnect_server()`, `reconnect_all()`, `get_server_status()` public methods for management API; store per-server config JSON so it can be serialised back to `mcp-config.json` |
| `cpp/utils.h` / `.cpp` | Add `atomic_write_file()`, `parse_bool_env()`, `url_encode()` helpers |
| `README.md` | Full rewrite per FR-4.1 |

---

## 4. Detailed Design

### 4.1 `AppState` (`cpp/app_state.h`)

Holds all shared mutable runtime state. **Two categories of fields**:

- **Atomic fields** (`std::atomic<bool>`): safe to read/write without holding `mutex_`
- **String/int/optional fields**: must be read or written while holding `mutex_` (shared or exclusive as appropriate)

```cpp
struct AppState {
    // --- Atomic (no lock needed) ---
    std::atomic<bool> proxy_running{false};
    std::atomic<bool> ollama_reachable{false};
    std::atomic<bool> shutdown_requested{false};  // set by signal pipe reader
    std::atomic<bool> fail_on_ollama_unavailable{false};

    // --- Write-once (set in constructor, never modified — no lock needed) ---
    const std::chrono::steady_clock::time_point start_time{std::chrono::steady_clock::now()};

    // --- Mutex-protected (acquire mutex_ before access) ---
    mutable std::mutex mutex_;

    // Live-updateable config fields (no restart needed)
    std::string ollama_url;
    std::optional<int> max_tool_rounds;
    std::optional<std::string> system_prompt;
    std::string cors_origins;

    // Web server config (restart required to change)
    std::string web_host;
    int web_port{11464};
    bool web_tls{false};

    // Proxy config (restart required to change)
    std::string proxy_host;
    int proxy_port{8000};
    std::string config_file;

    // TLS state
    std::string active_cert_name;   // filename stem in cert_dir
    std::string cert_dir;

    // Proxy restart management (see §6)
    std::optional<std::thread> proxy_thread;
};
```

Shared as `std::shared_ptr<AppState>` between `Server`, `WebServer`, and `main`.

### 4.2 `LogBuffer` (`cpp/log_buffer.h` / `.cpp`)

- Ring buffer of `std::deque<std::string>` capped at 500 entries
- Custom `spdlog::sinks::base_sink<std::mutex>` subclass: every log message formatted as a string is pushed into the ring buffer
- `get_lines(int n)` returns the last N lines as `std::vector<std::string>`
- Added to spdlog's global logger alongside the existing console sink at startup
- **Locking**: `spdlog::sinks::base_sink<std::mutex>` owns an internal `std::mutex` (member `mutex_`) that it locks before calling `sink_it_()`. `get_lines()` is deliberately **non-const** (since `spdlog`'s `mutex_` is not `mutable`) and must lock the **same** inherited mutex via `std::lock_guard<std::mutex> lock(this->mutex_)` to provide mutual exclusion with writes. No additional mutex is needed.

```cpp
class LogBuffer : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit LogBuffer(size_t max_lines = 500);
    std::vector<std::string> get_lines(int n);
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override {}
private:
    size_t max_lines_;
    std::deque<std::string> lines_;
};
```

### 4.3 `ConfigManager` (`cpp/config_manager.h` / `.cpp`)

Responsibilities:
- Load `mcp-config.json` on startup; hold the raw JSON for round-tripping
- Persist changes to `mcp-config.json` via `atomic_write_file()` (write to `.tmp` + rename)
- Load `bridge-state.json` on startup (create if missing); persist active TLS cert name
- All writes use `atomic_write_file()` from `utils.cpp`:
  ```cpp
  void atomic_write_file(const std::string& path, const std::string& content);
  // Writes content to path + ".tmp", then rename()s over path
  ```

Key methods:
```cpp
class ConfigManager {
public:
    explicit ConfigManager(const std::string& config_file_path);
    json get_mcp_config() const;
    void save_mcp_config(const json& cfg);
    std::string get_active_cert() const;
    void set_active_cert(const std::string& name);
    std::string config_dir() const;
    std::string state_file_path() const;  // <config_dir>/bridge-state.json
private:
    std::string config_path_;
    json mcp_config_;
    json bridge_state_;
    mutable std::mutex mutex_;
};
```

### 4.4 `TlsManager` (`cpp/tls_manager.h` / `.cpp`)

Responsibilities:
- Scan `cert_dir` for `.pem` / `.crt` files paired with matching `.key` files
- Generate self-signed certificates asynchronously (using OpenSSL C API: `EVP_PKEY_keygen`, `X509_*`)
- Upload cert+key pair (validate PEM, write to cert_dir with `chmod 600`)
- Delete a cert+key pair (guard: cannot delete active cert when TLS is enabled)

```cpp
struct CertInfo {
    std::string name;         // stem, e.g. "my-server"
    std::string cert_file;    // full path
    std::string key_file;     // full path
    std::string common_name;
    std::vector<std::string> sans;
    std::chrono::system_clock::time_point not_after;
    bool is_active{false};
    bool is_expiring_soon{false};  // within 30 days
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
    ~TlsManager();  // joins all active gen_threads_ before returning
    std::vector<CertInfo> list_certs(const std::string& active_cert_name) const;
    // Returns job_id immediately (HTTP 202). HTTP 429 if >= 20 active jobs.
    std::string start_generate_job(const std::string& cn, int days,
                                   int key_size, const std::vector<std::string>& sans);
    std::optional<GenJob> get_job(const std::string& job_id) const;
    void upload_cert(const std::string& name,
                     const std::string& cert_pem, const std::string& key_pem);
    void delete_cert(const std::string& name, bool tls_enabled, const std::string& active_cert);
    void activate_cert(const std::string& name);
private:
    std::string cert_dir_;
    std::shared_ptr<ConfigManager> cfg_mgr_;  // shared_ptr — no dangling risk
    mutable std::mutex jobs_mutex_;
    std::map<std::string, GenJob> jobs_;  // max 20
    std::vector<std::thread> gen_threads_;  // joinable cert generation threads
    void prune_expired_jobs();            // jobs older than 5 min removed; also prunes finished gen_threads_
};
```

Self-signed cert generation runs in **joinable** `std::thread` instances stored in a `std::vector<std::thread> gen_threads_` member. Job result stored in `jobs_` map, accessed via `get_job()`. `TlsManager`'s destructor joins all active generation threads before returning — this ensures no thread is mid-write to the cert directory when the process exits. Completed threads are pruned from `gen_threads_` during `prune_expired_jobs()`.

### 4.5 `WebServer` (`cpp/web_server.h` / `.cpp`)

Owns a `httplib::Server` (or `httplib::SSLServer` when `--web-tls` is set).  
Runs in its own thread via `WebServer::start_async()` which calls `std::thread([this]{ svr_.listen(...); })`.

Constructor parameters:
```cpp
WebServer(
    std::shared_ptr<AppState> state,
    std::shared_ptr<LogBuffer> log_buf,
    std::shared_ptr<ConfigManager> cfg_mgr,
    std::shared_ptr<MCPManager> mcp_mgr,
    std::shared_ptr<TlsManager> tls_mgr,
    std::shared_ptr<Server> proxy_server  // shared_ptr — safe across restart cycles
);
```

Routes registered in `WebServer::setup_routes()`:

**Static SPA**
```
GET /          →  res.set_content(WEB_UI_HTML, "text/html")
GET /favicon.ico  →  404 (favicon is inline data: URI in HTML)
```

**Management API** (all under `/api/`):
```
GET  /api/status
GET  /api/config
POST /api/config
GET  /api/mcp-servers
POST /api/mcp-servers
PUT  /api/mcp-servers/:name
DEL  /api/mcp-servers/:name
POST /api/mcp-servers/:name/reconnect
GET  /api/tools
POST /api/proxy/start
POST /api/proxy/stop
GET  /api/proxy/status
GET  /api/logs?n=200
POST /api/ollama/test
GET  /api/tls/certificates
POST /api/tls/generate-self-signed
GET  /api/tls/jobs/:job_id
POST /api/tls/upload          (multipart/form-data: "cert" + "key" fields)
DEL  /api/tls/certificates/:name
POST /api/tls/activate/:name
```

All responses: `Content-Type: application/json`.  
Error shape: `{"error": "<message>"}`.

`POST /api/proxy/stop` calls `Server::stop()` (unblocks the proxy thread's `listen()`) and then joins `AppState::proxy_thread` before returning HTTP 200 — ensuring the old thread is fully retired before any restart. `POST /api/proxy/start` may only be called after the thread has been joined; it calls `Server::start_async()` which stores a new `std::thread` in `AppState::proxy_thread`. See §6 for the full synchronisation protocol.

### 4.6 Modified `Server` (`cpp/server.h` / `.cpp`)

New methods:
```cpp
void start_async();   // starts server in a background std::thread; returns immediately
// void start() remains (blocking, used by main for single-thread mode)

void reset();
// Re-creates the internal httplib::Server (or SSLServer) instance from scratch.
// Required before calling start()/start_async() a second time because
// cpp-httplib's Server::stop() permanently disables the existing instance —
// it cannot be restarted; only a fresh object can listen again.
// Called by POST /api/proxy/start after a prior stop+join cycle.
```

Constructor takes `std::shared_ptr<AppState>` and `std::shared_ptr<LogBuffer>`.

**Restart semantics**: `Server` now uses a **shared** `MCPManager` (passed as `std::shared_ptr<MCPManager>` from `main`), rather than owning it internally. This means:
- The management API's `add_server()` / `remove_server()` mutations on `MCPManager` survive proxy restarts.
- All management API mutations **must** persist via `ConfigManager::save_mcp_config()` immediately — `mcp-config.json` on disk is the single source of truth. On initial startup, `main()` calls `MCPManager::load_servers()` once from disk.
- `stop()` destroys only `ProxyService` and calls `httplib::Server::stop()`. It does **not** destroy or reset `MCPManager`.

**Route registration responsibility** — `setup_routes()` is called in exactly one place per lifecycle:
- **Initial blocking start** (`start()`): creates `ProxyService`, calls `setup_routes()`, then calls `svr.listen()` (blocking). Used by `main()` for the initial launch.
- **Restart via management API**: `reset()` re-creates the `httplib::Server` instance (the `Impl` struct), creates `ProxyService`, and calls `setup_routes()`. After `reset()` returns, `start_async()` spawns a background thread that only calls `svr.listen()` — it does **not** call `setup_routes()` or create `ProxyService` again.
- **Initial async start** (`start_async()` without prior `reset()`): creates `ProxyService`, calls `setup_routes()` synchronously, then spawns the `svr.listen()` thread. Used if the proxy is started asynchronously at process startup.

This ensures routes are never double-registered. `start_async()` checks whether `ProxyService` already exists (set by `reset()`) and skips initialization if so.

Behaviour change — Ollama unreachable at startup:
- If `AppState::fail_on_ollama_unavailable == false` (default): log a warning, set `AppState::ollama_reachable = false`, and continue. `/api/chat` returns HTTP 503 while `ollama_reachable == false`.
- If `AppState::fail_on_ollama_unavailable == true` (`--fail-on-ollama-unavailable`): exit with code 1 (old behaviour).

Ollama reachability is re-checked on every `GET /api/status` poll and on the periodic background health check (every 30 s via `std::thread` + `std::this_thread::sleep_for`).

### 4.7 Modified `MCPManager` (`cpp/mcp_manager.h` / `.cpp`)

Store per-server config JSON keyed by name so the management API can write back to `mcp-config.json`:
```cpp
std::map<std::string, json> server_configs_;  // name → raw config JSON
```

New public API:
```cpp
// Returns per-server status snapshot
struct ServerStatus {
    std::string name;
    std::string transport_type;   // "stdio" | "sse" | "http"
    bool connected;
    size_t tool_count;
    std::string last_error;
};
std::vector<ServerStatus> get_server_statuses() const;

// Mutating operations (take unique_lock)
void add_server(const std::string& name, const json& config);
void update_server(const std::string& name, const json& config);
void remove_server(const std::string& name);
void reconnect_server(const std::string& name);
void reconnect_all();
```

`add_server` / `update_server` / `remove_server` rebuild `all_tools_` and `all_tools_json_` after the mutation.

### 4.8 Asset Embedding via CMake

**Problem**: `configure_file(@ONLY)` still replaces `@IDENTIFIER@` tokens, which collides with CSS at-rules (`@media`, `@charset`, `@keyframes`, `@import`). `configure_file` is therefore **not used** for the HTML content.

**Chosen approach**: use CMake's `file(READ ...)` to load the HTML into a CMake variable, then build the C++ header as a CMake string using `set()` with standard `${VAR}` interpolation, and write it with `file(WRITE ...)`. This avoids both `configure_file` and `string(CONFIGURE)` — neither is used. CMake's `${WEB_UI_HTML_CONTENT}` interpolation inserts the HTML verbatim into the raw string literal without any `@...@` token scanning, so CSS at-rules are safe.

`CMakeLists.txt` configure-time code:
```cmake
# Read HTML source into a CMake variable
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" WEB_UI_HTML_CONTENT)

# Guard: the raw string delimiter )WEBUI" must not appear in the HTML
string(FIND "${WEB_UI_HTML_CONTENT}" ")WEBUI\"" _DELIM_POS)
if(NOT _DELIM_POS EQUAL -1)
    message(FATAL_ERROR
        "web/index.html contains the raw-string delimiter sequence )WEBUI\". "
        "Choose a different delimiter in CMakeLists.txt and cpp/web_ui.h.in.")
endif()

# Build the header content as a CMake string, then write it
set(_WEB_UI_HEADER
"#pragma once
// Auto-generated by CMake — do not edit manually
namespace omb {
inline const char* web_ui_html() {
    static const char html[] = R\"WEBUI(${WEB_UI_HTML_CONTENT})WEBUI\";
    return html;
}
}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/web_ui.h" "${_WEB_UI_HEADER}")

target_include_directories(ollama-mcp-bridge PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
```

Note: `file(WRITE ...)` is used (not `configure_file`) so CMake does not attempt a second pass of `@...@` substitution on the already-substituted content. The delimiter collision check is **mandatory** — the build **must** fail with a `FATAL_ERROR` if the sequence is found.

### 4.9 OpenSSL Integration in CMakeLists.txt

```cmake
# Replace existing HTTPLIB_REQUIRE_OPENSSL OFF lines:
find_package(OpenSSL REQUIRED)
set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE ON CACHE BOOL "" FORCE)

# Add to target_link_libraries:
target_link_libraries(ollama-mcp-bridge PRIVATE OpenSSL::SSL OpenSSL::Crypto)
```

If `find_package(OpenSSL REQUIRED)` fails, CMake exits with an error. The Debian 12 guide in README.md instructs the user to run `sudo apt install libssl-dev` first.

### 4.10 New CLI Flags (`cpp/main.cpp`)

Added to the `CLI::App` instance:

| Flag | Env Var | Default | Notes |
|------|---------|---------|-------|
| `--web-port <N>` | `WEB_PORT` | `11464` | Web UI listen port |
| `--web-host <addr>` | `WEB_HOST` | `0.0.0.0` | Web UI bind address |
| `--no-web-ui` | `WEB_UI=0` | (off) | Disable web UI entirely |
| `--web-tls` | `WEB_TLS=1` | (off) | Enable HTTPS for web UI |
| `--web-cert <path>` | `WEB_CERT` | — | PEM cert file |
| `--web-key <path>` | `WEB_KEY` | — | PEM key file |
| `--web-cert-dir <path>` | `WEB_CERT_DIR` | `<config_dir>/certs/` | Cert storage directory |
| `--fail-on-ollama-unavailable` | `FAIL_ON_OLLAMA_UNAVAILABLE=true` | (off) | Old exit-on-failure behaviour |

`WEB_UI` env var: falsy values `0`, `false`, `no`, `off` (case-insensitive) disable the UI; parsed by `parse_bool_env()` helper in `utils.cpp`.

### 4.11 `web/index.html` — SPA Design

Single HTML file. No build toolchain (no npm, webpack, etc.).

Structure:
```
<html>
  <head>
    <meta charset="UTF-8">
    <title>Ollama MCP Bridge</title>
    <link rel="icon" href="data:image/svg+xml,...">   <!-- inline SVG favicon -->
    <style>  /* all CSS inline */  </style>
  </head>
  <body>
    <nav> <!-- left sidebar: Dashboard | MCP Servers | Tools | Configuration | Logs --> </nav>
    <main id="app">  <!-- section containers, shown/hidden by JS --> </main>
    <script>  /* all JS inline — vanilla, no frameworks */  </script>
  </body>
</html>
```

Navigation: clicking a nav link calls `showSection(name)` which hides all sections and shows the target one.

API calls use `fetch()` with `async/await`. Status polling uses `setInterval`.

Tooltip implementation: every `<button>`, `<input>`, `<select>`, `<th>` has a `title="..."` attribute. Additionally a CSS `.tooltip` class with a `::after` pseudo-element provides styled hover tooltips for longer descriptions.

Favicon: 32×32 SVG inline data URI — a simplified bridge icon using `#2563eb` (blue) on transparent background.

---

## 5. Data Models & API Shapes

### `GET /api/status` response
```json
{
  "proxy_running": true,
  "proxy_port": 8000,
  "proxy_host": "0.0.0.0",
  "ollama_url": "http://localhost:11434",
  "ollama_reachable": true,
  "mcp_servers_total": 2,
  "mcp_servers_connected": 2,
  "tools_total": 14,
  "web_port": 11464,
  "web_tls": false,
  "uptime_seconds": 3720,
  "version": "0.1.0"
}
```

### `GET /api/config` response
```json
{
  "ollama_url": "http://localhost:11434",
  "proxy_port": 8000,
  "proxy_host": "0.0.0.0",
  "web_port": 11464,
  "web_host": "0.0.0.0",
  "web_tls": false,
  "max_tool_rounds": 0,
  "system_prompt": "",
  "cors_origins": "*",
  "config_file": "/etc/ollama-mcp-bridge/mcp-config.json",
  "web_tls_active_cert": "my-server"
}
```

### `POST /api/config` request body (partial update supported)
```json
{
  "ollama_url": "http://192.168.1.10:11434",
  "max_tool_rounds": 5,
  "system_prompt": "You are a helpful assistant."
}
```

Fields that require restart: `proxy_port`, `proxy_host`, `web_port`, `web_host`, `web_tls`. Response includes `"requires_restart": true` if any such field was changed.

**Unknown fields**: Any field name not in the set of recognised config fields is silently ignored — this allows forward compatibility with future config keys. No HTTP 400 for unknown fields.

### `GET /api/mcp-servers` response
```json
{
  "servers": [
    {
      "name": "weather",
      "transport": "stdio",
      "config": { "command": "python", "args": ["-m", "weather_mcp"] },
      "connected": true,
      "tool_count": 3,
      "last_error": null
    }
  ]
}
```

### `POST /api/mcp-servers` / `PUT /api/mcp-servers/:name` request body

An explicit **`"transport"`** field is **required** in every request. Valid values: `"stdio"`, `"http"`, `"sse"`. The server returns HTTP 400 if the field is missing or has an unrecognised value. This removes any inference ambiguity (e.g. a config that has both `command` and `url` fields).

`stdio` transport:
```json
{
  "name": "my-server",
  "transport": "stdio",
  "command": "npx",
  "args": ["-y", "@example/mcp-server"],
  "env": { "API_KEY": "abc" },
  "toolFilter": { "mode": "include", "tools": ["search", "fetch"] }
}
```

`http` or `sse` transport:
```json
{
  "name": "remote-server",
  "transport": "http",
  "url": "http://localhost:3001/mcp",
  "headers": { "Authorization": "Bearer token" }
}
```

The `transport` value is stored in `bridge-state.json` alongside the server config and is returned verbatim in `GET /api/mcp-servers`. When writing to `mcp-config.json` via the management API, `transport` is stored as a field in the server's config object so the file remains self-describing; `MCPManager::connect_server()` continues to infer the transport from `command` vs `url` for configs loaded from pre-existing `mcp-config.json` files without a `transport` key (backward compatibility).

Server name validation regex: `^[a-zA-Z0-9._-]+$`. HTTP 400 if invalid.

### `GET /api/tools` response
```json
{
  "tools": [
    {
      "name": "weather.get_forecast",
      "server": "weather",
      "description": "Get weather forecast for a location"
    }
  ]
}
```

### `GET /api/proxy/status` response
```json
{
  "running": true,
  "host": "0.0.0.0",
  "port": 8000,
  "ollama_url": "http://localhost:11434",
  "ollama_reachable": true,
  "uptime_seconds": 3720
}
```
When proxy is stopped: `"running": false`, other fields reflect last-known config, `"uptime_seconds": 0`.

### `POST /api/ollama/test` request / response
Request: `{"url": "http://192.168.1.10:11434"}`  
Response: `{"reachable": true, "latency_ms": 42}` or `{"reachable": false, "error": "connection refused"}`

### `GET /api/logs` response
```json
{ "lines": ["[2025-01-01 12:00:00.000] [info] Started", "..."], "total": 142 }
```

### `GET /api/tls/certificates` response
```json
{
  "certificates": [
    {
      "name": "my-server",
      "cert_file": "/etc/.../certs/my-server.crt",
      "common_name": "localhost",
      "sans": ["127.0.0.1", "localhost"],
      "not_after": "2026-01-01T00:00:00Z",
      "is_active": true,
      "is_expiring_soon": false
    }
  ]
}
```

### `POST /api/tls/generate-self-signed` request
```json
{ "cn": "localhost", "days": 365, "key_size": 2048, "sans": ["127.0.0.1", "::1"] }
```
Response: `{"job_id": "a1b2c3d4"}` (immediate, HTTP 202).  
HTTP 429 if there are already ≥ 20 active (running) jobs — rejected before any work starts.

### `GET /api/tls/jobs/:job_id` response
```json
{ "status": "done", "cert_name": "localhost-20260101", "error": null }
```
or `{ "status": "running" }` or `{ "status": "error", "error": "key generation failed: ..." }`

HTTP 404 if job not found or expired (jobs expire 5 minutes after completion/failure).

---

## 6. Thread Model & Shutdown

### Static layout

```
main()
├── shared_ptr<AppState>
├── shared_ptr<LogBuffer>
├── shared_ptr<ConfigManager>
├── shared_ptr<MCPManager>
├── shared_ptr<TlsManager>
│
├── shared_ptr<Server> proxy_server(state, log_buf, cfg_mgr, mcp_mgr)
│   └── AppState::proxy_thread = std::thread{ proxy_server->start() }
│
└── WebServer web_server(state, log_buf, cfg_mgr, mcp_mgr, tls_mgr, proxy_server)
    └── web_thread = std::thread{ web_server.start() }
```

### Signal handling — async-signal-safe design

Calling arbitrary C++ methods (including those that acquire mutexes) from a POSIX signal handler is undefined behaviour. The shutdown sequence uses a **self-pipe** pattern:

```cpp
// In main(), before signal registration:
int shutdown_pipe[2];
pipe(shutdown_pipe);  // shutdown_pipe[0]=read end, [1]=write end

// Signal handler (only async-signal-safe operations):
static void signal_handler(int) {
    char byte = 1;
    write(shutdown_pipe_write_fd, &byte, 1);  // write() is async-signal-safe
}
std::signal(SIGINT,  signal_handler);
std::signal(SIGTERM, signal_handler);

// Main thread: block on read() from pipe
// (or poll with select/epoll alongside other work)
char buf;
read(shutdown_pipe[0], &buf, 1);   // unblocks when signal fires

// Now in main thread context — safe to call anything:
web_server.stop();   // httplib::Server::stop()
if (state->proxy_thread && state->proxy_thread->joinable()) {
    proxy_server->stop();
    state->proxy_thread->join();
}
web_thread.join();
```

`AppState::shutdown_requested` (`std::atomic<bool>`) is set to `true` immediately after the pipe write is consumed, so any polling code in other threads can check it.

### Proxy stop/restart synchronisation (management API)

`POST /api/proxy/stop` handler (runs in web server's thread pool):
```
1. proxy_server->stop()            // signals httplib to stop listening
2. {lock state->mutex_}
     old_thread = std::move(state->proxy_thread)
     state->proxy_thread = std::nullopt
   {unlock}
3. if (old_thread && old_thread->joinable()) old_thread->join()
                                   // blocks until proxy thread exits
4. state->proxy_running = false
5. return HTTP 200
```

`POST /api/proxy/start` handler:
```
1. bool expected = false
   if (!state->proxy_running.compare_exchange_strong(expected, true))
       return HTTP 409 "Proxy already running"
   // proxy_running is now atomically true — concurrent callers see it immediately,
   // closing the TOCTOU window between check and set.
2. proxy_server->reset()           // reinitialise httplib::Server internal state
3. new_thread = std::thread{ [proxy_server]{ proxy_server->start(); } }
4. {lock state->mutex_}
     state->proxy_thread = std::move(new_thread)
   {unlock}
5. return HTTP 200
   // proxy_running remains true (set in step 1); do NOT set it again here.
```

`Server::reset()` is a new method that re-creates the internal `httplib::Server` instance (the existing one is consumed after `stop()`). This is necessary because `cpp-httplib`'s `Server::stop()` is terminal on the `httplib::Server` object — a fresh instance is required to re-listen.

---

## 7. `mcp-config.json` Schema (unchanged structure, extended documentation)

```json
{
  "mcpServers": {
    "<server-name>": {
      "command": "string",        // stdio transport: executable
      "args": ["string"],         // stdio: arguments array
      "env": { "KEY": "value" },  // stdio: environment overrides
      "url": "string",            // HTTP/SSE transport: endpoint URL
      "headers": { "K": "v" },   // HTTP/SSE: request headers
      "toolFilter": {
        "mode": "include|exclude",
        "tools": ["tool-name"]
      }
    }
  }
}
```

`bridge-state.json` (new, managed by `ConfigManager`):
```json
{
  "web_tls_active_cert": "my-server"
}
```

---

## 8. Security Notes (Implementation-Level)

- Key files written with `std::filesystem::permissions(..., std::filesystem::perms::owner_read | std::filesystem::perms::owner_write)` → equivalent to `chmod 600`
- On startup, if web UI is enabled and `--web-host` is `0.0.0.0`, log a warning: `"Web UI has no authentication. Ensure port 11464 is firewalled or use --web-host 127.0.0.1"`
- Management API does **not** set CORS headers (no `Access-Control-Allow-Origin`) — the existing CORS logic in `server.cpp` only applies to the proxy server. The embedded SPA uses relative URLs (e.g. `fetch("/api/status")`) so all requests are same-origin — no CORS is needed. Cross-origin access from external tools is intentionally unsupported.
- No authentication on the management API (future work per PRD non-goals)
- **Upload size limit**: The web management server sets `httplib::Server::set_payload_max_length(10 * 1024 * 1024)` (10 MB) to prevent memory exhaustion from oversized uploads on `POST /api/tls/upload` or other endpoints.

---

## 9. Verification Approach

Since the C++ codebase has no automated test suite, verification is done manually:

### Build Verification
```bash
sudo apt install libssl-dev cmake g++ git
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
# Expected: single binary ./ollama-mcp-bridge, zero warnings
```

### Smoke Tests
```bash
# Start with web UI enabled
./ollama-mcp-bridge --config ../mcp-config.json

# Web UI accessible
curl -s http://localhost:11464/ | grep -q "Ollama MCP Bridge"

# Management API
curl -s http://localhost:11464/api/status | python3 -m json.tool

# Stop/start proxy via API
curl -s -X POST http://localhost:11464/api/proxy/stop
curl -s http://localhost:11464/api/proxy/status
curl -s -X POST http://localhost:11464/api/proxy/start

# Ollama test
curl -s -X POST http://localhost:11464/api/ollama/test \
  -H 'Content-Type: application/json' \
  -d '{"url":"http://localhost:11434"}'

# TLS: generate self-signed cert and start in TLS mode
curl -s -X POST http://localhost:11464/api/tls/generate-self-signed \
  -H 'Content-Type: application/json' \
  -d '{"cn":"localhost","days":365,"key_size":2048,"sans":["127.0.0.1"]}'
# Poll job until done, then:
./ollama-mcp-bridge --web-tls --web-cert certs/localhost.crt --web-key certs/localhost.key
curl -k https://localhost:11464/api/status

# Disable web UI
./ollama-mcp-bridge --no-web-ui
curl http://localhost:11464/ # must fail (connection refused)
```

### CMake Build Failure Test
```bash
# Remove OpenSSL, confirm CMake fails clearly
sudo apt remove libssl-dev
cmake ..  # Expected: CMake error "Could not find OpenSSL..."
sudo apt install libssl-dev  # Restore
```
