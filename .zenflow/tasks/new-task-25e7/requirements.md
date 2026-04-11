# Product Requirements Document (PRD)
## Ollama MCP Bridge — Unified Binary + Web Management Frontend + Documentation

---

## 1. Background & Context

**Ollama MCP Bridge** is a proxy/API server written in C++ that sits in front of an Ollama instance and transparently injects tools from one or more MCP (Model Context Protocol) servers into every chat request. It is API-compatible with the Ollama REST API, so any Ollama client works without modification.

The C++ source lives in `cpp/`, is compiled with CMake, and produces a single binary (`ollama-mcp-bridge`). A parallel Python reference implementation lives in `src/ollama_mcp_bridge/` and is published to PyPI; it is **not** in scope for this task beyond serving as a reference for behaviour.

### Current state

| Aspect | Detail |
|--------|--------|
| Binary | Single C++ binary `ollama-mcp-bridge` built from CMake |
| API port | Configurable, default **8000** |
| Endpoints | `POST /api/chat`, `GET /health`, `GET /version`, all other paths proxied to Ollama |
| Configuration | `mcp-config.json` (MCP servers) + CLI flags / env-vars (runtime settings) |
| Management UI | None |
| Installation docs | Exists in `README.md` but targets generic systems, not Debian 12 specifically |

---

## 2. Goals

1. **Single binary**: The web management frontend must be served from the **same binary** as the API proxy — no separate process, no separate install step.
2. **Web management frontend** on a fixed port **11464** with full lifecycle control, configuration, and status display.
3. **Comprehensive README.md** that covers every feature, Debian 12 installation from scratch, and how to connect to Ollama both from CLI and from the web frontend.

---

## 3. Feature Requirements

### 3.1 Single Unified Binary

**FR-1.1** The compiled binary `ollama-mcp-bridge` shall serve both:
- The existing Ollama-compatible API proxy (port configurable, default 8000)
- The new web management frontend (port configurable, default **11464**)

These two HTTP servers run concurrently inside the same process (separate threads / ports).

**FR-1.2** All web-frontend assets (HTML, CSS, JavaScript) are embedded in the binary at compile time (as C++ string literals or via a CMake resource mechanism such as `xxd` / `cmake_path` / raw string headers). The build must enable OpenSSL support in `cpp-httplib` (`HTTPLIB_REQUIRE_OPENSSL ON`) so the web management server can operate in HTTPS mode. **OpenSSL is now a required build-time dependency** — this is a breaking change from the current `CMakeLists.txt` which disables OpenSSL. The Debian 12 installation guide (FR-4.1 §4) must include `libssl-dev` in the list of build dependencies (`apt install libssl-dev`). The build will fail with a clear CMake error if OpenSSL is missing.

**FR-1.3** The web management frontend network binding is configurable via:
- CLI flag `--web-port <N>` (default: 11464) / env var `WEB_PORT`
- CLI flag `--web-host <addr>` (default: `0.0.0.0`) / env var `WEB_HOST` — allows operators to bind to `127.0.0.1` only for local-only access

**FR-1.4** The web management frontend can be disabled entirely via:
- CLI flag `--no-web-ui`
- Environment variable `WEB_UI=0` (accepted falsy values: `0`, `false`, `no`, `off` — case-insensitive; any other value or absence of the variable leaves the UI enabled)

