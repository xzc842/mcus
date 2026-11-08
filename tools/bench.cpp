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

// mcus 性能基准：CPU 指令吞吐、事件流吞吐、共享内存寄存器访问。
//
// 示例：
//   ./build/mcus_bench --model models/ch552g.json --isa ./build/libmcus_isa_8051.so
//   ./build/mcus_bench --model models/stm32f103c8t6.json --isa ./build/libmcus_isa_arm.so
//   ./build/mcus_bench --model models/dummy.json --steps 500000 --repeat 5

#include "mcus/model.h"
#include "mcus/config.h"
#include "mcus/cpu.h"
#include "mcus/shared_memory.h"
#include "mcus/event_stream.h"
#include "mcus/firmware.h"
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"
#include "mcus/log.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string model = "models/dummy.json";
    std::string isa;
    std::string firmware;
    uint64_t steps = 1000000;
    uint64_t warmup = 10000;
    uint32_t repeat = 3;
    bool json = false;
    bool skip_events = false;
    bool skip_shm = false;
};

void usage(const char* argv0) {
    std::cerr
        << "usage: " << argv0 << " [options]\n"
        << "  --model, -m <model.json>   芯片模型（默认 models/dummy.json）\n"
        << "  --isa, -i <isa.so>         ISA 动态库（mcus-minimal 可省略）\n"
        << "  --firmware, -f <file>      可选固件；默认生成 tight-loop\n"
        << "  --steps <N>                每次测量的指令数（默认 1000000）\n"
        << "  --warmup <N>               预热指令数（默认 10000）\n"
        << "  --repeat <N>               重复次数，取最好成绩（默认 3）\n"
        << "  --skip-events              跳过事件流基准\n"
        << "  --skip-shm                 跳过共享内存基准\n"
        << "  --json                     输出 JSON\n";
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            std::exit(0);
        } else if (a == "-m" || a == "--model") {
            o.model = next("--model");
        } else if (a == "-i" || a == "--isa") {
            o.isa = next("--isa");
        } else if (a == "-f" || a == "--firmware") {
            o.firmware = next("--firmware");
        } else if (a == "--steps") {
            o.steps = std::stoull(next("--steps"));
        } else if (a == "--warmup") {
            o.warmup = std::stoull(next("--warmup"));
        } else if (a == "--repeat") {
            o.repeat = static_cast<uint32_t>(std::stoul(next("--repeat")));
        } else if (a == "--skip-events") {
            o.skip_events = true;
        } else if (a == "--skip-shm") {
            o.skip_shm = true;
        } else if (a == "--json") {
            o.json = true;
        } else {
            throw std::runtime_error("unknown option: " + a);
        }
    }
    if (o.repeat == 0) o.repeat = 1;
    if (o.steps == 0) o.steps = 1;
    return o;
}

void load_isa(const std::string& path, const std::string& model_arch) {
    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
        throw std::runtime_error("dlopen " + path + ": " + ::dlerror());
    auto abi = reinterpret_cast<mcus::IsaAbiVersionFn>(
        ::dlsym(handle, mcus::kIsaAbiVersionSymbol));
    auto arch = reinterpret_cast<mcus::IsaArchFn>(
        ::dlsym(handle, mcus::kIsaArchSymbol));
    auto create = reinterpret_cast<mcus::IsaCreateFn>(
        ::dlsym(handle, mcus::kIsaCreateSymbol));
    auto destroy = reinterpret_cast<mcus::IsaDestroyFn>(
        ::dlsym(handle, mcus::kIsaDestroySymbol));
    if (!abi || !arch || !create || !destroy)
        throw std::runtime_error("ISA library missing ABI symbols: " + path);
    if (abi() != mcus::kIsaAbiVersion)
        throw std::runtime_error("ISA ABI mismatch: " + path);
    if (arch() != model_arch)
        throw std::runtime_error("ISA arch mismatch: library=" +
                                 std::string(arch()) + " model=" + model_arch);
    mcus::IsaRegistry::instance().register_isa(
        arch(), [create, destroy](const mcus::Model& m) -> mcus::IsaPtr {
            return mcus::IsaPtr(create(&m), mcus::IsaDeleter{destroy});
        });
}

