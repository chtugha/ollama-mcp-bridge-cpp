# Full SDD workflow

## Configuration
- **Artifacts Path**: `.zenflow/tasks/new-task-25e7`

---

## Agent Instructions

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: b341a0a7-83c5-41c9-a2e7-ded90ca17a5f -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Focus on **what** the feature should do and **why**, not **how** it should be built. Do not include technical implementation details, technology choices, or code-level decisions — those belong in the Technical Specification.

Save the PRD to `.zenflow/tasks/new-task-25e7/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: 634e5966-ff53-4b4e-82f8-e9b4029b874a -->

Create a technical specification based on the PRD in `.zenflow/tasks/new-task-25e7/requirements.md`.

1. Review existing codebase architecture and identify reusable components
2. Define the implementation approach

Do not include implementation steps, phases, or task breakdowns — those belong in the Planning step.

Save to `.zenflow/tasks/new-task-25e7/spec.md` with:
- Technical context (language, dependencies)
- Implementation approach referencing existing code patterns
- Source code structure changes
- Data model / API / interface changes
- Verification approach using project lint/test commands

### [x] Step: Planning
<!-- chat-id: 1684531f-2015-4282-9601-f062fe54291e -->

Create a detailed implementation plan based on `.zenflow/tasks/new-task-25e7/spec.md`.

### [x] Step 1: CMakeLists.txt — OpenSSL + asset embedding + new sources
<!-- chat-id: 763372c0-4aad-432c-ac98-99f6675a5dfd -->

Update `CMakeLists.txt` to:
- Replace `HTTPLIB_REQUIRE_OPENSSL OFF` with `HTTPLIB_REQUIRE_OPENSSL ON` and remove the `USE_OPENSSL_IF_AVAILABLE OFF` line
- Add `find_package(OpenSSL REQUIRED)` after the Threads find (CMake exits with a clear error if missing)
- Add a `file(READ ...)` + `file(WRITE ...)` step (inside `cmake_language(DEFER ...)` or at configure time) that reads `web/index.html` and emits `cpp/web_ui.h` containing `const char* WEB_UI_HTML = R"WEBUI(...)WEBUI";` — use raw string literal to avoid escaping, and use `file(READ)` → CMake variable → `file(WRITE)` without `configure_file` so CSS at-rules are never substituted
- Add new source files to the target: `cpp/config_manager.cpp`, `cpp/log_buffer.cpp`, `cpp/tls_manager.cpp`, `cpp/web_server.cpp`
- Add `OpenSSL::SSL` and `OpenSSL::Crypto` to `target_link_libraries`
- Add `${CMAKE_CURRENT_BINARY_DIR}` (or `cpp/`) to `target_include_directories` so `web_ui.h` is found

Verify: `cmake ..` succeeds; `cmake --build .` produces the binary without OpenSSL errors.

### [x] Step 2: Shared infrastructure — AppState, LogBuffer, utils helpers
<!-- chat-id: c795701e-f904-411a-be6b-87bc6aac1c18 -->

Implement:
- **`cpp/app_state.h`** — `struct AppState` as specified in spec §4.1: atomic fields (`proxy_running`, `ollama_reachable`, `shutdown_requested`, `fail_on_ollama_unavailable`), write-once `start_time`, mutex-protected string/int fields for all config, TLS state, and `std::optional<std::thread> proxy_thread`
- **`cpp/log_buffer.h` / `cpp/log_buffer.cpp`** — `class LogBuffer : public spdlog::sinks::base_sink<std::mutex>` as specified in spec §4.2: ring buffer of `std::deque<std::string>` capped at 500 lines; `sink_it_()` pushes formatted messages; `get_lines(int n)` returns last N lines locking the inherited `this->mutex_`
- **`cpp/utils.h` / `cpp/utils.cpp`** additions (add to existing file, don't overwrite): `atomic_write_file(path, content)` (write to `path + ".tmp"` then `rename()`), `parse_bool_env(val)` (returns false for `"0"`, `"false"`, `"no"`, `"off"` case-insensitive; true otherwise), `url_encode(s)` (percent-encode non-alphanumeric/safe chars)

Verify: changes compile without errors; `LogBuffer` can be constructed and accepts a log message.

### [x] Step 3: ConfigManager — config + bridge-state.json persistence
<!-- chat-id: 463c86e6-6ed8-463e-ace5-c9e0907f395a -->

Implement **`cpp/config_manager.h`** and **`cpp/config_manager.cpp`** as specified in spec §4.3:
- Constructor: `explicit ConfigManager(const std::string& config_file_path)` — loads `mcp-config.json` into `mcp_config_` JSON; loads or creates `bridge-state.json` into `bridge_state_`; derives `config_dir_` from the config file path using `std::filesystem::path`
- `get_mcp_config() const` — returns copy of `mcp_config_` under lock
- `save_mcp_config(const json& cfg)` — atomically writes new JSON to `config_path_` using `atomic_write_file()`; updates in-memory `mcp_config_`
- `get_active_cert() const` — reads `bridge_state_["web_tls_active_cert"]` under lock; returns empty string if missing
- `set_active_cert(const std::string& name)` — updates `bridge_state_` and atomically writes `bridge-state.json`
- `config_dir() const` — returns parent directory of `config_path_` as `std::filesystem::path`
- `state_file_path() const` — returns `std::filesystem::path` of `config_dir() / "bridge-state.json"` (use `operator/` not string concatenation for correct cross-platform path joining)
- All public methods acquire `mutex_` appropriately

Verify: construct with a valid path; `get_active_cert()` returns empty string on fresh state; `set_active_cert("foo")` persists to disk.

### [ ] Step 4: TlsManager — certificate scanning, self-signed generation, upload/delete

Implement **`cpp/tls_manager.h`** and **`cpp/tls_manager.cpp`** as specified in spec §4.4:
- `list_certs(active_cert_name)` — scans `cert_dir_` for `.pem`/`.crt` files paired with a matching `.key` file; uses OpenSSL `X509` API to read CN, SANs, `not_after`; sets `is_active` and `is_expiring_soon` (< 30 days)
- `start_generate_job(cn, days, key_size, sans)` — returns a `job_id` (8-char random hex) immediately (HTTP 202 semantics); spawns a `std::thread` stored in `gen_threads_` that uses OpenSSL `EVP_PKEY_keygen` + `X509_*` to create a self-signed cert; updates `jobs_` with result; returns HTTP 429 logic (caller must check) if `>= 20` active jobs exist before spawning
- `get_job(job_id)` — returns `std::optional<GenJob>`; HTTP 404 logic if not found or expired (> 5 min old done/error jobs)
- `upload_cert(name, cert_pem, key_pem)` — validates PEM parse with OpenSSL; writes `<cert_dir>/<name>.crt` and `<cert_dir>/<name>.key`; sets key file permissions to `owner_read | owner_write` (chmod 600)
- `delete_cert(name, tls_enabled, active_cert)` — refuses if `tls_enabled && name == active_cert`; deletes both files
- `activate_cert(name)` — calls `cfg_mgr_->set_active_cert(name)`
- Destructor joins all `gen_threads_` before returning
- `prune_expired_jobs()` — removes jobs older than 5 min from `jobs_`; joins and removes finished threads from `gen_threads_`; **call sites**: called at the top of `get_job()` (before the lookup) and at the top of `start_generate_job()` (before the 20-job limit check) — both hold `jobs_mutex_` so no additional locking is needed inside `prune_expired_jobs()`

Verify: `list_certs()` compiles and returns empty vector when cert_dir is empty or non-existent; generate job roundtrip works (start → poll → done).

### [ ] Step 5: Modify MCPManager — management API methods and per-server config storage

Modify **`cpp/mcp_manager.h`** and **`cpp/mcp_manager.cpp`** as specified in spec §4.7:
- Add `std::map<std::string, json> server_configs_` private field to store raw per-server JSON keyed by name
- Populate `server_configs_` in `load_servers()` / constructor by storing the raw config object for each server
- Add public `ServerStatus` struct:
  ```cpp
  struct ServerStatus {
      std::string name;
      std::string transport_type;  // "stdio" | "sse" | "http"
      bool connected;
      size_t tool_count;
      std::string last_error;
  };
  ```
- Add methods:
  - `std::vector<ServerStatus> get_all_server_status() const`
  - `void add_server(const std::string& name, const json& config)` — validates name regex `^[a-zA-Z0-9._-]+$`; throws if duplicate; adds to `server_configs_` and connects
  - `void remove_server(const std::string& name)` — disconnects and removes
  - `void update_server(const std::string& name, const json& config)` — remove + re-add
  - `void reconnect_server(const std::string& name)`
  - `void reconnect_all()`
  - `json get_server_config(const std::string& name) const` — returns raw JSON for a server (for round-tripping to `mcp-config.json`)
- All new methods must be thread-safe (use existing or new mutex as appropriate)

Verify: compile cleanly; `get_all_server_status()` returns empty vector when no servers configured.

### [ ] Step 6: Modify Server — shared_ptr<AppState>, start_async/reset, Ollama 503 behavior

Modify **`cpp/server.h`** and **`cpp/server.cpp`** as specified in spec §4.6:
- Change constructor to accept `std::shared_ptr<AppState>` and `std::shared_ptr<LogBuffer>` in addition to `ServerConfig`; accept `std::shared_ptr<MCPManager>` (passed in, not created internally) — `MCPManager` outlives proxy restarts
- Add `void start_async()` — creates `ProxyService`, calls `setup_routes()`, spawns background thread calling `svr.listen()`; stores thread in `AppState::proxy_thread`; sets `AppState::proxy_running = true`
- Add `void reset()` — re-creates the internal `httplib::Server` object (since `stop()` permanently disables it); resets `ProxyService` pointer
- `start_async()` checks if `ProxyService` already created (by `reset()`) and skips re-creation if so — ensures routes are never double-registered
- Change Ollama-unavailable at startup: if `AppState::fail_on_ollama_unavailable == false`, log warning and continue with `ollama_reachable = false`; if true, exit with code 1 (old behavior)
- In `/api/chat` handler, return HTTP 503 with JSON error body when `AppState::ollama_reachable == false`
- Add background thread (30s poll) that re-checks Ollama health and updates `AppState::ollama_reachable`
- Remove `CliInputs` / `ServerConfig` from this path: `Server` reads what it needs from `AppState` and `ConfigManager` instead (or keep `ServerConfig` but populate `AppState` from it in `main`)

Verify: compile cleanly; proxy can be started, stopped, and restarted via `start_async()` + `stop()` + `reset()` + `start_async()` cycle.

### [ ] Step 7: WebServer — management HTTP(S) server and all API routes

Implement **`cpp/web_server.h`** and **`cpp/web_server.cpp`** as specified in spec §4.5 and §5:

Constructor: `WebServer(shared_ptr<AppState>, shared_ptr<LogBuffer>, shared_ptr<ConfigManager>, shared_ptr<MCPManager>, shared_ptr<TlsManager>, shared_ptr<Server>)`

- **Server type**: `httplib::SSLServer` inherits from `httplib::Server` in cpp-httplib, so hold the server as `std::unique_ptr<httplib::Server> svr_`. In the constructor (or a `make_server()` factory), branch on `AppState::web_tls`: if false, `svr_ = std::make_unique<httplib::Server>()`; if true, `svr_ = std::make_unique<httplib::SSLServer>(cert_path, key_path)`. All subsequent calls (`svr_->Get(...)`, `svr_->listen(...)`, `svr_->stop()`, `svr_->set_payload_max_length(...)`) go through the base-class pointer — no `std::variant` or duplicated route registration needed. Guard the `SSLServer` construction with `#ifdef CPPHTTPLIB_OPENSSL_SUPPORT` and a `static_assert` to produce a clear compile error if OpenSSL is not linked.
- `setup_routes()` registers all routes listed in spec §4.5
- `start()` — blocking `svr_.listen(web_host, web_port)`
- `stop()` — calls `svr_.stop()`
- Set `svr_.set_payload_max_length(10 * 1024 * 1024)` (10 MB upload limit)

