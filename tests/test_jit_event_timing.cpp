// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0
//
// 外设事件时序差分：8051 UART0 TX 的 BusEvent 时间戳解释器 vs JIT。

#include "mcus/cpu.h"
#include "mcus/event_stream.h"
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"
#include "mcus/model.h"
#include "mcus/shared_memory.h"

#include <sys/mman.h>
#include <cstdio>
#include <vector>

using namespace mcus;

extern "C" InstructionSet* mcus_isa_create(const Model*);
extern "C" void mcus_isa_destroy(InstructionSet*);

static void install_8051() {
    IsaRegistry::instance().register_isa(
        "8051", [](const Model& m) -> IsaPtr {
            return IsaPtr(mcus_isa_create(&m), IsaDeleter{mcus_isa_destroy});
        });
}

struct TxEvent {
    VirtTime time = 0;
    uint8_t data = 0;
};

static std::vector<TxEvent> run(const Model& model, bool jit,
                                const char* suffix) {
    const std::string shm_name = std::string("/mcus_jit_event_") + suffix;
    const std::string stream_name = shm_name + "_s";
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());

    std::vector<TxEvent> out;
    {
        SharedMemory shm(model, shm_name);
        EventStream stream(stream_name, 256);
        Cpu cpu(model, shm, stream);
        // MOV 0x99,#0x41; SJMP 0
        const std::vector<uint8_t> fw = {0x75, 0x99, 0x41, 0x80, 0xFB};
        cpu.load_firmware(fw);
        if (jit && !cpu.enable_jit(MCUS_TEST_JIT8051_PATH)) {
            std::puts("jit enable failed");
            return out;
        }

        if (jit) {
            uint64_t done = 0;
            while (done < 20) {
                const uint64_t n = cpu.run_jit(20 - done);
                if (n == 0) {
                    const VirtTime dt = cpu.step();
                    if (dt == 0) break;
                    cpu.advance_peripherals(dt);
                    ++done;
                    continue;
                }
                done += n;
            }
        } else {
            for (int i = 0; i < 20; ++i) {
                const VirtTime dt = cpu.step();
                if (dt == 0) break;
                cpu.advance_peripherals(dt);
            }
        }

        DebugEvent ev{};
        while (stream.pop(ev)) {
            if (ev.type == DebugEventType::Bus &&
                ev.u.bus.kind == static_cast<uint8_t>(BusEventKind::UartTx))
                out.push_back({ev.time, ev.u.bus.data[0]});
        }
    }
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    return out;
}

int main() {
    install_8051();
    const Model model = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);
    const auto a = run(model, false, "interp");
    const auto b = run(model, true, "jit");

    std::printf("interp tx=%zu", a.size());
    for (const auto& e : a)
        std::printf(" [t=%llu,0x%02x]", (unsigned long long)e.time, e.data);
    std::printf("\njit    tx=%zu", b.size());
    for (const auto& e : b)
        std::printf(" [t=%llu,0x%02x]", (unsigned long long)e.time, e.data);
    std::printf("\n");

    if (a.size() != b.size()) {
        std::puts("uart tx count mismatch");
        return 1;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].time != b[i].time || a[i].data != b[i].data) {
            std::puts("uart tx timing mismatch");
            return 1;
        }
    }
    std::puts("jit event timing ok");
    return 0;
}
