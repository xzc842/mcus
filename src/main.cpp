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

#include "mcus/model.h"
#include "mcus/config.h"
#include "mcus/simulator.h"
#include "mcus/log.h"
#include "mcus/firmware.h"
#include "mcus/isa_plugin.h"

#include <dlfcn.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string isa;
    std::string model;
    std::string firmware;
    std::string config;
};

void usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " --isa <isa.so> --model <model.json> "
        << "--firmware <firmware.hex|firmware.bin> --config <config.toml>\n"
        << "\n"
        << "  --isa, -i       架构指令集动态库（如 libmcus_isa_8051.so）\n"
        << "  --model, -m     芯片模型文件（JSON）\n"
        << "  --firmware, -f  固件文件（.bin 或 .hex/.ihx）\n"
        << "  --config, -c    配置文件（TOML）\n";
}

std::string require_value(int argc, char** argv, int& i,
                          const std::string& name) {
    if (i + 1 >= argc) {
        throw std::runtime_error("参数 " + name + " 缺少值");
    }
    return argv[++i];
}

Options parse_args(int argc, char** argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (arg == "-i" || arg == "--isa") {
            opts.isa = require_value(argc, argv, i, arg);
        } else if (arg == "-m" || arg == "--model") {
            opts.model = require_value(argc, argv, i, arg);
        } else if (arg == "-f" || arg == "--firmware") {
            opts.firmware = require_value(argc, argv, i, arg);
        } else if (arg == "-c" || arg == "--config") {
            opts.config = require_value(argc, argv, i, arg);
        } else {
            throw std::runtime_error("未知参数: " + arg);
        }
    }

    if (opts.isa.empty() || opts.model.empty() ||
        opts.firmware.empty() || opts.config.empty()) {
        usage(argv[0]);
        throw std::runtime_error("--isa/--model/--firmware/--config 都是必填项");
    }
    return opts;
}

mcus::LogLevel parse_log_level(const std::string& s) {
    if (s == "trace") return mcus::LogLevel::Trace;
    if (s == "debug") return mcus::LogLevel::Debug;
    if (s == "info")  return mcus::LogLevel::Info;
    if (s == "warn")  return mcus::LogLevel::Warn;
    if (s == "error") return mcus::LogLevel::Error;
    return mcus::LogLevel::Info;
}

// 加载外部 ISA 动态库，并按模型 arch 注册到指令集注册表。
// handle 由调用方持有，进程生命周期内不 dlclose。
void* load_isa_library(const std::string& path, const std::string& model_arch) {
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error("无法加载 ISA 库 " + path + ": " +
                                 std::string(::dlerror()));
    }

    auto abi = reinterpret_cast<mcus::IsaAbiVersionFn>(
        ::dlsym(handle, mcus::kIsaAbiVersionSymbol));
    auto arch = reinterpret_cast<mcus::IsaArchFn>(
        ::dlsym(handle, mcus::kIsaArchSymbol));
    auto create = reinterpret_cast<mcus::IsaCreateFn>(
        ::dlsym(handle, mcus::kIsaCreateSymbol));
    auto destroy = reinterpret_cast<mcus::IsaDestroyFn>(
        ::dlsym(handle, mcus::kIsaDestroySymbol));

    if (!abi || !arch || !create || !destroy) {
        ::dlclose(handle);
        throw std::runtime_error(
            "ISA 库 " + path +
            " 缺少入口符号 mcus_isa_abi_version / mcus_isa_arch / "
            "mcus_isa_create / mcus_isa_destroy");
    }

    if (abi() != mcus::kIsaAbiVersion) {
        ::dlclose(handle);
        throw std::runtime_error(
            "ISA ABI 版本不匹配: 库为 " + std::to_string(abi()) +
            "，模拟器要求 " + std::to_string(mcus::kIsaAbiVersion));
    }

    const std::string lib_arch = arch();
    if (lib_arch != model_arch) {
        ::dlclose(handle);
        throw std::runtime_error("ISA 架构不匹配: 库为 " + lib_arch +
                                 "，模型为 " + model_arch);
    }

    mcus::IsaRegistry::instance().register_isa(
        lib_arch,
        [create, destroy](const mcus::Model& model) -> mcus::IsaPtr {
            return mcus::IsaPtr(create(&model), mcus::IsaDeleter{destroy});
        });
    return handle;
}

} // namespace

int main(int argc, char** argv) {
    mcus::register_all_isas();

    try {
        Options opts = parse_args(argc, argv);

        mcus::Config cfg = mcus::Config::load_from_file(opts.config);
        mcus::log_set_level(parse_log_level(cfg.debug.log_level));
        mcus::log_set_output(cfg.debug.log_output);

        mcus::Model model = mcus::Model::load_from_file(opts.model);
        cfg.apply_to_model(model);

        void* isa_handle = load_isa_library(opts.isa, model.arch);

        auto firmware = mcus::load_firmware(opts.firmware);
        MCUS_INFO("model=" + model.name + " arch=" + model.arch +
                  " crystal=" + std::to_string(model.crystal_hz) + "Hz");
        MCUS_INFO("firmware=" + opts.firmware + " (load=0x" +
                  std::to_string(firmware.load_addr) + ", " +
                  std::to_string(firmware.data.size()) + " bytes)");

        mcus::Simulator sim(std::move(model), std::move(firmware), cfg);
        sim.run();

        (void)isa_handle;
    } catch (const std::exception& e) {
        std::cerr << "mcus error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