Implement all management API endpoints:
- `GET /api/status` → JSON from `AppState` + `MCPManager` + `LogBuffer`
- `GET /api/config` / `POST /api/config` — read/write `AppState` fields; `POST` returns `"requires_restart": true` if restart-required fields changed; unknown fields silently ignored
- `GET /api/mcp-servers` / `POST` / `PUT /:name` / `DELETE /:name` / `POST /:name/reconnect` — delegate to `MCPManager`; on mutation call `ConfigManager::save_mcp_config()` with updated JSON
- `GET /api/tools` — delegate to `MCPManager`
- `POST /api/proxy/start` / `POST /api/proxy/stop` / `GET /api/proxy/status` — use atomic `compare_exchange` start guard; call `Server::stop()` + join + `Server::reset()` + `Server::start_async()` sequence (spec §6 synchronisation protocol)
- `GET /api/logs?n=200` — `LogBuffer::get_lines(n)`
- `POST /api/ollama/test` — HTTP GET to provided URL, measure latency, return `{reachable, latency_ms}`
- `GET /api/tls/certificates` / `POST /api/tls/generate-self-signed` / `GET /api/tls/jobs/:job_id` / `POST /api/tls/upload` / `DELETE /api/tls/certificates/:name` / `POST /api/tls/activate/:name` — delegate to `TlsManager`
- `GET /` — serve `WEB_UI_HTML` with `Content-Type: text/html`

