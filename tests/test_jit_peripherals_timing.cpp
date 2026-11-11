// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0
//
// 8051 外设事件时序差分：GPIO/PWM/SPI/UART RX/ADC/TKEY/USB。
// 解释器与 JIT 使用同一固件和同一注入事件，比较事件序列与关键寄存器。

#include "mcus/cpu.h"
#include "mcus/event_stream.h"
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"
#include "mcus/model.h"
#include "mcus/shared_memory.h"

#include <sys/mman.h>
#include <cstdio>
#include <string>
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

struct Result {
    std::vector<std::string> events;
    std::vector<uint32_t> regs;
};

static void format_event(const DebugEvent& e, std::string& s) {
    char buf[256];
    switch (e.type) {
        case DebugEventType::Pin:
            std::snprintf(buf, sizeof(buf), "P %llu %u %u %u %u",
                          (unsigned long long)e.time, e.u.pin.pin_id,
                          e.u.pin.value, e.u.pin.changed, e.u.pin.pulse_width);
            break;
        case DebugEventType::Bus:
            std::snprintf(buf, sizeof(buf), "B %llu %u %u %u %u %02x%02x",
                          (unsigned long long)e.time, e.u.bus.kind,
                          e.u.bus.direction, e.u.bus.bus_id, e.u.bus.len,
                          e.u.bus.data[0], e.u.bus.data[1]);
            break;
        case DebugEventType::Irq:
            std::snprintf(buf, sizeof(buf), "I %llu %u",
                          (unsigned long long)e.time, e.u.irq.number);
            break;
        case DebugEventType::Usb:
            std::snprintf(buf, sizeof(buf), "U %llu %u %u %u",
                          (unsigned long long)e.time, e.u.usb.token,
                          e.u.usb.ep, e.u.usb.len);
            break;
        default:
            std::snprintf(buf, sizeof(buf), "M %llu %u %u",
                          (unsigned long long)e.time, e.u.mmio.addr,
                          e.u.mmio.value);
            break;
    }
    s = buf;
}

static Result run(const Model& model, bool jit, const char* suffix,
                  const std::vector<uint8_t>& fw,
                  const std::vector<DebugEvent>& inject, int steps,
                  const std::vector<std::pair<SpaceId, Addr>>& regs) {
    const std::string shm_name = std::string("/mcus_jit_periph_") + suffix;
    const std::string stream_name = shm_name + "_s";
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    Result out;
    {
        SharedMemory shm(model, shm_name);
        EventStream stream(stream_name, 512);
        Cpu cpu(model, shm, stream);
        cpu.load_firmware(fw);
        if (jit && !cpu.enable_jit(MCUS_TEST_JIT8051_PATH)) {
            std::puts("jit enable failed");
            return out;
        }
        for (const auto& ev : inject) {
            if (!stream.push_in(ev)) {
                std::puts("push_in failed");
                return out;
            }
        }

        if (jit) {
            uint64_t done = 0;
            while (done < static_cast<uint64_t>(steps)) {
                const uint64_t n = cpu.run_jit(steps - done);
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
            for (int i = 0; i < steps; ++i) {
                const VirtTime dt = cpu.step();
                if (dt == 0) break;
                cpu.advance_peripherals(dt);
            }
        }

        DebugEvent ev{};
        while (stream.pop(ev)) {
            std::string s;
            format_event(ev, s);
            out.events.push_back(std::move(s));
        }
        for (const auto& [space, addr] : regs)
            out.regs.push_back(cpu.read8(space, addr));
    }
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    return out;
}

static int check(const Model& model, const char* name,
                 const std::vector<uint8_t>& fw,
                 const std::vector<DebugEvent>& inject, int steps,
                 const std::vector<std::pair<SpaceId, Addr>>& regs) {
    const Result a = run(model, false, (std::string(name) + "i").c_str(),
                         fw, inject, steps, regs);
    const Result b = run(model, true, (std::string(name) + "j").c_str(),
                         fw, inject, steps, regs);
    std::printf("%s: interp events=%zu regs=", name, a.events.size());
    for (auto v : a.regs) std::printf("%02x,", v);
    std::printf(" jit events=%zu regs=", b.events.size());
    for (auto v : b.regs) std::printf("%02x,", v);
    std::printf("\n");
    if (a.events != b.events || a.regs != b.regs) {
        std::printf("%s timing mismatch\n", name);
        for (size_t i = 0; i < a.events.size(); ++i)
            std::printf("  I[%zu] %s\n", i, a.events[i].c_str());
        for (size_t i = 0; i < b.events.size(); ++i)
            std::printf("  J[%zu] %s\n", i, b.events[i].c_str());
        return 1;
    }
    return 0;
}

int main() {
    install_8051();
    const Model m = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);
    int failures = 0;

    // GPIO output（P1）
    failures += check(m, "gpio",
                      {0x75, 0x90, 0x01, 0x75, 0x90, 0x00, 0x80, 0xFE},
                      {}, 2, {});

    // PWM1 output（P1.5）
    failures += check(m, "pwm",
                      {0x75, 0x9C, 0x80, 0x75, 0x9D, 0x01, 0x80, 0xFE},
                      {}, 8, {});

    // SPI0 TX 请求
    failures += check(m, "spi",
                      {0x75, 0xF9, 0xAA, 0x80, 0xFE}, {}, 2, {});

    // UART0 RX：注入 1 字节，检查 RX 标志
    {
        DebugEvent rx{};
        rx.type = DebugEventType::Bus;
        rx.u.bus.kind = static_cast<uint8_t>(BusEventKind::UartRx);
        rx.u.bus.direction = static_cast<uint8_t>(BusDirection::ToMcu);
        rx.u.bus.bus_id = 0;
        rx.u.bus.len = 1;
        rx.u.bus.data[0] = 0x5A;
        failures += check(m, "uart_rx", {0x80, 0xFE}, {rx}, 1,
                          {{"sfr", 0x98}});
    }

    // ADC：注入采样值，检查转换结果
    {
        DebugEvent adc{};
        adc.type = DebugEventType::Bus;
        adc.u.bus.kind = static_cast<uint8_t>(BusEventKind::AdcSample);
        adc.u.bus.direction = static_cast<uint8_t>(BusDirection::ToMcu);
        adc.u.bus.bus_id = 0;
        adc.u.bus.len = 4;
        const uint32_t sample = 0x321;
        std::memcpy(adc.u.bus.data, &sample, 4);
        failures += check(m, "adc", {0x75, 0x80, 0x01, 0x80, 0xFE},
                          {adc}, 100, {{"sfr", 0x9F}});
    }

    // TKEY 扫描完成
    failures += check(m, "tkey", {0x75, 0xC3, 0x01, 0x80, 0xFE}, {},
                      300, {{"sfr", 0xC4}, {"sfr", 0xC5}});

    // USB SETUP 包
    {
        DebugEvent usb{};
        usb.type = DebugEventType::Usb;
        usb.u.usb.token = static_cast<uint8_t>(UsbToken::Setup);
        usb.u.usb.ep = 0;
        usb.u.usb.len = 8;
        const uint8_t setup[8] = {0x80, 0x06, 0x00, 0x01,
                                  0x00, 0x00, 0x40, 0x00};
        std::memcpy(usb.u.usb.data, setup, 8);
        failures += check(m, "usb", {0x80, 0xFE}, {usb}, 1,
                          {{"sfr", 0xD9}, {"sfr", 0xDB}});
    }

    if (failures) {
        std::printf("%d peripheral timing tests failed\n", failures);
        return 1;
    }
    std::puts("jit peripheral timing ok");
    return 0;
}
