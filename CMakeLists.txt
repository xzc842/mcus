# Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

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

# 模拟器核心：不含具体架构 ISA，ISA 由 .so 动态加载
add_library(mcus_core SHARED
    src/model.cpp
    src/shared_memory.cpp
    src/event_stream.cpp
    src/cpu.cpp
    src/simulator.cpp
    src/config.cpp
    src/isa_base.cpp
    src/isa_registry.cpp
    src/firmware.cpp
    src/log.cpp
)
target_include_directories(mcus_core PUBLIC include)
target_link_libraries(mcus_core
    PUBLIC
        Threads::Threads
        nlohmann_json::nlohmann_json
        toml11::toml11
)
target_compile_options(mcus_core PRIVATE -Wall -Wextra -Wpedantic)

# 8051 指令集：外部 ISA 动态库，入口符号见 include/mcus/isa_plugin.h
add_library(mcus_isa_8051 SHARED src/isa_8051.cpp)
target_link_libraries(mcus_isa_8051 PRIVATE mcus_core)
target_compile_options(mcus_isa_8051 PRIVATE -Wall -Wextra -Wpedantic)

add_executable(mcus src/main.cpp)
target_link_libraries(mcus PRIVATE mcus_core ${CMAKE_DL_LIBS})
set_target_properties(mcus PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# 调试工具：dump 共享内存
add_executable(dump_shm tools/dump_shm.cpp)
target_link_libraries(dump_shm PRIVATE mcus_core)

# 测试
enable_testing()
add_subdirectory(tests)
