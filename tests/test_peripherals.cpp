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
#include "mcus/shared_memory.h"
#include "mcus/event_stream.h"
#include "mcus/cpu.h"
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"

#include <sys/mman.h>
#include <cassert>
#include <iostream>
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

static void cleanup(const char* n) { ::shm_unlink(n); }

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

static bool pop_usb(EventStream& s, UsbToken token, UsbEvent& out) {
    DebugEvent ev{};
    while (s.pop(ev)) {
        if (ev.type == DebugEventType::Usb &&
            ev.u.usb.token == static_cast<uint8_t>(token)) {
            out = ev.u.usb;
            return true;
        }
    }
    return false;
}

int main() {
    install_8051();

    Model m = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);
    const char* shm_name = "/mcus_test_periph_shm";
    const char* stream_name = "/mcus_test_periph_stream";
    cleanup(shm_name);
    cleanup(stream_name);

    {
        SharedMemory shm(m, shm_name);
        EventStream stream(stream_name, 64);

        // 固件：0x80 0xFE = SJMP $
        std::vector<uint8_t> fw(0x200, 0x00);
        fw[0] = 0x80;
        fw[1] = 0xFE;
        Cpu cpu(m, shm, stream);
        cpu.load_firmware(fw);
        PinState* pins = shm.pins();

        // xRAM 的 addr_mask 边界：16 位访问跨 0x3FF -> 0 时要逐字节回绕。
        cpu.write8("xram", 0x3FF, 0xAA);
        cpu.write8("xram", 0x000, 0xBB);
        assert(cpu.read16("xram", 0x3FF) == 0xBBAA);

        // ---- UART0 TX ----
        cpu.write8("sfr", 0x98, 0x50);       // SCON: mode 1
        cpu.write8("sfr", 0x99, 0x55);       // SBUF
        BusEvent tx{};
        assert(pop_bus(stream, BusEventKind::UartTx, tx));
        assert(tx.bus_id == 0 && tx.len == 1 && tx.data[0] == 0x55);
        assert((cpu.read8("sfr", 0x98) & 0x02) == 0); // TI 还没置
        cpu.advance_peripherals(64);
        assert(cpu.read8("sfr", 0x98) & 0x02);        // TI
        cpu.write8("sfr", 0x98, cpu.read8("sfr", 0x98) & ~0x02);

        // ---- UART0 RX ----
        DebugEvent rx{};
        rx.type = DebugEventType::Bus;
        rx.u.bus.kind = static_cast<uint8_t>(BusEventKind::UartRx);
        rx.u.bus.direction = static_cast<uint8_t>(BusDirection::ToMcu);
        rx.u.bus.bus_id = 0;
        rx.u.bus.len = 1;
        rx.u.bus.data[0] = 0x42;
        assert(stream.push_in(rx));
        cpu.step();
        assert(cpu.read8("sfr", 0x98) & 0x01);        // RI
        assert(cpu.read8("sfr", 0x99) == 0x42);       // 读 SBUF 取走数据
        assert((cpu.read8("sfr", 0x98) & 0x01) == 0); // RI 清

        // ---- UART1 TX/RX ----
        cpu.write8("sfr", 0xC0, 0x50);
        cpu.write8("sfr", 0xC1, 0xA5);
        BusEvent tx1{};
        assert(pop_bus(stream, BusEventKind::UartTx, tx1));
        assert(tx1.bus_id == 1 && tx1.data[0] == 0xA5);
        cpu.advance_peripherals(64);
        assert(cpu.read8("sfr", 0xC0) & 0x02);
        DebugEvent rx1 = rx;
        rx1.u.bus.bus_id = 1;
        rx1.u.bus.data[0] = 0x5A;
        assert(stream.push_in(rx1));
        cpu.step();
        assert(cpu.read8("sfr", 0xC1) == 0x5A);

        // ---- SPI0 ----
        cpu.write8("sfr", 0xF9, 0x55);
        BusEvent req{};
        assert(pop_bus(stream, BusEventKind::SpiXfer, req));
        assert(req.direction == static_cast<uint8_t>(BusDirection::ToHost));
        assert(cpu.read8("sfr", 0xF8) & 0x80); // busy
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
        assert(cpu.read8("sfr", 0xF8) & 0x10);        // SPIF
        assert((cpu.read8("sfr", 0xF8) & 0x80) == 0); // not busy
        assert(cpu.read8("sfr", 0xF9) == 0x3C);

        // ---- SPI：外部程序 attach 时同步等待响应 ----
        {
            EventStream peer(stream_name, 0, false);
            assert(peer.host_attached());

            cpu.write8("sfr", 0xF9, 0x11);
            BusEvent wait_req{};
            assert(pop_bus(stream, BusEventKind::SpiXfer, wait_req));
            const VirtTime before = cpu.now();
            assert(cpu.step() == 0);             // 冻结等待
            assert(cpu.waiting_for_bus());
            assert(cpu.now() == before);

            DebugEvent wait_resp{};
            wait_resp.type = DebugEventType::Bus;
            wait_resp.u.bus.kind = static_cast<uint8_t>(BusEventKind::SpiXfer);
            wait_resp.u.bus.direction = static_cast<uint8_t>(BusDirection::ToMcu);
            wait_resp.u.bus.bus_id = 0;
            wait_resp.u.bus.xfer_id = wait_req.xfer_id;
            wait_resp.u.bus.len = 1;
            wait_resp.u.bus.data[0] = 0x22;
            assert(peer.push_in(wait_resp));

            assert(cpu.step() > 0);
            assert(!cpu.waiting_for_bus());
            assert(cpu.read8("sfr", 0xF9) == 0x22);
            assert(cpu.read8("sfr", 0xF8) & 0x10);
        }

        // ---- ADC ----
        pins[1].value.store(0x80); // P1.1 = AIN0
        cpu.write8("sfr", 0x9A, 0x00);
        cpu.write8("sfr", 0x80, 0x01); // start
        assert(cpu.read8("sfr", 0x80) & 0x10); // busy
        cpu.advance_peripherals(96);
        assert(cpu.read8("sfr", 0x80) & 0x20); // ADC IF
        assert(cpu.read8("sfr", 0x9F) == 0x80);

        // ---- TKEY ----
        pins[0].value.store(1);
        pins[1].value.store(0);
        pins[4].value.store(1);
        pins[10].value.store(0);
        cpu.write8("sfr", 0xC3, 0x01);
        cpu.advance_peripherals(256);
        assert(cpu.read8("sfr", 0xC3) & 0x80);
        assert(cpu.read8("sfr", 0xC4) == 0x05); // channel 0 + 2

        // ---- PWM ----
        cpu.write8("sfr", 0x9C, 128); // PWM_DATA1
        cpu.write8("sfr", 0x9B, 64);  // PWM_DATA2
        cpu.write8("sfr", 0x9D, 0x03); // enable PWM1/PWM2
        cpu.advance_peripherals(100);
        assert((cpu.read8("sfr", 0x9D) & 0x10) == 0);
        assert(pins[5].value.load() == 1); // P1.5 PWM1 high
        assert(pins[9].value.load() == 0); // P3.1 PWM2 low
        cpu.advance_peripherals(200);
        assert(cpu.read8("sfr", 0x9D) & 0x10); // PWM IF

        // ---- GPIO 输入采样（不依赖方向位） ----
        assert(pins[0].direction.load() == 1);
        pins[0].value.store(0);
        assert((cpu.read8("sfr", 0x90) & 0x01) == 0);
        pins[0].value.store(1);
        assert((cpu.read8("sfr", 0x90) & 0x01) == 1);

        // ---- GPIO 输入 / 边沿中断 ----
        // P1_DIR_PU bit0=0 -> P1.0 输入
        cpu.write8("sfr", 0x93, 0xFE);
        assert(pins[0].direction.load() == 0);
        cpu.write8("sfr", 0xC7, 0x01); // GPIO_IE bit0
        cpu.write8("sfr", 0xE8, 0x40); // IE_EX: GPIO
        cpu.write8("sfr", 0xA8, 0x80); // EA
        pins[0].value.store(0);        // 外部拉低
        cpu.step();                    // poll -> 边沿中断
        assert(cpu.read8("sfr", 0xC6) & 0x40); // GPIO pending
        assert(cpu.regs().pc == 99);            // GPIO 向量
        assert((cpu.read8("sfr", 0x90) & 0x01) == 0); // 读 P1 采样到低电平

        // ---- USB SETUP ----
        {
            DebugEvent setup{};
            setup.type = DebugEventType::Usb;
            setup.u.usb.token = static_cast<uint8_t>(UsbToken::Setup);
            setup.u.usb.ep = 0;
            setup.u.usb.len = 8;
            setup.u.usb.xfer_id = 76;
            const uint8_t setup_data[8] = {0x80, 0x06, 0x00, 0x01,
                                            0x00, 0x00, 0x40, 0x00};
            std::memcpy(setup.u.usb.data, setup_data, 8);
            assert(stream.push_in(setup));
            cpu.step();
            assert(cpu.read8("sfr", 0xDB) == 8);       // RX_LEN
            assert(cpu.read8("sfr", 0xD9) & 0x30);     // SETUP token bits
            assert(cpu.read8("xram", 0) == 0x80);
            assert(cpu.read8("xram", 7) == 0x00);
        }

        // ---- USB IN / OUT ----
        for (uint32_t i = 0; i < 4; ++i)
            cpu.write8("xram", i, static_cast<uint8_t>(0x10 + i));

        DebugEvent uin{};
        uin.type = DebugEventType::Usb;
        uin.u.usb.token = static_cast<uint8_t>(UsbToken::In);
        uin.u.usb.ep = 0;
        uin.u.usb.len = 4;
        uin.u.usb.xfer_id = 77;
        assert(stream.push_in(uin));
        cpu.step();
        UsbEvent uout{};
        assert(pop_usb(stream, UsbToken::In, uout));
        assert(uout.xfer_id == 77);
        assert(uout.handshake == static_cast<uint8_t>(UsbHandshake::Ack));
        assert(uout.len == 4 && uout.data[0] == 0x10 && uout.data[3] == 0x13);

        DebugEvent uout_ev{};
        uout_ev.type = DebugEventType::Usb;
        uout_ev.u.usb.token = static_cast<uint8_t>(UsbToken::Out);
        uout_ev.u.usb.ep = 0;
        uout_ev.u.usb.len = 2;
        uout_ev.u.usb.xfer_id = 78;
        uout_ev.u.usb.data[0] = 0xAA;
        uout_ev.u.usb.data[1] = 0xBB;
        assert(stream.push_in(uout_ev));
        cpu.step();
        assert(cpu.read8("sfr", 0xD8) & 0x02); // USB_INT_FG
        assert(cpu.read8("sfr", 0xDB) == 2);   // RX_LEN
        assert(cpu.read8("xram", 0) == 0xAA);
        assert(cpu.read8("xram", 1) == 0xBB);

        // USB EP2 TX buffer（EP2 buffer + 64）应通过 In 事件返回
        cpu.write8("xram", 84, 0x0D);
        cpu.write8("xram", 85, 0x0A);
        cpu.write8("sfr", 0xD5, 2); // UEP2_T_LEN
        DebugEvent txin{};
        txin.type = DebugEventType::Usb;
        txin.u.usb.token = static_cast<uint8_t>(UsbToken::In);
        txin.u.usb.ep = 2;
        txin.u.usb.len = 2;
        txin.u.usb.xfer_id = 79;
        assert(stream.push_in(txin));
        cpu.step();
        UsbEvent txout{};
        assert(pop_usb(stream, UsbToken::In, txout));
        assert(txout.xfer_id == 79 && txout.len == 2);
        assert(txout.data[0] == 0x0D && txout.data[1] == 0x0A);

        std::cout << "test_peripherals passed\n";
    }

    cleanup(shm_name);
    cleanup(stream_name);
    return 0;
}
