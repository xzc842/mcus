#include "mcus/model.h"
#include "mcus/config.h"
#include "mcus/simulator.h"
#include "mcus/log.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <string>

static std::vector<uint8_t> load_binary(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open firmware: " + path);
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
}

static mcus::LogLevel parse_log_level(const std::string& s) {
    if (s == "trace") return mcus::LogLevel::Trace;
    if (s == "debug") return mcus::LogLevel::Debug;
    if (s == "info")  return mcus::LogLevel::Info;
    if (s == "warn")  return mcus::LogLevel::Warn;
    if (s == "error") return mcus::LogLevel::Error;
    return mcus::LogLevel::Info;
}

int main(int argc, char** argv) {
    mcus::register_all_isas();
    if (argc < 4) {
        std::cerr << "usage: mcus <model.json> <firmware.bin> <config.toml>\n";
        return 1;
    }

    try {
        mcus::Config cfg = mcus::Config::load_from_file(argv[3]);
        mcus::log_set_level(parse_log_level(cfg.debug.log_level));
        mcus::log_set_output(cfg.debug.log_output);

        mcus::Model model = mcus::Model::load_from_file(argv[1]);
        cfg.apply_to_model(model);

        auto firmware = load_binary(argv[2]);
        MCUS_INFO("model=" + model.name + " crystal=" +
                  std::to_string(model.crystal_hz) + "Hz");

        mcus::Simulator sim(std::move(model), std::move(firmware), cfg);
        sim.run();
    } catch (const std::exception& e) {
        std::cerr << "mcus error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}