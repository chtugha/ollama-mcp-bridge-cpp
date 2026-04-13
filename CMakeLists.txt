cmake_minimum_required(VERSION 3.14)
project(ollama-mcp-bridge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.3
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.4.1
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
    GIT_SHALLOW TRUE
)

set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(json httplib cli11 spdlog)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

if(APPLE)
    find_library(SECURITY_FRAMEWORK Security)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
endif()

# Embed web/index.html into a C++ raw string literal at BUILD time.
# Uses add_custom_command so that touching web/index.html without re-running
# cmake still triggers a regeneration on the next `make` / `ninja` invocation.
# The raw-string delimiter OMB_WEB_9F3A must not appear verbatim in index.html.

set(WEB_UI_H   "${CMAKE_CURRENT_BINARY_DIR}/web_ui.h")
set(WEB_UI_CPP "${CMAKE_CURRENT_BINARY_DIR}/web_ui.cpp")

# The header only declares the symbol — it never changes, so write it once at
# configure time (avoids the custom command having to emit two files).
file(WRITE "${WEB_UI_H}"
    "// Auto-generated from web/index.html — do not edit\n"
    "#pragma once\n"
    "extern const char* const WEB_UI_HTML;\n"
)

add_custom_command(
    OUTPUT  "${WEB_UI_CPP}"
    COMMAND ${CMAKE_COMMAND}
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "-DOUTPUT=${WEB_UI_CPP}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_web.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_web.cmake"
    COMMENT "Embedding web/index.html → web_ui.cpp"
)

add_executable(ollama-mcp-bridge
    ${CMAKE_CURRENT_BINARY_DIR}/web_ui.cpp
    cpp/main.cpp
    cpp/utils.cpp
    cpp/mcp_client.cpp
    cpp/mcp_manager.cpp
    cpp/proxy_service.cpp
    cpp/server.cpp
    cpp/config_manager.cpp
    cpp/log_buffer.cpp
    cpp/tls_manager.cpp
    cpp/web_server.cpp
)

target_include_directories(ollama-mcp-bridge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/cpp
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(ollama-mcp-bridge PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    CLI11::CLI11
    spdlog::spdlog
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(APPLE)
    target_link_libraries(ollama-mcp-bridge PRIVATE
        ${SECURITY_FRAMEWORK}
        ${COREFOUNDATION_FRAMEWORK}
    )
endif()

target_compile_definitions(ollama-mcp-bridge PRIVATE
    PROJECT_VERSION="${PROJECT_VERSION}"
)
