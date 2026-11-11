// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0

#include "mcus/cpu.h"
#include "mcus/event_stream.h"
#include "mcus/model.h"
#include "mcus/shared_memory.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

using namespace mcus;

namespace {
constexpr Addr kMmio = 0x40000000;
bool pop_kind(EventStream& s, BusEventKind kind, BusEvent& out) {
    DebugEvent ev{};
    while (s.pop(ev)) {
        if (ev.type == DebugEventType::Bus && ev.u.bus.kind ==
                static_cast<uint8_t>(kind)) {
            out = ev.u.bus;
            return true;
        }
    }
    return false;
}
bool pop_pin(EventStream& s, uint32_t pin, PinEvent& out) {
    DebugEvent ev{};
    while (s.pop(ev)) {
        if (ev.type == DebugEventType::Pin && ev.u.pin.pin_id == pin) {
            out = ev.u.pin;
            return true;
        }
    }
    return false;
}
void push_bus(EventStream& s, BusEventKind kind, BusDirection dir,
              uint32_t bus_id, const uint8_t* data, uint16_t len,
              uint8_t flags = 0) {
    BusEvent b{};
    b.bus_id = bus_id;
    b.xfer_id = 1;
    b.kind = static_cast<uint8_t>(kind);
    b.direction = static_cast<uint8_t>(dir);
    b.len = len;
    b.flags = flags;
    std::memcpy(b.data, data, len);
    DebugEvent ev{};
    ev.type = DebugEventType::Bus;
    ev.u.bus = b;
    assert(s.push_in(ev));
}
} // namespace