Verify: server starts and serves `GET /` returning the embedded HTML; `GET /api/status` returns valid JSON.

### [ ] Step 8: SPA — web/index.html (Dashboard, MCP Servers, Tools, Configuration, Logs)

Create **`web/index.html`** as specified in spec §4.11 and requirements §3.2.2:

Single file containing all HTML, CSS (in `<style>`), and JS (in `<script>`). No external CDN dependencies, no build toolchain.

Sections:
- **Dashboard**: status panel (proxy status, Ollama status + URL, MCP servers count, tools count, proxy port, web port); Start/Stop Proxy + Reconnect All buttons; auto-refresh every 5 s; prominent warning banner when Ollama unreachable
- **MCP Servers**: table (Name, Type, Status, Tool Count, Actions); Reconnect/Edit/Delete per row; Add Server button → inline form for stdio (command, args, env) or HTTP/SSE (url, headers) with transport selector, tool filter mode + list
- **Tools**: read-only table (Tool Name, Server, Description); filter/search bar
- **Configuration**: form fields for all settings (Ollama URL + Test Connection button, proxy port/host, max tool rounds, system prompt, CORS origins, config file path read-only); Save button; restart-required badges; TLS sub-section (Enable HTTPS toggle, cert dropdown with CN+expiry, Generate Self-Signed form with async polling, Upload cert form with HTTP-mode warning, Delete cert with active-cert guard, expiry warning badge)
- **Logs**: scrollable log viewer (last 500 lines); Clear Display button; color-coded by level (ERROR=red, WARN=yellow, DEBUG=grey); auto-refresh every 3 s

