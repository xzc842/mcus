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
#include "jit.h"
#include <atomic>
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mcus {

class Cpu : public MemoryBus {
public:
    Cpu(const Model& model, SharedMemory& shm, EventStream& stream);
    ~Cpu();
    VirtTime step();

    // 加载 JIT 插件；失败返回 false，之后继续用解释器。
    bool enable_jit(const std::string& plugin_path);
    bool jit_enabled() const { return jit_ && jit_->loaded(); }
    // 连续执行最多 max_steps 条指令；返回实际执行数。
    // 只用于当前 PC 已被 Translator 支持的快速路径，否则返回 0。
    // yield 在每条指令的 Check 之后调用；返回 true 表示让出控制权。
    using JitYieldFn = bool (*)(void*);
    uint64_t run_jit(uint64_t max_steps, JitYieldFn yield = nullptr,
                     void* yield_user = nullptr);
    // JIT 每指令 Check 回调：执行现有完整检查，返回 false 表示退出 JIT。
    bool run_full_checks();
    void sync_jit_regs(const CpuRegisters& regs) { regs_ = regs; }
    void sync_jit_pc(uint32_t pc) { regs_.pc = pc; }
    // 供 JIT 的 CallIsa 回退执行一条解释器指令，返回消耗的虚拟周期。
    VirtTime execute_isa_one();
    bool halted() const { return halted_.load(std::memory_order_relaxed); }
    bool paused() const { return paused_.load(std::memory_order_relaxed); }
    void set_paused(bool p) { paused_.store(p, std::memory_order_relaxed); }
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

    uint8_t fetch8(Addr addr) override;
    uint16_t fetch16(Addr addr) override;
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
    struct FastBlock {
        MemBlock* block = nullptr;
        Addr base = 0;
        Addr mask = 0;
        Addr mask_base = 0;
        bool has_mask = false;
        bool read_only = false;
    };
    std::unordered_map<SpaceId, MemBlock> memory_;
    struct FastBlockEntry { SpaceId space; FastBlock block; };
    std::vector<FastBlockEntry> fast_blocks_;
    FastBlock* code_fast_ = nullptr;

    FastBlock* find_fast(const SpaceId& space) {
        for (auto& e : fast_blocks_)
            if (e.space == space) return &e.block;
        return nullptr;
    }

    ControlBlock* ctl_ = nullptr;
    PinState* pins_ = nullptr;
    uint8_t* pin_hold_ = nullptr;
    uint8_t* irq_bitmap_ = nullptr;
    size_t irq_bitmap_size_ = 0;
    InjectHeader* inject_header_ = nullptr;
    InjectSlot* inject_slots_ = nullptr;
    uint32_t inject_capacity_ = 0;
    BreakpointHeader* breakpoint_header_ = nullptr;
    BreakpointSlot* breakpoint_slots_ = nullptr;

    bool has_global_irq_enable_ = false;
    SpaceId global_irq_space_;
    Addr global_irq_reg_ = 0;
    uint8_t global_irq_bit_ = 0;
    size_t global_irq_off_ = SIZE_MAX;
    uint8_t global_irq_width_ = 8;
    bool global_irq_invert_ = false;

    struct IrqRuntime {
        const InterruptDesc* desc = nullptr;
        size_t pending_off = SIZE_MAX;
        size_t enable_off = SIZE_MAX;
        uint8_t pending_width = 8;
        uint8_t enable_width = 8;
        uint32_t pending_mask = 0;
        uint32_t enable_mask = 0;
    };
    std::vector<IrqRuntime> irq_rt_;
    struct IrqGroup {
        size_t enable_off = SIZE_MAX;
        uint8_t enable_width = 8;
        std::vector<const IrqRuntime*> irqs;
    };
    std::vector<IrqGroup> irq_groups_;

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
                    uint32_t direction, bool force = false);
    void inject_pulse(size_t pin, uint32_t width, uint32_t level);
    void service_pulses();
    void sync_pins_for_register(const SpaceId& space, Addr addr,
                                uint32_t value);
    void sync_pin_mux(const SpaceId& space, Addr addr, uint32_t value);
    void on_pin_change(size_t index, uint32_t value);
    void handle_external_pin_change(size_t index, uint32_t value);
    bool pin_width(size_t index, uint32_t& width);
    void update_wave(size_t index);
    void start_wave(size_t index, uint32_t value);
    void stop_wave(size_t index);
    uint32_t read_reg_value(const SpaceId& space, Addr addr) const;
    void write_reg_value(const SpaceId& space, Addr addr, uint32_t value);

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
    std::vector<VirtTime> pin_last_edge_;
    std::vector<uint32_t> last_pin_value_;
    std::vector<uint32_t> last_pin_function_;
    std::vector<uint32_t> last_pin_direction_;
    uint32_t pin_poll_skip_ = 0;
    uint32_t pin_poll_interval_ = 1;
    uint32_t pin_slow_counter_ = 0;
    uint32_t last_reported_pc_ = 0xFFFFFFFF;

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
    struct I2cRuntime {
        bool busy = false;
        uint32_t xfer_id = 0;
    };
    struct CanRuntime {
        bool busy = false;
    };
    struct EncoderRuntime {
        bool initialized = false;
        uint32_t last_ab = 0;
        int64_t count = 0;
    };
    struct CaptureRuntime {
        bool armed = false;
        VirtTime start = 0;
    };
    struct PulseIn {
        uint32_t width = 0;
        uint32_t level = 0;   // 脉冲起始电平
    };
    struct PulseRuntime {
        std::deque<PulseIn> queue;
        bool active = false;
        VirtTime end = 0;
        uint32_t start_level = 0;
    };
    struct WaveRuntime {
        bool active = false;
        uint32_t level = 0;
        VirtTime next_toggle = 0;
        uint32_t high = 1;
        uint32_t low = 1;
        uint64_t remaining = 0;
        bool finish_after_low = false;
    };
    std::vector<UartRuntime> uart_rt_;
    std::vector<SpiRuntime> spi_rt_;
    std::vector<uint8_t> usb_tx_pending_;
    std::vector<PwmRuntime> pwm_rt_;
    std::vector<AdcRuntime> adc_rt_;
    std::vector<TkeyRuntime> tkey_rt_;
    std::vector<I2cRuntime> i2c_rt_;
    std::vector<CanRuntime> can_rt_;
    std::vector<EncoderRuntime> encoder_rt_;
    std::vector<CaptureRuntime> capture_rt_;
    std::vector<WaveRuntime> wave_rt_;
    std::vector<PulseRuntime> pulse_rt_;
    // 只有排队/激活的引脚才参与 service_pulses 扫描，空闲时每步零开销。
    std::vector<uint32_t> pulse_pins_;
    std::vector<uint8_t> pulse_queued_;
    std::unordered_map<uint32_t, uint32_t> adc_stream_samples_;
    uint32_t next_bus_xfer_id_ = 1;
    uint32_t periph_active_ = 0; // 1=UART TX, 2=SPI, 4=PWM, 8=ADC, 16=TKEY, 128=GPIO WAVE, 256=PULSE IN

    uint32_t last_bp_pc_ = 0xFFFFFFFF;

    std::unique_ptr<JitEngine> jit_;
    std::unordered_map<Addr, std::unique_ptr<NativeProgram>> jit_cache_;
    std::unordered_map<Addr, uint32_t> jit_hits_;
};

} // namespace mcus