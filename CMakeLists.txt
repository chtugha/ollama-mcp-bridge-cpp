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

set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(json httplib cli11 spdlog)

find_package(Threads REQUIRED)

if(APPLE)
    find_library(SECURITY_FRAMEWORK Security)
    find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
endif()

add_executable(ollama-mcp-bridge
    cpp/main.cpp
    cpp/utils.cpp
    cpp/mcp_client.cpp
    cpp/mcp_manager.cpp
    cpp/proxy_service.cpp
    cpp/server.cpp
)

target_include_directories(ollama-mcp-bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp)

target_link_libraries(ollama-mcp-bridge PRIVATE
    nlohmann_json::nlohmann_json
    httplib::httplib
    CLI11::CLI11
    spdlog::spdlog
    Threads::Threads
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