mcus::FirmwareImage make_loop_firmware(const mcus::Model& model) {
    using namespace mcus;
    FirmwareImage fw;
    fw.load_addr = 0;
    if (model.arch == "8051") {
        fw.data = {0x80, 0xFE}; // SJMP $
    } else if (model.arch == "mcus-minimal") {
        fw.data = {0x03, 0x00, 0x00, 0x00, 0x00}; // JMP 0
    } else if (model.arch == "arm") {
        fw.data.assign(0x44, 0);
        auto put32 = [&](size_t off, uint32_t v) {
            for (int i = 0; i < 4; ++i)
                fw.data[off + i] = static_cast<uint8_t>(v >> (8 * i));
        };
        const uint32_t sp = model.arch_desc.initial_sp
                                ? model.arch_desc.initial_sp
                                : 0x20000000u;
        const uint32_t handler = model.arch_desc.reset_vector + 0x40;
        put32(0x00, sp);
        put32(0x04, handler | 1u);
        put32(0x40, 0xE7FE); // b .
    } else {
        throw std::runtime_error("no default benchmark firmware for arch: " +
                                 model.arch);
    }
    return fw;
}

double elapsed_seconds(std::chrono::steady_clock::time_point a,
                       std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

struct CpuResult {
    double steps_per_sec = 0;
    double cycles_per_sec = 0;
    double ns_per_step = 0;
    uint64_t steps = 0;
};

CpuResult run_cpu(const mcus::Model& model, const mcus::FirmwareImage& fw,
                  uint64_t steps, uint64_t warmup, int round) {
    using namespace mcus;
    const std::string shm_name =
        "/mcus_bench_cpu_" + std::to_string(::getpid()) + "_" +
        std::to_string(round);
    const std::string stream_name = shm_name + "_s";
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());

    CpuResult result;
    {
        SharedMemory shm(model, shm_name);
        EventStream stream(stream_name, 256);
        Cpu cpu(model, shm, stream);
        cpu.load_firmware(fw);

        for (uint64_t i = 0; i < warmup; ++i) {
            VirtTime dt = cpu.step();
            if (dt == 0 && cpu.halted()) break;
            if (dt) cpu.advance_peripherals(dt);
        }

        const auto t0 = std::chrono::steady_clock::now();
        uint64_t done = 0;
        uint64_t cycles = 0;
        for (uint64_t i = 0; i < steps; ++i) {
            VirtTime dt = cpu.step();
            if (dt == 0) {
                if (cpu.halted()) break;
                continue;
            }
            ++done;
            cycles += dt;
            cpu.advance_peripherals(dt);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double sec = std::max(1e-9, elapsed_seconds(t0, t1));
        result.steps = done;
        result.steps_per_sec = static_cast<double>(done) / sec;
        result.cycles_per_sec = static_cast<double>(cycles) / sec;
        result.ns_per_step = sec * 1e9 / static_cast<double>(std::max<uint64_t>(1, done));
    }
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    return result;
}

double run_event_stream(uint64_t n, int round) {
    using namespace mcus;
    const std::string name =
        "/mcus_bench_ev_" + std::to_string(::getpid()) + "_" +
        std::to_string(round);
    ::shm_unlink(name.c_str());
    double rate = 0;
    {
        EventStream s(name, 4096);
        DebugEvent ev{};
        ev.type = DebugEventType::Mmio;
        ev.u.mmio.addr = 0x1000;
        ev.u.mmio.value = 1;
        ev.u.mmio.width = 32;

        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < n; ++i) {
            if (!s.push(ev)) {
                DebugEvent drop;
                s.pop(drop);
                s.push(ev);
            }
            DebugEvent out{};
            s.pop(out);
        }
        const auto t1 = std::chrono::steady_clock::now();
        rate = static_cast<double>(n) / std::max(1e-9, elapsed_seconds(t0, t1));
    }
    ::shm_unlink(name.c_str());
    return rate;
}

