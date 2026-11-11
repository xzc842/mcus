// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0
//
// 时序差分：同一 8051 固件，解释器和 JIT 在相同的虚拟时间注入中断，
// 比较 Irq 事件时间、数量、入口 PC 和最终虚拟时间。

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

struct Trace {
    uint32_t irq_count = 0;
    uint64_t irq_time = 0;
    uint32_t irq_number = 0;
    uint32_t pc = 0;
    VirtTime now = 0;
};

static uint8_t reg_width(const Model& m, const SpaceId& space, Addr addr) {
    const RegisterDesc* r = m.find_register(space, addr);
    return r ? r->width : 8;
}

static void write_bits(const Model& m, SharedMemory& shm, const SpaceId& space,
                       Addr addr, uint32_t mask) {
    const uint8_t w = reg_width(m, space, addr);
    const uint32_t v = shm.read_reg(space, addr, w) | mask;
    shm.write_reg(space, addr, v, w);
}

static Trace run(const Model& model, const InterruptDesc& irq,
                 const std::vector<uint8_t>& firmware, bool jit,
                 const char* suffix) {
    const std::string shm_name = std::string("/mcus_jit_timing_") + suffix;
    const std::string stream_name = shm_name + "_s";
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());

    Trace tr{};
    {
        SharedMemory shm(model, shm_name);
        EventStream stream(stream_name, 256);
        Cpu cpu(model, shm, stream);
        cpu.load_firmware(firmware);

        // 全局中断使能 + 目标中断使能。
        const auto& g = model.arch_desc.global_irq_enable;
        if (g.present)
            write_bits(model, shm, g.space, g.reg, 1u << g.bit);
        write_bits(model, shm, irq.enable_space, irq.enable_reg,
                   1u << irq.enable_bit);

        const uint64_t pre = 20;
        if (jit) {
            if (!cpu.enable_jit(MCUS_TEST_JIT8051_PATH)) {
                std::puts("jit enable failed");
                return tr;
            }
            cpu.run_jit(pre);
        } else {
            for (uint64_t i = 0; i < pre; ++i) {
                const VirtTime dt = cpu.step();
                if (dt) cpu.advance_peripherals(dt);
            }
        }
        tr.now = cpu.now();

        // 在同一个虚拟时间点注入 pending。
        write_bits(model, shm, irq.pending_space, irq.pending_reg,
                   1u << irq.pending_bit);

        if (jit) {
            // run_jit 遇到中断会提前返回；正常 Simulator 会再次进入 JIT，
            // 这里同样循环到 32 条指令，比较完整路径的时序。
            uint64_t done = 0;
            while (done < 32) {
                const uint64_t n = cpu.run_jit(32 - done);
                if (n == 0) {
                    // 入场检查进入中断/断点时 run_jit 不执行指令，
                    // 正常 Simulator 会回退到 step()；这里同样处理。
                    const VirtTime dt = cpu.step();
                    if (dt == 0) break;
                    cpu.advance_peripherals(dt);
                    ++done;
                    continue;
                }
                done += n;
            }
        } else {
            for (int i = 0; i < 32; ++i) {
                const VirtTime dt = cpu.step();
                if (dt == 0) break;
                cpu.advance_peripherals(dt);
            }
        }

        DebugEvent ev{};
        while (stream.pop(ev)) {
            if (ev.type == DebugEventType::Irq) {
                if (tr.irq_count == 0) {
                    tr.irq_time = ev.time;
                    tr.irq_number = ev.u.irq.number;
                }
                ++tr.irq_count;
            }
        }
        tr.pc = cpu.regs().pc;
        tr.now = cpu.now();
    }
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    return tr;
}

int main() {
    install_8051();
    const Model model = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);
    if (model.interrupts.empty()) {
        std::puts("no interrupt in model");
        return 1;
    }
    const InterruptDesc& irq = model.interrupts.front();

    // reset: SJMP $；中断入口也放 SJMP $，只观察第一次进入。
    std::vector<uint8_t> fw(0x100, 0x00);
    fw[0] = 0x80; fw[1] = 0xFE;
    const size_t vec = irq.vector_addr;
    if (vec + 1 < fw.size()) {
        fw[vec] = 0x80;
        fw[vec + 1] = 0xFE;
    }

    const Trace a = run(model, irq, fw, false, "interp");
    const Trace b = run(model, irq, fw, true, "jit");

    std::printf("interp irq=%u time=%llu pc=0x%x now=%llu\n", a.irq_count,
                (unsigned long long)a.irq_time, a.pc,
                (unsigned long long)a.now);
    std::printf("jit    irq=%u time=%llu pc=0x%x now=%llu\n", b.irq_count,
                (unsigned long long)b.irq_time, b.pc,
                (unsigned long long)b.now);

    if (a.irq_count != b.irq_count || a.irq_time != b.irq_time ||
        a.irq_number != b.irq_number || a.pc != b.pc || a.now != b.now) {
        std::puts("jit timing mismatch");
        return 1;
    }
    std::puts("jit timing ok");
    return 0;
}