**FR-1.5** The web management frontend supports HTTPS (TLS) mode:
- CLI flag `--web-tls` / env var `WEB_TLS=1` switches the web server to HTTPS-only. When enabled, the listener is a TLS-only `httplib::SSLServer`. A plain HTTP request to the same port will fail with a TLS handshake error at the TCP layer — `httplib::SSLServer` cannot produce an HTTP 301 redirect because no HTTP parsing occurs before the handshake failure. No redirect mechanism is implemented. Instead, the correct URL scheme is prominently documented in the README and printed to the log at startup (e.g., `Web UI available at: https://<host>:11464`).
- CLI flag `--web-cert <path>` / env var `WEB_CERT` specifies the PEM certificate file to use at startup.
- CLI flag `--web-key <path>` / env var `WEB_KEY` specifies the PEM private key file to use at startup.
- If `--web-tls` is set but no certificate is resolvable (neither CLI flags nor a persisted active certificate), the binary logs a clear error and exits.
- Certificate files are stored in a configurable directory (`--web-cert-dir`, default: `<config-file directory>/certs/`; if no config file path is set, defaults to `./certs/` relative to the working directory). Multiple cert/key pairs may be stored there simultaneously.
- **Active certificate persistence**: The name of the active cert/key pair is stored as a field (`web_tls_active_cert`) in a separate runtime state file (`<config-file directory>/bridge-state.json`). This file is distinct from `mcp-config.json` (which is user-managed) so the bridge never overwrites user config. If `bridge-state.json` does not exist it is created on first write. All writes to `bridge-state.json` must be **atomic**: the new content is written to a temporary file in the same directory, then `rename()`d over the target, so a process crash mid-write cannot corrupt the file.
- **Activation semantics**: Changing the active certificate (via `POST /api/tls/activate/:name`) updates `bridge-state.json` immediately but the running `SSLServer` instance continues using the previously loaded cert until the next restart. The UI annotates this state as "pending restart". Enabling or disabling HTTPS mode also requires a restart.
- **Hot-reload is a non-goal**: There is no live cert swap without restart in this version.

---

### 3.2 Web Management Frontend

The web frontend is a single-page application (SPA) served as static HTML/CSS/JS embedded in the binary. It communicates with a **management REST API** also served by the same binary on the same web port.

#### 3.2.1 Management API (served on web-port)

