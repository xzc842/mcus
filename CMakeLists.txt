cmake_minimum_required(VERSION 3.16)
project(mcus LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# 可选：用 FetchContent 拉取 nlohmann/json 和 toml11，避免系统依赖问题
include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    toml11
    GIT_REPOSITORY https://github.com/ToruNiina/toml11.git
    GIT_TAG        v4.0.2
)
FetchContent_MakeAvailable(toml11)

add_library(mcus_core
    src/model.cpp
    src/shared_memory.cpp
    src/event_stream.cpp
    src/cpu.cpp
    src/simulator.cpp
    src/config.cpp
    src/isa_registry.cpp
    src/log.cpp
    src/isa_8051.cpp
)
target_include_directories(mcus_core PUBLIC include)
target_link_libraries(mcus_core
    PUBLIC
        Threads::Threads
        nlohmann_json::nlohmann_json
        toml11::toml11
)
target_compile_options(mcus_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mcus src/main.cpp)
target_link_libraries(mcus PRIVATE mcus_core)

# 调试工具：dump 共享内存
add_executable(dump_shm tools/dump_shm.cpp)
target_link_libraries(dump_shm PRIVATE mcus_core)

# 测试
enable_testing()
add_subdirectory(tests)