/*
 * Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <string>
#include <cstdint>
#include "mcus/model.h"

namespace mcus {

struct SimulatorSection {
    std::string shm_name = "/mcus_shm";
    std::string stream_name = "/mcus_stream";
    size_t stream_capacity = 4096;
    bool start_paused = false;
    bool stay_alive = false;
    std::string jit;  // 可选：8051 JIT Translator 插件路径
    uint32_t control_poll_interval = 64;
    uint32_t clock_step_interval = 100;
    // 同步总线等待外部响应的真实时间上限；0=无限等待。
    uint32_t bus_wait_timeout_ms = 1000;
};

struct CrystalSection {
    uint32_t frequency_hz = 16000000;
    uint32_t cycles_per_instruction = 4;
};

struct DebugSection {
    std::string log_level = "info";
    std::string log_output = "stderr";
    std::string dump_prefix = "/tmp/mcus_dump";
};

struct Config {
    SimulatorSection simulator;
    CrystalSection crystal;
    DebugSection debug;

    static Config load_from_file(const std::string& path);
    void apply_to_model(Model& model) const;
};

} // namespace mcus