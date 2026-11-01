// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mcus/config.h"
#include "mcus/model.h"
#include <toml.hpp>
#include <stdexcept>

namespace mcus {

static toml::value get_section(const toml::value& data, const char* key) {
    if (data.contains(key)) {
        return data.at(key);
    }
    return toml::value(toml::table{});
}

Config Config::load_from_file(const std::string& path) {
    Config cfg;
    toml::value data;
    try {
        data = toml::parse(path);
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot parse config: " + std::string(e.what()));
    }

    {
        toml::value sim = get_section(data, "simulator");
        cfg.simulator.shm_name = toml::find_or<std::string>(
            sim, "shm_name", cfg.simulator.shm_name);
        cfg.simulator.stream_name = toml::find_or<std::string>(
            sim, "stream_name", cfg.simulator.stream_name);
        cfg.simulator.stream_capacity = static_cast<size_t>(
            toml::find_or<int64_t>(sim, "stream_capacity",
                static_cast<int64_t>(cfg.simulator.stream_capacity)));
        cfg.simulator.start_paused = toml::find_or<bool>(
            sim, "start_paused", cfg.simulator.start_paused);
        cfg.simulator.control_poll_interval = static_cast<uint32_t>(
            toml::find_or<int64_t>(sim, "control_poll_interval",
                static_cast<int64_t>(cfg.simulator.control_poll_interval)));
        cfg.simulator.clock_step_interval = static_cast<uint32_t>(
            toml::find_or<int64_t>(sim, "clock_step_interval",
                static_cast<int64_t>(cfg.simulator.clock_step_interval)));
        if (cfg.simulator.clock_step_interval == 0)
            cfg.simulator.clock_step_interval = 1;
    }

    {
        toml::value cry = get_section(data, "crystal");
        cfg.crystal.frequency_hz = static_cast<uint32_t>(
            toml::find_or<int64_t>(cry, "frequency_hz",
                static_cast<int64_t>(cfg.crystal.frequency_hz)));
        cfg.crystal.cycles_per_instruction = static_cast<uint32_t>(
            toml::find_or<int64_t>(cry, "cycles_per_instruction",
                static_cast<int64_t>(cfg.crystal.cycles_per_instruction)));
    }

    {
        toml::value dbg = get_section(data, "debug");
        cfg.debug.log_level = toml::find_or<std::string>(
            dbg, "log_level", cfg.debug.log_level);
        cfg.debug.log_output = toml::find_or<std::string>(
            dbg, "log_output", cfg.debug.log_output);
        cfg.debug.dump_prefix = toml::find_or<std::string>(
            dbg, "dump_prefix", cfg.debug.dump_prefix);
    }

    return cfg;
}

void Config::apply_to_model(Model& model) const {
    if (crystal.frequency_hz != 0) {
        model.crystal_hz = crystal.frequency_hz;
    }
    if (crystal.cycles_per_instruction != 0) {
        model.cycles_per_instruction = crystal.cycles_per_instruction;
    }
}

} // namespace mcus