UI requirements:
- Every `<button>`, `<input>`, `<select>`, `<th>` has `title="..."` tooltip attribute
- Inline SVG favicon (`data:image/svg+xml`) in `<link rel="icon">`
- Page title: "Ollama MCP Bridge"
- Responsive layout ≥ 1024 px, left sidebar navigation
- CSS `.tooltip` class with `::after` pseudo-element for longer styled tooltips
- Vanilla JS, `fetch()` with `async/await`, `setInterval` for polling
- TLS job polling: `setInterval` every 2 s polls `GET /api/tls/jobs/:job_id` until `done` or `error`, then updates cert dropdown
- 4096-bit key warning shown inline in generate form

Verify: HTML is valid; page loads in browser; all sections render; Dashboard shows live status; API calls connect to backend.

### [ ] Step 9: Modify main.cpp — new CLI flags, dual-thread startup, self-pipe shutdown

Modify **`cpp/main.cpp`** as specified in spec §4.10 and §6:

- Add all new CLI flags to `CLI::App`: `--web-port`, `--web-host`, `--no-web-ui`, `--web-tls`, `--web-cert`, `--web-key`, `--web-cert-dir`, `--fail-on-ollama-unavailable`; read corresponding env vars using `parse_bool_env()` and `get_env()`
- Construct `AppState` (shared_ptr) with all config values from CLI
- Construct `LogBuffer` (shared_ptr) and add to spdlog as a second sink alongside the existing console sink
- Construct `ConfigManager` (shared_ptr) from `--config` path
- Construct `MCPManager` (shared_ptr) using config from `ConfigManager::get_mcp_config()`
- Construct `TlsManager` (shared_ptr) with cert dir derived from `--web-cert-dir` or default
- If `--web-tls` set but no cert resolvable → log error and exit
- Construct `Server` (shared_ptr) with `AppState`, `LogBuffer`, `ConfigManager`, `MCPManager`
- Start proxy: `Server::start_async()` stores thread in `AppState::proxy_thread`
- If `--no-web-ui` not set: construct `WebServer`, launch in `web_thread = std::thread{[&]{ web_server.start(); }}`
- Log startup banner: `"Web UI available at: http(s)://<web_host>:<web_port>"` and security warning if binding to `0.0.0.0`
- Replace old `std::signal` handler with **self-pipe** pattern (spec §6): create `shutdown_pipe[2]`, signal handler writes 1 byte, main thread blocks on `read(shutdown_pipe[0], ...)`, then calls `web_server.stop()` + proxy stop/join + `web_thread.join()`
- Remove old `g_server` global

