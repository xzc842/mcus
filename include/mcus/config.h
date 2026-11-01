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
    uint32_t control_poll_interval = 64;
    uint32_t clock_step_interval = 100;
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