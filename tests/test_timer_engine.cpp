// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "mcus/model.h"
#include "mcus/config.h"
#include "mcus/simulator.h"
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
using namespace mcus;
extern "C" InstructionSet* mcus_isa_create(const Model*);
extern "C" void mcus_isa_destroy(InstructionSet*);
static void install_isa(const char* arch) {
    IsaRegistry::instance().register_isa(arch, [](const Model& m) -> IsaPtr {
        return IsaPtr(mcus_isa_create(&m), IsaDeleter{mcus_isa_destroy});
    });
}
#ifndef MCUS_TEST_CH552G_MODEL_PATH
#define MCUS_TEST_CH552G_MODEL_PATH ""
#endif
#ifndef MCUS_TEST_ARM_MODEL_PATH
#define MCUS_TEST_ARM_MODEL_PATH ""
#endif

static void test_ch552() {
    Model model = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);
    Config cfg;
    cfg.simulator.shm_name = "/mcus_timer_engine_ch552_" + std::to_string(::getpid());
    cfg.simulator.stream_name = cfg.simulator.shm_name + "_s";
    cfg.simulator.start_paused = true;
    cfg.crystal.frequency_hz = 24000000;
    cfg.crystal.cycles_per_instruction = 1;
    cfg.apply_to_model(model);
    ::shm_unlink(cfg.simulator.shm_name.c_str());
    ::shm_unlink(cfg.simulator.stream_name.c_str());
    Simulator sim(model, FirmwareImage{}, cfg);
    auto& shm = sim.shm_for_test();
    for (size_t i = 0; i < 3; ++i) {
        const auto& t = model.timers[i];
        shm.write_reg(t.counter.reg_space, t.enable.reg,
                      static_cast<uint32_t>(1u << t.enable.bit), 8);
        shm.write_reg(t.counter.reg_space, t.counter.lo_reg, 0, 8);
        if (t.counter.hi_reg) shm.write_reg(t.counter.reg_space, t.counter.hi_reg, 0, 8);
        sim.advance_clock_for_test(1200);
        const uint32_t lo = shm.read_reg(t.counter.reg_space, t.counter.lo_reg, 8);
        assert(lo != 0);
        // 12T 预分频在模型里显式配置；这里只验证计数确实推进。
        shm.write_reg(t.counter.reg_space, t.enable.reg, 0, 8);
    }
    // Timer0/1/2 溢出必须置中断 pending。
    for (size_t i = 0; i < 3; ++i) {
        const auto& t = model.timers[i];
        shm.write_reg(t.counter.reg_space, t.counter.lo_reg, 0xFF, 8);
        shm.write_reg(t.counter.reg_space, t.counter.hi_reg, 0xFF, 8);
        shm.write_reg(t.counter.reg_space, t.enable.reg,
                      static_cast<uint32_t>(1u << t.enable.bit), 8);
        sim.advance_clock_for_test(1);
        const InterruptDesc* irq = model.find_interrupt(t.irq);
        assert(irq);
        const uint8_t pw = 8;
        const uint32_t pending =
            shm.read_reg(irq->pending_space, irq->pending_reg, pw);
        assert(pending & (1u << irq->pending_bit));
        shm.write_reg(t.counter.reg_space, t.enable.reg, 0, 8);
    }

    // WDOG 递减到 0 后必须请求复位。
    const auto& w = model.timers[3];
    shm.write_reg(w.counter.reg_space, w.counter.lo_reg, 5, 8);
    shm.write_reg(w.counter.reg_space, w.enable.reg,
                  static_cast<uint32_t>(1u << w.enable.bit), 8);
    sim.advance_clock_for_test(16);
    assert(shm.control()->reset_pin.load() != 0);
    ::shm_unlink(cfg.simulator.shm_name.c_str());
    ::shm_unlink(cfg.simulator.stream_name.c_str());
    std::puts("test_timer_engine ch552 passed");
}
#ifdef MCUS_TEST_ARM
static void test_stm32() {
    Model model = Model::load_from_file(MCUS_TEST_ARM_MODEL_PATH);
    Config cfg;
    cfg.simulator.shm_name = "/mcus_timer_engine_arm_" + std::to_string(::getpid());
    cfg.simulator.stream_name = cfg.simulator.shm_name + "_s";
    cfg.simulator.start_paused = true;
    cfg.crystal.frequency_hz = 72000000;
    cfg.crystal.cycles_per_instruction = 1;
    cfg.apply_to_model(model);
    ::shm_unlink(cfg.simulator.shm_name.c_str());
    ::shm_unlink(cfg.simulator.stream_name.c_str());
    Simulator sim(model, FirmwareImage{}, cfg);
    auto& shm = sim.shm_for_test();
    for (size_t ti = 0; ti < 3; ++ti) { // TIM2/3/4
        const auto& t = model.timers[ti];
        shm.write_reg(t.counter.reg_space, t.prescaler_reg, 0, 32);
        shm.write_reg(t.counter.reg_space, t.counter.reload_lo_reg, 100, 32);
        shm.write_reg(t.counter.reg_space, t.counter.lo_reg, 0, 32);
        shm.write_reg(t.counter.reg_space, t.enable.reg,
                      static_cast<uint32_t>(1u << t.enable.bit), 32);
        sim.advance_clock_for_test(50);
        assert(shm.read_reg(t.counter.reg_space, t.counter.lo_reg, 32) == 50);
        sim.advance_clock_for_test(60);
        // auto-reload 后计数器回到 ARR。
        assert(shm.read_reg(t.counter.reg_space, t.counter.lo_reg, 32) == 100);
        assert(shm.read_reg(t.counter.reg_space, t.overflow.reg, 32) &
               (1u << t.overflow.bit));
        shm.write_reg(t.counter.reg_space, t.enable.reg, 0, 32);
    }

    // SysTick：reload=100，推进 100 周期后置 COUNTFLAG 并重装。
    {
        const auto& st = model.timers[3];
        shm.write_reg(st.counter.reg_space, st.counter.reload_lo_reg, 100, 32);
        shm.write_reg(st.counter.reg_space, st.counter.lo_reg, 0, 32);
        shm.write_reg(st.counter.reg_space, st.enable.reg,
                      static_cast<uint32_t>(1u << st.enable.bit), 32);
        sim.advance_clock_for_test(100);
        assert(shm.read_reg(st.counter.reg_space, st.overflow.reg, 32) &
               (1u << st.overflow.bit));
        shm.write_reg(st.counter.reg_space, st.enable.reg, 0, 32);
    }
    ::shm_unlink(cfg.simulator.shm_name.c_str());
    ::shm_unlink(cfg.simulator.stream_name.c_str());
    std::puts("test_timer_engine stm32 passed");
}
#endif

int main() {
#ifdef MCUS_TEST_ARM
    install_isa("arm");
    test_stm32();
#else
    install_isa("8051");
    test_ch552();
#endif
    return 0;
}