Verify: `./ollama-mcp-bridge --help` shows all new flags; binary starts both proxy and web server threads; Ctrl-C shuts down cleanly.

### [ ] Step 10: README.md — comprehensive documentation

Write **`README.md`** covering all requirements from spec §3.4 / requirements §3.4:
- Project overview and architecture diagram (ASCII)
- Feature summary: unified binary, web management UI, MCP proxy, TLS support
- All CLI flags and environment variables (full table)
- **Debian 12 installation guide** (from scratch): `apt install` dependencies (build-essential, cmake, g++, git, libssl-dev, curl), clone repo, cmake build, optional systemd service unit file
- Ollama connection guide: how to set `--ollama-url`, how to test connection from CLI (`curl /api/ollama/test`) and from the web frontend Configuration page
- `mcp-config.json` format and examples
- Web frontend usage guide for each section
- TLS/HTTPS setup guide: generate self-signed cert via web UI, upload custom cert, enable HTTPS
- Security notes: firewall recommendation, `--web-host 127.0.0.1`, no auth warning
- `bridge-state.json` explanation
- API reference for both management API (port 11464) and proxy API (port 8000)

Verify: markdown renders correctly; all CLI flags mentioned in docs match implementation.

### [ ] Step 11: Build verification and smoke tests

Run the verification steps from spec §9:
- `cmake .. -DCMAKE_BUILD_TYPE=Release && cmake --build . --parallel` — must succeed with zero errors
- Confirm single binary produced
- Start binary, check `GET /` returns HTML, `GET /api/status` returns valid JSON
- Test proxy stop/start via management API
- Test `POST /api/ollama/test`
- Test `--no-web-ui` flag disables web server
- Test `--fail-on-ollama-unavailable` exits when Ollama is down
- Confirm `cmake ..` without `libssl-dev` produces a clear CMake error (document result)

Record pass/fail for each check inline in this step.