**FR-2.1** The management API provides the following endpoints:

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/status` | Overall status: running, tools count, Ollama reachability, MCP servers list |
| GET | `/api/config` | Current runtime configuration (all settings) |
| POST | `/api/config` | Update configuration (applies without restart where possible) |
| GET | `/api/mcp-servers` | List MCP servers with per-server status (connected, tool count, errors) |
| POST | `/api/mcp-servers` | Add a new MCP server entry |
| PUT | `/api/mcp-servers/:name` | Update an existing MCP server entry |
| DELETE | `/api/mcp-servers/:name` | Remove an MCP server entry |
| POST | `/api/mcp-servers/:name/reconnect` | Force reconnect a specific MCP server |
| GET | `/api/tools` | List all currently registered tools (name, server, description) |
| POST | `/api/proxy/start` | Start the Ollama proxy (if stopped) |
| POST | `/api/proxy/stop` | Stop the Ollama proxy (without stopping the web UI) |
| GET | `/api/proxy/status` | Running / stopped, port, Ollama URL, uptime |
| GET | `/api/logs` | Last N log lines (query param `?n=200`) |
| POST | `/api/ollama/test` | Test connectivity to a given Ollama URL (body: `{"url":"..."}`) |
| GET | `/api/tls/certificates` | List all stored cert/key pairs: name, CN, SANs, expiry, active flag |
| POST | `/api/tls/generate-self-signed` | Start async key generation job (body: CN, days, key_size, sans[]); returns `{"job_id":"..."}` immediately |
| GET | `/api/tls/jobs/:job_id` | Poll job status: `{"status":"running"\|"done"\|"error", "cert_name":"...", "error":"..."}`. Completed/failed jobs expire after 5 minutes; max 20 tracked concurrently (HTTP 429 if exceeded) |
| POST | `/api/tls/upload` | Upload a PEM certificate and key (multipart/form-data); stores in cert dir |
| DELETE | `/api/tls/certificates/:name` | Delete a stored cert/key pair (rejected if it is the active certificate and HTTPS is enabled) |
| POST | `/api/tls/activate/:name` | Set a stored cert/key pair as the active one (takes effect on next restart) |

**FR-2.2** All management API responses are JSON. Errors use `{"error": "<message>"}` shape.

**FR-2.2a** The management API has **no authentication** in this version (see §4 Non-Goals). Operators MUST be informed (via startup log warning and README) that port 11464 should be firewalled on public-facing machines, or `--web-host 127.0.0.1` used to restrict access to localhost. A future version may add token-based auth.

**FR-2.2b** MCP server names used as `:name` path parameters in the management API must be URL-encoded by the client. Valid server name characters are: alphanumeric, hyphen (`-`), underscore (`_`), and dot (`.`). The server must validate names at registration time and reject names containing path-unsafe characters (spaces, slashes, etc.) with HTTP 400.

#### 3.2.2 Web Frontend Pages / Sections

The SPA has a left navigation with the following sections. Each control element has a **tooltip** (HTML `title` attribute + CSS-powered styled tooltip) that explains what it does.

**Dashboard**

- **FR-2.3** Displays a live status panel showing:
  - Proxy service status (Running / Stopped) with a green/red indicator
  - Ollama connection status (reachable / unreachable) and configured URL
  - Number of MCP servers connected / total
  - Total tools available
  - Current API proxy port
  - Web UI port
- **FR-2.4** Quick-action buttons: **Start Proxy**, **Stop Proxy** (only visible when appropriate), **Reconnect All MCP Servers**
- **FR-2.5** Dashboard refreshes status automatically every 5 seconds

**MCP Servers**

- **FR-2.6** Table listing all configured MCP servers with columns: Name, Type (stdio / HTTP / SSE), Status (Connected / Disconnected / Error), Tool Count, Actions
- **FR-2.7** Action buttons per server: **Reconnect**, **Edit**, **Delete**
- **FR-2.8** **Add Server** button opens a form (inline or modal) with fields:
  - Server name (unique identifier)
  - Transport type selector: `stdio` | `HTTP` | `SSE`
  - For `stdio`: command, arguments (comma/newline separated), environment variables (key=value pairs)
  - For `HTTP` / `SSE`: URL, optional HTTP headers (key=value pairs)
  - Tool filter mode: `none (all)` | `include` | `exclude`
  - Tool filter list (multiline, one tool name per line)
- **FR-2.9** Editing an existing server uses the same form pre-populated with current values
- **FR-2.10** All form fields have tooltips explaining the accepted format and semantics

**Tools**

- **FR-2.11** Read-only table of all currently loaded tools: Tool Name, Server, Description (truncated with expand)
- **FR-2.12** Filter/search bar to find tools by name or description

**Configuration**

- **FR-2.13** Form for all runtime settings:
  - Ollama URL (text input + **Test Connection** button that calls `POST /api/ollama/test`)
  - API Proxy Port (number input, requires restart note)
  - API Proxy Host bind address
  - Max Tool Rounds (number input, 0 = unlimited)
  - System Prompt (textarea)
  - CORS Origins (text input, comma-separated; `*` = allow all)
  - MCP Config File path (read-only display; changes require restart)
- **FR-2.14** **Save Configuration** button applies changes. Settings that require a restart are annotated with a "requires restart" badge
- **FR-2.15** All fields have tooltips with description and valid values/examples

**TLS / HTTPS sub-section (within Configuration)**

- **FR-2.15a** A toggle switch **Enable HTTPS** enables/disables TLS mode for the web server (requires restart; annotated accordingly).
- **FR-2.15b** When HTTPS is enabled, a **Certificate** dropdown lists all `.pem` / `.crt` + `.key` pairs found in the cert directory. Each entry shows the certificate's CN (Common Name) and expiry date. The active pair is pre-selected.
- **FR-2.15c** A **Generate Self-Signed Certificate** button opens an inline form with fields:
  - Common Name (CN) — e.g. hostname or IP address
  - Validity period in days (default: 365)
  - Key size: 2048 / 4096 bits (dropdown). When 4096 is selected the UI shows an inline warning: *"4096-bit key generation may take 10–30 seconds."*
  - An optional **Subject Alternative Names (SANs)** field (comma-separated IPs or DNS names)
  - A **Generate** button that calls `POST /api/tls/generate-self-signed`. Because key generation runs asynchronously in a background thread, the button transitions to a disabled spinner state and the UI polls `GET /api/tls/jobs/:job_id` every 2 seconds until status is `done` or `error`, then adds the new cert to the dropdown and selects it (or shows an error message).
- **FR-2.15d** An **Upload Certificate** area accepts a PEM certificate file and a PEM private key file (two separate file inputs, or a single combined PEM file). Calls `POST /api/tls/upload`. On success the new cert appears in the dropdown and is selected. **Security note**: if the web server is currently in plain HTTP mode, the UI displays a prominent warning banner above the upload form: *"⚠ You are uploading a private key over an unencrypted connection. Consider enabling HTTPS first or using --web-host 127.0.0.1 to restrict access to localhost."* This warning is **non-blocking** — the upload proceeds regardless; it is informational only.
- **FR-2.15e** The active certificate's CN and expiry date are displayed as read-only info text next to the dropdown, with a warning badge if the certificate expires within 30 days or is already expired.
- **FR-2.15f** The **Delete** action for a certificate entry (in the dropdown) is rendered as a disabled, greyed-out button when that entry is the currently active certificate and HTTPS is enabled, with tooltip text: *"Cannot delete the active certificate while HTTPS is enabled. Switch to a different certificate or disable HTTPS first."* The `DELETE /api/tls/certificates/:name` API also returns HTTP 409 Conflict with body `{"error": "Cannot delete the active certificate while HTTPS is enabled"}` if attempted directly.

**Logs**

- **FR-2.16** Scrollable log viewer showing the last 500 log lines (auto-refreshed every 3 seconds)
- **FR-2.17** **Clear Display** button to clear the view (does not delete logs)
- **FR-2.18** Log level color coding: ERROR=red, WARN=yellow, INFO=default, DEBUG=grey

#### 3.2.3 UI/UX Requirements

**FR-2.19** The frontend is usable without any external CDN resources — all CSS and JS are embedded in the binary. A minimal, clean design using plain HTML/CSS and vanilla JS is acceptable (no React / Vue / Angular required).

**FR-2.20** The UI is responsive and usable on screens ≥ 1024 px wide. Mobile is a nice-to-have, not required.

**FR-2.21** **Every** interactive element (button, input, select, table column header) has a tooltip (at minimum `title="..."` attribute) explaining what it does.

**FR-2.22** The page title and favicon reflect the application name. The favicon shall be a 32×32 SVG (inline `data:image/svg+xml` URI in the `<link rel="icon">` tag) so it requires no separate file and no binary format complications.

---

### 3.3 Ollama Connectivity from the Web Frontend

**FR-3.1** The Configuration page allows the user to change the Ollama URL and immediately test connectivity via a **Test Connection** button that calls `POST /api/ollama/test` and shows a success/failure indicator with latency.

**FR-3.2** The Dashboard shows a persistent Ollama connection indicator (green checkmark / red X) that reflects the current reachability status (checked at startup and on every status poll).

**FR-3.3** If Ollama is unreachable at startup the proxy still starts and the web UI reports the issue, rather than exiting (current behaviour exits). This is a behaviour change with the following mitigations:
- A CLI flag `--fail-on-ollama-unavailable` (and env var `FAIL_ON_OLLAMA_UNAVAILABLE=true`) restores the old exit-on-failure behaviour for users who rely on it for scripting, process supervision, or systemd `Restart=on-failure` health-check patterns.
- The proxy API (`/api/chat`, etc.) returns HTTP 503 with a clear error body while Ollama is unreachable, so downstream clients receive an explicit failure rather than a silent hang.
- The Dashboard shows a persistent, prominent warning banner while Ollama is unreachable.

---

### 3.4 Documentation Requirements

**FR-4.1** `README.md` is fully rewritten to cover:

1. **Project overview** — what it does and why
2. **Feature list** — complete, current, including web UI
3. **Architecture overview** — how the proxy, MCP manager, and web UI interact at a high level
4. **Debian 12 installation guide** — step-by-step from a fresh Debian 12 system:
   - Installing build dependencies (`cmake`, `g++`, `git`, etc.)
   - Cloning the repository
   - Building with CMake
   - Installing the binary system-wide (`/usr/local/bin`)
   - Creating a systemd service unit for auto-start
   - Firewall / port considerations (8000, 11464)
5. **Connecting to Ollama**:
   - From CLI: `--ollama-url` flag and `OLLAMA_URL` env var
   - From web frontend: Configuration page → Ollama URL field + Test Connection
   - Remote Ollama over the network
   - Ollama running in Docker
6. **MCP Server configuration reference** — full JSON schema with explanations of every field
7. **CLI reference** — all flags and env vars
8. **Web frontend guide** — each section of the UI described
9. **API reference** — proxy API endpoints and management API endpoints
10. **Development guide** — how to build from source. Note: the C++ codebase currently has no automated test suite; the guide documents the manual test approach (running the binary and verifying via `curl`) and notes that adding a C++ test framework (e.g., Catch2) is a future task.

**FR-4.2** The README must be accurate and not reference the Python implementation as the primary way to run the bridge (Python details may appear in a "Alternative: Python implementation" section).

---

## 4. Non-Goals

- Mobile-optimised web UI (responsive down to 768 px is sufficient)
- Authentication / access control for the web management frontend (out of scope; should be noted as a security consideration in the docs)
- Rewriting the Python implementation
- Migrating to a different HTTP server library than `cpp-httplib`
- Real-time log streaming via WebSocket (polling is sufficient)
- Automatic certificate renewal (e.g. Let's Encrypt / ACME integration)
- Enabling TLS on the Ollama API proxy port (default 8000) — TLS is supported on the web management port only

---

## 5. Assumptions

- **A-1**: "Merge the binaries into one binary" refers to embedding the web frontend server into the existing C++ binary, not merging the Python and C++ codebases. The C++ binary is already a single executable.
- **A-2**: The web frontend assets (HTML/CSS/JS) will be embedded as C++ string literals or header files generated at build time — no separate file serving is needed at runtime.
- **A-3**: Stopping the proxy (FR-2.4) means stopping the MCP-enriched `/api/chat` and Ollama proxy routes, while the web UI remains accessible. This requires the web server and proxy server to run in independent threads.
- **A-4**: The management API is co-located on the same port as the web frontend (port 11464), under the `/api/` prefix.
- **A-5**: Configuration changes that can be applied live (e.g., system prompt, max tool rounds, CORS, Ollama URL) are applied without restart. Changes that require a new socket bind (port, host) require restart.
- **A-6**: Log capture for the web log viewer will buffer recent log lines in a ring buffer in memory (e.g., last 500 lines).

---

## 6. Security Considerations

These are not blocking requirements but must be addressed in the README and via runtime warnings:

| Concern | Mitigation in this version |
|---------|---------------------------|
| Unauthenticated management API on port 11464 | Startup log warning; README firewall instructions; `--web-host` / `WEB_HOST` flag so operators can bind to `127.0.0.1` only (FR-1.3) |
| Self-signed certificate browser warnings | Expected behaviour; README documents how to add the cert to the OS/browser trust store |
| Private key stored on disk | Key files are written to the cert directory with `600` permissions (owner read/write only); README advises restricting the cert directory |
| CORS `*` default on proxy port | Existing warning log preserved; README security section updated |

---

## 7. Success Criteria

| Criterion | Measure |
|-----------|---------|
| Single binary | `cmake --build` produces exactly one executable that serves both API and web UI |
| Web UI accessible | Opening `http://localhost:11464` in a browser shows the management dashboard |
| Lifecycle control | Proxy can be stopped and started from the web UI without restarting the binary |
| MCP server management | Servers can be added, edited, deleted, and reconnected from the web UI |
| Tooltips | Every interactive element has a tooltip |
| Ollama connectivity test | Test button in Configuration shows result within 5 seconds |
| README accuracy | README installation steps produce a working binary on a clean Debian 12 VM |
| HTTPS mode | `curl -k https://localhost:11464/` returns the management dashboard when `--web-tls` and a valid cert/key are configured |
| Self-signed cert generation | Clicking Generate in the web UI produces a 2048-bit cert within 60 seconds on the target Debian 12 system and it appears in the dropdown |
