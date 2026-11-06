/*
 * Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include "types.h"
#include "model.h"
#include "firmware.h"
#include "shared_memory.h"
#include "event_stream.h"
#include "instruction.h"
#include <atomic>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mcus {

struct CpuRegisters {
    uint32_t r[32]{};
    uint32_t pc = 0;
    uint32_t sp = 0;
    uint32_t flags = 0;
};

class Cpu : public MemoryBus {
public:
    Cpu(const Model& model, SharedMemory& shm, EventStream& stream);
    VirtTime step();
    bool halted() const { return halted_.load(); }
    bool paused() const { return paused_.load(); }
    void set_paused(bool p) { paused_.store(p); }
    const CpuRegisters& regs() const { return regs_; }
    uint32_t pc() const { return regs_.pc; }

    void load_firmware(const FirmwareImage& firmware);
    void load_firmware(const std::vector<uint8_t>& firmware);
    void reset();
    size_t export_memory(const SpaceId& space, Addr addr, size_t len, uint8_t* dst) const;

    // 推进 UART/SPI/PWM/ADC/TKEY/USB 的虚拟时间。
    // Simulator 会在每条指令后调用；直接使用 Cpu 的测试也可以手动调用。
    void advance_peripherals(VirtTime delta);
    VirtTime now() const { return now_; }

    // SPI 等同步总线正在等待外部响应时返回 true；此时 CPU 不应继续执行。
    bool waiting_for_bus() const;
    // 超时兜底：以默认 MISO(0xFF) 完成所有挂起传输。
    void force_complete_spi();

    uint8_t read8(const SpaceId& space, Addr addr) override;
    uint16_t read16(const SpaceId& space, Addr addr) override;
    uint32_t read32(const SpaceId& space, Addr addr) override;
    void write8(const SpaceId& space, Addr addr, uint8_t v) override;
    void write16(const SpaceId& space, Addr addr, uint16_t v) override;
    void write32(const SpaceId& space, Addr addr, uint32_t v) override;
    void on_reti() override;

private:
    const Model& model_;
    SharedMemory& shm_;
    EventStream& stream_;
    IsaPtr isa_;
    CpuRegisters regs_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> halted_{false};

    struct MemBlock { Addr base; std::vector<uint8_t> data; };
    std::unordered_map<SpaceId, MemBlock> memory_;
    std::unordered_map<SpaceId, MemBlock*> fast_blocks_;

    bool has_global_irq_enable_ = false;
    SpaceId global_irq_space_;
    Addr global_irq_reg_ = 0;
    uint8_t global_irq_bit_ = 0;

    // 中断服务状态：进入 ISR 时压入，RETI 时弹出。
    // 默认策略：同号/同级中断在服务期间屏蔽；需要抢占时由模型 priority 决定。
    std::vector<uint32_t> irq_in_service_;
    std::vector<uint32_t> irq_service_priority_;

    size_t mem_offset(const SpaceId& space, Addr addr) const;
    void handle_mmio_write(const SpaceId& space, Addr addr, uint32_t v, uint8_t w);
    uint32_t handle_mmio_read(const SpaceId& space, Addr addr, uint8_t w);
    bool check_interrupt();
    bool check_breakpoint();
    void enter_interrupt(const InterruptDesc& irq);
    void check_reset_pin();
    void check_inject_slots();

    void emit_mmio_event(const SpaceId& space, Addr addr, uint32_t value,
                         uint8_t width);
    void update_pin(size_t index, uint32_t value, uint32_t function,
                    uint32_t direction);

    // ---- 外设 / 总线事件 ----
    void poll_bus_in();
    void poll_external_pins();
    void on_peripheral_mmio_write(const SpaceId& space, Addr addr,
                                  uint32_t value, uint8_t width);
    uint32_t on_peripheral_mmio_read(const SpaceId& space, Addr addr,
                                     uint32_t value, uint8_t width);

    void emit_bus_event(uint32_t bus_id, BusEventKind kind, BusDirection dir,
                        uint32_t xfer_id, const uint8_t* data, uint16_t len,
                        uint8_t flags = 0);
    void emit_usb_event(uint32_t xfer_id, UsbToken token, uint8_t ep,
                        UsbHandshake hs, const uint8_t* data, uint16_t len);
    void set_irq_pending(uint32_t number);
    uint32_t read_reg8(const SpaceId& space, Addr addr) const;
    void write_reg8(const SpaceId& space, Addr addr, uint32_t value);
    void raise_usb_int_fg(uint32_t bits);
    uint32_t read_usb_byte(uint8_t ep, uint32_t offset);
    void write_usb_byte(uint8_t ep, uint32_t offset, uint8_t value);
    void complete_spi(size_t index);
    void complete_adc(size_t index);
    void complete_tkey(size_t index);
    void drive_pwm_pin(const PwmPinDesc& pin, uint32_t value);
    uint32_t function_index(const PinDesc& pin, const std::string& name) const;

    VirtTime now_ = 0;
    std::vector<uint32_t> last_pin_value_;
    std::vector<uint32_t> last_pin_function_;
    std::vector<uint32_t> last_pin_direction_;

    struct UartRuntime {
        std::deque<uint8_t> rx;
        bool tx_pending = false;
        VirtTime tx_ready = 0;
    };
    struct SpiRuntime {
        bool busy = false;
        uint8_t tx = 0;
        uint8_t rx = 0xFF;
        bool responded = false;
        uint32_t xfer_id = 0;
        VirtTime ready_at = 0;
    };
    struct PwmRuntime {
        bool started = false;
        uint64_t acc = 0;
        uint32_t counter = 0;
    };
    struct AdcRuntime {
        bool busy = false;
        uint8_t channel = 0;
        VirtTime ready_at = 0;
    };
    struct TkeyRuntime {
        bool busy = false;
        VirtTime ready_at = 0;
    };
    std::vector<UartRuntime> uart_rt_;
    std::vector<SpiRuntime> spi_rt_;
    std::vector<uint8_t> usb_tx_pending_;
    std::vector<PwmRuntime> pwm_rt_;
    std::vector<AdcRuntime> adc_rt_;
    std::vector<TkeyRuntime> tkey_rt_;
    uint32_t next_bus_xfer_id_ = 1;

    uint32_t last_bp_pc_ = 0xFFFFFFFF;
};

} // namespace mcus