double run_shared_memory(const mcus::Model& model, uint64_t n, int round) {
    using namespace mcus;
    const std::string name =
        "/mcus_bench_shm_" + std::to_string(::getpid()) + "_" +
        std::to_string(round);
    ::shm_unlink(name.c_str());
    double rate = 0;
    {
        SharedMemory shm(model, name);
        if (model.registers.empty())
            throw std::runtime_error("model has no MMIO register");
        const RegisterDesc* rd = &model.registers.front();
        const uint8_t width = rd->width;
        const uint32_t mask = width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1u);

        volatile uint32_t sink = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (uint64_t i = 0; i < n; ++i) {
            shm.write_reg(rd->space, rd->addr,
                          static_cast<uint32_t>(i) & mask, width);
            sink ^= shm.read_reg(rd->space, rd->addr, width);
        }
        const auto t1 = std::chrono::steady_clock::now();
        (void)sink;
        rate = static_cast<double>(n) / std::max(1e-9, elapsed_seconds(t0, t1));
    }
    ::shm_unlink(name.c_str());
    return rate;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Options opt = parse(argc, argv);
        // 基准输出保持干净；ISA/模型错误仍通过异常报告。
        mcus::log_set_level(mcus::LogLevel::Error);
        mcus::Model model = mcus::Model::load_from_file(opt.model);
        if (!opt.isa.empty()) load_isa(opt.isa, model.arch);
        else if (model.arch != "mcus-minimal")
            throw std::runtime_error("model arch requires --isa: " + model.arch);

        mcus::FirmwareImage fw =
            opt.firmware.empty() ? make_loop_firmware(model)
                                 : mcus::load_firmware(opt.firmware);

        CpuResult best;
        for (uint32_t r = 0; r < opt.repeat; ++r) {
            CpuResult cur = run_cpu(model, fw, opt.steps, opt.warmup,
                                    static_cast<int>(r));
            if (cur.steps_per_sec > best.steps_per_sec) best = cur;
        }

        double ev_rate = 0;
        double shm_rate = 0;
        if (!opt.skip_events)
            ev_rate = run_event_stream(std::max<uint64_t>(1000, opt.steps), 0);
        if (!opt.skip_shm)
            shm_rate = run_shared_memory(model, std::max<uint64_t>(1000, opt.steps), 0);

        if (opt.json) {
            std::printf(
                "{\"model\":\"%s\",\"arch\":\"%s\",\"steps\":%llu,"
                "\"steps_per_sec\":%.0f,\"virtual_cycles_per_sec\":%.0f,"
                "\"ns_per_step\":%.3f,\"events_per_sec\":%.0f,"
                "\"shm_ops_per_sec\":%.0f}\n",
                model.name.c_str(), model.arch.c_str(),
                static_cast<unsigned long long>(best.steps),
                best.steps_per_sec, best.cycles_per_sec, best.ns_per_step,
                ev_rate, shm_rate);
        } else {
            std::printf("mcus benchmark\n");
            std::printf("  model : %s (%s)\n", model.name.c_str(),
                        model.arch.c_str());
            std::printf("  steps : %llu (warmup=%llu, repeat=%u)\n",
                        static_cast<unsigned long long>(opt.steps),
                        static_cast<unsigned long long>(opt.warmup),
                        opt.repeat);
            std::printf("  cpu   : %.0f steps/s, %.0f virtual cycles/s, "
                        "%.2f ns/step\n",
                        best.steps_per_sec, best.cycles_per_sec,
                        best.ns_per_step);
            if (!opt.skip_events)
                std::printf("  events: %.0f events/s\n", ev_rate);
            if (!opt.skip_shm)
                std::printf("  shm   : %.0f register ops/s\n", shm_rate);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "mcus_bench error: " << e.what() << "\n";
        return 1;
    }
}
