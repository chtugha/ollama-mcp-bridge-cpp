# Auto

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Agent Instructions

Ask the user questions when anything is unclear or needs their input. This includes:
- Ambiguous or incomplete requirements
- Technical decisions that affect architecture or user experience
- Trade-offs that require business context

Do not make assumptions on important decisions — get clarification first.

---

## Workflow Steps

### [x] Step: Implementation
<!-- chat-id: 8559c320-706b-4019-9778-2a24a869f139 -->

Convert the entire Python ollama-mcp-bridge to C++17 as a standalone binary.

**Architecture decisions:**
- C++17, CMake build system
- Header-only libs via FetchContent: cpp-httplib, nlohmann/json, CLI11, spdlog
- All MCP transports: stdio, SSE, StreamableHTTP
- Standalone binary, no runtime dependencies

**Files to create:**
- `CMakeLists.txt` - build config with FetchContent
- `cpp/main.cpp` - CLI entry point
- `cpp/server.h/cpp` - HTTP server routes
- `cpp/proxy_service.h/cpp` - Ollama proxy logic
- `cpp/mcp_manager.h/cpp` - MCP server management
- `cpp/mcp_client.h/cpp` - MCP protocol client
- `cpp/utils.h/cpp` - Utilities
- Update `.gitignore` for C++ artifacts
