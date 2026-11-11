// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "mcus/model.h"
#include "mcus/shared_memory.h"
#include "mcus/event_stream.h"
#include "mcus/cpu.h"
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"
#include <sys/mman.h>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
using namespace mcus;
extern "C" InstructionSet* mcus_isa_create(const Model*);
extern "C" void mcus_isa_destroy(InstructionSet*);
static void install_arm() {
    IsaRegistry::instance().register_isa("arm", [](const Model& m) -> IsaPtr {
        return IsaPtr(mcus_isa_create(&m), IsaDeleter{mcus_isa_destroy});
    });
}
static bool pop_bus(EventStream& s, BusEventKind kind, BusEvent& out) {
    DebugEvent ev{};
    while (s.pop(ev)) {
        if (ev.type == DebugEventType::Bus &&
            ev.u.bus.kind == static_cast<uint8_t>(kind)) {
            out = ev.u.bus;
            return true;
        }
    }
    return false;
}
int main() {
    install_arm();
    Model m = Model::load_from_file(MCUS_TEST_ARM_MODEL_PATH);
    const char* sn = "/mcus_test_stm32p_shm";
    const char* stn = "/mcus_test_stm32p_stream";
    ::shm_unlink(sn); ::shm_unlink(stn);
    {
        SharedMemory shm(m, sn);
        EventStream stream(stn, 32);
        Cpu cpu(m, shm, stream);
        std::vector<uint8_t> fw(0x100, 0);
        fw[0] = 0x00; fw[1] = 0x50; fw[2] = 0x00; fw[3] = 0x20; // SP
        fw[4] = 0x41; fw[5] = 0x00; fw[6] = 0x00; fw[7] = 0x08; // reset
        fw[0x40] = 0xFE; fw[0x41] = 0xE7; // b .
        FirmwareImage img; img.load_addr = 0x08000000; img.data = fw;
        cpu.load_firmware(img);
        assert(cpu.regs().sp == 0x20005000u);

        // USART1 TX / RX
        cpu.write32("periph", 0x40013804, 0x55);
        BusEvent tx{};
        assert(pop_bus(stream, BusEventKind::UartTx, tx));
        assert(tx.bus_id == 0 && tx.len == 1 && tx.data[0] == 0x55);
        DebugEvent rx{};
        rx.type = DebugEventType::Bus;
        rx.u.bus.kind = static_cast<uint8_t>(BusEventKind::UartRx);
        rx.u.bus.direction = static_cast<uint8_t>(BusDirection::ToMcu);
        rx.u.bus.bus_id = 0;
        rx.u.bus.len = 1;
        rx.u.bus.data[0] = 0x42;
        assert(stream.push_in(rx));
        cpu.step();
        assert(cpu.read32("periph", 0x40013804) == 0x42);

        // SPI1 request / response
        cpu.write32("periph", 0x4001300c, 0x99);
        BusEvent req{};
        assert(pop_bus(stream, BusEventKind::SpiXfer, req));
        assert(req.direction == static_cast<uint8_t>(BusDirection::ToHost));
        DebugEvent resp{};
        resp.type = DebugEventType::Bus;
        resp.u.bus.kind = static_cast<uint8_t>(BusEventKind::SpiXfer);
        resp.u.bus.direction = static_cast<uint8_t>(BusDirection::ToMcu);
        resp.u.bus.bus_id = 0;
        resp.u.bus.xfer_id = req.xfer_id;
        resp.u.bus.len = 1;
        resp.u.bus.data[0] = 0x3C;
        assert(stream.push_in(resp));
        cpu.step();
        cpu.advance_peripherals(8);
        assert(cpu.read32("periph", 0x4001300c) == 0x3C);

        // ADC1：注入样本并完成转换
        cpu.write32("periph", 0x40012408, 1u << 22);
        DebugEvent adc{};
        adc.type = DebugEventType::Bus;
        adc.u.bus.kind = static_cast<uint8_t>(BusEventKind::AdcSample);
        adc.u.bus.direction = static_cast<uint8_t>(BusDirection::ToMcu);
        uint32_t sample = 0x321;
        std::memcpy(adc.u.bus.data, &sample, 4);
        adc.u.bus.len = 4;
        adc.u.bus.flags = 0;
        assert(stream.push_in(adc));
        cpu.step();
        cpu.advance_peripherals(96);
        assert((cpu.read32("periph", 0x4001244c) & 0xFFFu) == 0x321);

        // CAN1 请求/响应事件
        cpu.write32("periph", 0x40006580, 0x123);
        cpu.write32("periph", 0x40006584, 8);
        cpu.write32("periph", 0x40006588, 0x11223344);
        cpu.write32("periph", 0x4000658c, 0xAABBCCDD);
        cpu.write32("periph", 0x40006400, 1);
        BusEvent can{};
        assert(pop_bus(stream, BusEventKind::CanFrame, can));
        assert(can.len == 12);
        uint32_t can_id = 0;
        std::memcpy(&can_id, can.data, 4);
        assert(can_id == 0x123);
        uint8_t can_resp[8] = {0x56, 0x04, 0, 0, 1, 2, 0, 0};
        assert(stream.push_in([&] {
            DebugEvent ev{};
            ev.type = DebugEventType::Bus;
            ev.u.bus.kind = static_cast<uint8_t>(BusEventKind::CanFrame);
            ev.u.bus.direction = static_cast<uint8_t>(BusDirection::ToMcu);
            ev.u.bus.len = 6;
            std::memcpy(ev.u.bus.data, can_resp, 6);
            return ev;
        }()));
        cpu.step();
        assert(cpu.read32("periph", 0x40006580) == 0x456);
        assert(cpu.read32("periph", 0x40006408) & 1u); // done flag

        std::cout << "test_stm32_peripherals passed\n";
    }
    ::shm_unlink(sn); ::shm_unlink(stn);
    return 0;
}