int main() {
    Model model = Model::load_from_file(MCUS_TEST_INTERFACE_MODEL_PATH);
    const std::string shm_name = "/mcus_test_if_" + std::to_string(::getpid());
    const std::string stream_name = shm_name + "_s";
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());

    {
        SharedMemory shm(model, shm_name);
        EventStream stream(stream_name, 128);
        Cpu cpu(model, shm, stream);
        BusEvent b{};

        // 引脚复用：MUX 每 2 bit 一个引脚。
        const uint32_t mux = (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6) |
                             (1u << 8) | (2u << 10) | (1u << 12) | (1u << 14);
        cpu.write32("mmio", kMmio + 0x00, mux);
        assert(shm.pins()[0].function.load() == 1);
        assert(shm.pins()[5].function.load() == 2);
        assert(shm.pins()[7].function.load() == 1);

        // 注入区：外部写 MMIO，模拟器在下一条指令前消费。
        {
            auto* ih = shm.inject_header();
            auto* slots = shm.inject_slots();
            const uint32_t tail = ih->tail.load();
            auto& slot = slots[tail % shm.inject_capacity()];
            slot.addr.store(kMmio + 0x34);
            slot.value.store(0x123);
            slot.width.store(16);
            slot.space_idx.store(0);
            slot.ready.store(1);
            ih->tail.store(tail + 1);
            cpu.step();
            assert(shm.read_reg("mmio", kMmio + 0x34, 16) == 0x123);
        }

        // I2C：start -> data -> host response -> stop。
        cpu.write8("mmio", kMmio + 0x10, 1);
        assert(pop_kind(stream, BusEventKind::I2cStart, b));
        cpu.write8("mmio", kMmio + 0x11, 0x55);
        assert(pop_kind(stream, BusEventKind::I2cData, b));
        assert(b.data[0] == 0x55);
        uint8_t i2c_resp = 0x5A;
        push_bus(stream, BusEventKind::I2cData, BusDirection::ToMcu, 0,
                 &i2c_resp, 1);
        cpu.step();
        assert(shm.read_reg("mmio", kMmio + 0x11, 8) == 0x5A);
        assert(shm.read_reg("mmio", kMmio + 0x12, 8) & (1u << 3)); // done
        cpu.write8("mmio", kMmio + 0x10, 2); // stop
        assert(pop_kind(stream, BusEventKind::I2cStop, b));

        // CAN：id + 4 字节 payload。
        cpu.write32("mmio", kMmio + 0x22, 0x123);
        cpu.write8("mmio", kMmio + 0x2A, 8);
        cpu.write32("mmio", kMmio + 0x2E, 0x11223344);
        cpu.write32("mmio", kMmio + 0x26, 0xAABBCCDD);
        assert(pop_kind(stream, BusEventKind::CanFrame, b));
        assert(b.len == 12);
        uint32_t id = 0;
        std::memcpy(&id, b.data, 4);
        assert(id == 0x123);
        assert(b.data[4] == 0xDD && b.data[5] == 0xCC &&
               b.data[6] == 0xBB && b.data[7] == 0xAA);
        assert(b.data[8] == 0x44 && b.data[9] == 0x33 &&
               b.data[10] == 0x22 && b.data[11] == 0x11);
        uint8_t can_resp[8];
        uint32_t rid = 0x456;
        std::memcpy(can_resp, &rid, 4);
        can_resp[4] = 1; can_resp[5] = 2;
        push_bus(stream, BusEventKind::CanFrame, BusDirection::ToMcu, 0,
                 can_resp, 6);
        cpu.step();
        assert(shm.read_reg("mmio", kMmio + 0x22, 32) == 0x456);
        assert(shm.read_reg("mmio", kMmio + 0x2A, 8) == 2);
        assert(shm.read_reg("mmio", kMmio + 0x21, 8) & (1u << 1));

        // DAC 输出事件。
        cpu.write16("mmio", kMmio + 0x34, 0x345);
        assert(pop_kind(stream, BusEventKind::DacWrite, b));
        uint32_t dac = 0;
        std::memcpy(&dac, b.data, 4);
        assert(dac == 0x345);

        // 正交编码器：A/B 相各走一步。
        auto* pins = shm.pins();
        pins[0].value.store(1, std::memory_order_relaxed);
        cpu.step();
        pins[1].value.store(1, std::memory_order_relaxed);
        cpu.step();
        pins[0].value.store(0, std::memory_order_relaxed);
        cpu.step();
        pins[1].value.store(0, std::memory_order_relaxed);
        cpu.step();
        assert(shm.read_reg("mmio", kMmio + 0x40, 32) == 4);
        assert(pop_kind(stream, BusEventKind::EncoderDelta, b));

        // 任意 GPIO 都能通过 PinEvent 读取脉宽；PA6 不在 captures 数组里。
        pins[6].value.store(1, std::memory_order_relaxed);
        cpu.step();
        cpu.advance_peripherals(50);
        pins[6].value.store(0, std::memory_order_relaxed);
        cpu.step();
        PinEvent pev{};
        bool got_width = false;
        while (pop_pin(stream, 6, pev)) {
            if (pev.changed & 8u) {
                got_width = true;
                break;
            }
        }
        assert(got_width);
        assert(pev.pulse_width >= 50);

        // 外部程序通过事件直接注入一个指定宽度的脉冲。
        uint8_t pulse_data[4];
        uint32_t injected_width = 70;
        std::memcpy(pulse_data, &injected_width, 4);
        push_bus(stream, BusEventKind::PulseWidth, BusDirection::ToMcu, 2,
                 pulse_data, 4, 1); // pin=PA2, high pulse
        cpu.step();
        cpu.advance_peripherals(injected_width);
        const uint32_t cap_width =
            shm.read_reg("mmio", kMmio + 0x50, 32);
        assert(cap_width >= injected_width);
        assert(pop_kind(stream, BusEventKind::PwmCapture, b));

        // 传统方式：直接驱动 GPIO 电平，同样能测宽。
        pins[2].value.store(1, std::memory_order_relaxed);
        cpu.step();
        cpu.advance_peripherals(100);
        pins[2].value.store(0, std::memory_order_relaxed);
        cpu.step();
        const uint32_t width = shm.read_reg("mmio", kMmio + 0x50, 32);
        assert(width >= 100);
        assert(pop_kind(stream, BusEventKind::PwmCapture, b));

        // GPIO 波形输出：4 高 / 6 低。
        cpu.write32("mmio", kMmio + 0x64, 10);
        cpu.write32("mmio", kMmio + 0x68, 4);
        cpu.write32("mmio", kMmio + 0x60, 1);
        assert(pins[3].value.load() == 1);
        cpu.advance_peripherals(4);
        assert(pins[3].value.load() == 0);
        cpu.advance_peripherals(6);
        assert(pins[3].value.load() == 1);
        cpu.write32("mmio", kMmio + 0x60, 2);
        assert(pins[3].value.load() == 0);

        // ADC 数据流采样。
        cpu.write32("mmio", kMmio + 0x74, 0); // channel 0
        cpu.write32("mmio", kMmio + 0x70, 1); // start
        uint8_t adc_sample[4];
        uint32_t av = 0x321;
        std::memcpy(adc_sample, &av, 4);
        push_bus(stream, BusEventKind::AdcSample, BusDirection::ToMcu, 0,
                 adc_sample, 4, 0);
        cpu.step();
        cpu.advance_peripherals(10);
        assert(shm.read_reg("mmio", kMmio + 0x78, 32) == 0x321);
    }

    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    std::puts("interfaces ok");
    return 0;
}
