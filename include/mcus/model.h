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
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>

namespace mcus {

struct MemoryRegion {
    std::string name;
    SpaceId space;
    Addr base = 0;
    uint32_t size = 0;
    bool is_mmio = false;
    bool read_only = false;   // 普通内存区域的写保护（如 Flash/ROM）
    Addr addr_mask = 0;
    bool contains(Addr addr) const {
        return addr >= base && (addr - base) < size;
    }
};

// 寄存器读写后的通用动作（由模型描述，核心执行）
struct RegisterAction {
    enum class Kind {
        SetBit,
        ClearBit,
        SetValue,
        ClearValue,
    };

    Kind kind = Kind::SetBit;
    SpaceId space;
    Addr addr = 0;
    uint8_t bit = 0;
    uint32_t value = 0;
};

struct RegisterDesc {
    std::string name;
    SpaceId space;
    Addr addr = 0;
    uint8_t width = 32;
    RegSemantics semantics = RegSemantics::Plain;
    uint32_t reset_value = 0;
    std::string read_alias;
    std::string write_alias;
    std::vector<std::pair<Addr, uint8_t>> bit_aliases;
    std::vector<RegisterAction> on_write;
    std::vector<RegisterAction> on_read;
};

struct PinDesc {
    std::string name;
    uint32_t number = 0;
    std::vector<std::string> functions;
    uint32_t reset_function = 0;
    SpaceId source_space;
    Addr source_reg = 0;
    uint8_t source_bit = 0;

    // 方向控制寄存器（可选）：写该寄存器会更新 PinState.direction
    bool has_direction = false;
    SpaceId direction_space;
    Addr direction_reg = 0;
    uint8_t direction_bit = 0;
};

struct InterruptDesc {
    std::string name;
    uint32_t number = 0;
    uint32_t priority = 0;          // 数值越小优先级越高；默认 0（同级不嵌套）
    Addr vector_addr = 0;
    SpaceId vector_space;
    Addr enable_reg = 0;
    SpaceId enable_space;
    uint8_t enable_bit = 0;
    Addr pending_reg = 0;
    SpaceId pending_space;
    uint8_t pending_bit = 0;
    bool clear_pending = true;
};

struct CounterDesc {
    uint8_t bits = 16;
    Addr lo_reg = 0;
    Addr hi_reg = 0;
    SpaceId reg_space;
    Addr reload_lo_reg = 0;
    Addr reload_hi_reg = 0;
    bool up = true;
};

struct TimerDesc {
    std::string name;
    CounterDesc counter;
    struct CtrlBit { Addr reg; uint8_t bit; SpaceId space; } enable;
    struct FlagBit { Addr reg; uint8_t bit; SpaceId space; } overflow;
    uint32_t irq = 0;
    std::string overflow_action = "reload";
    uint32_t prescaler = 1;         // 每 N 个虚拟周期计数一次
};

// 串口：发送走 BusEvent(UartTx)，接收由外部 push_in(BusEvent(UartRx)) 注入。
struct UartDesc {
    std::string name;
    uint32_t irq = 0;
    SpaceId space;
    Addr ctrl = 0;              // 状态/控制寄存器（发送/接收标志所在）
    Addr data = 0;              // 数据寄存器
    uint8_t tx_flag_bit = 1;
    uint8_t rx_flag_bit = 0;
    uint32_t tx_cycles = 0;     // 一个字节的虚拟发送时间；0=立即置发送标志
    uint32_t rx_fifo = 8;       // 接收 FIFO 深度
};

// SPI 主机：写 data 发 SpiXfer(ToHost)，外部回 SpiXfer(ToMcu) 后置 SPIF。
struct SpiDesc {
    std::string name;
    uint32_t irq = 0;
    SpaceId space;
    Addr data = 0;
    Addr stat = 0;
    Addr ctrl = 0;
    uint8_t done_bit = 4;
    uint8_t busy_bit = 7;
    uint32_t xfer_cycles = 8;
};

struct PwmPinDesc {
    uint8_t channel = 1;        // 1 或 2
    uint32_t pin = 0;           // Model::pins 索引
    std::string function;       // 该通道对应的引脚功能名，如 "pwm1"
};

struct PwmDesc {
    std::string name;
    uint32_t irq = 0;
    SpaceId space;
    Addr ctrl = 0;
    Addr data1 = 0;
    Addr data2 = 0;
    Addr ck_se = 0;
    uint8_t if_bit = 4;
    uint8_t enable_bit1 = 0;
    uint8_t enable_bit2 = 1;
    uint32_t period = 256;
    uint32_t prescaler = 1;
    std::vector<PwmPinDesc> pins;
};

struct AdcDesc {
    std::string name;
    uint32_t irq = 0;
    SpaceId space;
    Addr ctrl = 0;
    Addr cfg = 0;
    Addr data = 0;
    uint8_t start_bit = 0;
    uint8_t if_bit = 5;
    uint8_t busy_bit = 4;
    uint32_t conversion_cycles = 96;
    uint32_t channel_mask = 0xFFFFFFFFu;  // cfg 寄存器里的通道字段掩码
    uint8_t channel_shift = 0;            // cfg 寄存器里的通道字段起始位
    std::vector<uint32_t> channel_pins;   // 通道号 -> Model::pins 索引
};

struct TkeyDesc {
    std::string name;
    uint32_t irq = 0;
    SpaceId space;
    Addr ctrl = 0;
    Addr datl = 0;
    Addr dath = 0;
    uint8_t start_bit = 0;
    uint8_t if_bit = 7;
    uint32_t scan_cycles = 256;
    uint8_t data_bits = 16;               // 扫描结果位数
    std::vector<uint32_t> channel_pins;   // 通道号 -> Model::pins 索引
};

struct GpioIrqDesc {
    bool present = false;
    uint32_t irq = 0;
    SpaceId enable_space;
    Addr enable_reg = 0;
    uint8_t enable_bit_base = 0;
    uint8_t enable_bits = 8;
};

struct UsbEndpointDesc {
    uint8_t index = 0;
    SpaceId ctrl_space;
    Addr ctrl = 0;
    uint16_t max_packet = 64;
    Addr len_reg = 0;           // 可选：发送长度寄存器
    SpaceId len_space;
    uint32_t buffer_offset = 0;    // RX 相对 usb.buffer_base 的偏移
    uint32_t tx_buffer_offset = 0; // TX 相对 RX 的偏移（CH552 EP2 为 64）
};

struct UsbDesc {
    bool present = false;
    uint32_t irq = 0;
    SpaceId space;
    Addr int_fg = 0;
    Addr int_st = 0;
    Addr mis_st = 0;
    Addr rx_len = 0;
    Addr int_en = 0;
    Addr ctrl = 0;
    Addr dev_ad = 0;
    uint8_t int_fg_bit = 1;
    uint8_t tog_ok_bit = 6;       // USB_INT_FG 里的 U_TOG_OK 位；0=不处理
    uint8_t bus_reset_bit = 0;    // USB_INT_FG 里的 UIF_BUS_RST 位
    SpaceId buffer_space;
    Addr buffer_base = 0;
    uint32_t buffer_stride = 64;
    std::vector<UsbEndpointDesc> endpoints;
};

// 架构级描述：换芯片只改这里，不改 Cpu/Simulator
struct ArchDesc {
    // 全局中断使能（8051 是 IE bit7，ARM 是 PRIMASK，RISC-V 是 mstatus.MIE）
    struct GlobalIrqEnable {
        SpaceId space;
        Addr reg = 0;
        uint8_t bit = 0;
        bool present = false;
    } global_irq_enable;

    // 中断压栈宽度（8051 压 2 字节 PC，ARM 压 4/8 字节，RISC-V 压 4 字节）
    uint8_t irq_push_bytes = 0;

    // 栈增长方向：true = 向低地址（ARM/RISC-V），false = 向高地址（8051）
    bool stack_grows_down = false;

    // 快速空间（code/iram/xram，别的架构 flash/ram/eeprom）
    std::vector<SpaceId> fast_spaces;

    // 复位向量空间/地址
    SpaceId reset_space;
    Addr reset_vector = 0;

    // 栈空间/初始 SP
    SpaceId sp_space;
    Addr initial_sp = 0;
    Addr sp_reg = 0;             // SP 寄存器地址，由模型描述
    SpaceId sp_reg_space;

    // PC 寄存器（如果架构有外部 PC 寄存器，8051 没有，用 regs_.pc）
    bool has_pc_reg = false;
    SpaceId pc_reg_space;
    Addr pc_reg_lo = 0;
    Addr pc_reg_hi = 0;
};

struct Model {
    std::string name;
    std::string description;
    std::string arch = "mcus-minimal";
    uint32_t crystal_hz = 0;
    uint32_t cycles_per_instruction = 1;
    Addr reset_vector = 0;
    SpaceId reset_space;
    Addr initial_sp = 0;
    SpaceId sp_space;

    std::vector<MemoryRegion> regions;
    std::vector<RegisterDesc> registers;
    std::vector<PinDesc> pins;
    std::vector<InterruptDesc> interrupts;
    std::vector<TimerDesc> timers;
    std::vector<UartDesc> uarts;
    std::vector<SpiDesc> spis;
    std::vector<PwmDesc> pwms;
    std::vector<AdcDesc> adcs;
    std::vector<TkeyDesc> tkeys;
    GpioIrqDesc gpio_irq;
    UsbDesc usb;
    std::vector<std::string> isa_extensions;
    std::vector<SpaceId> space_order;

    ArchDesc arch_desc;

    std::unordered_map<std::string, size_t> reg_index;

    static Model load_from_file(const std::string& path);
    void validate() const;

    const MemoryRegion* find_region(const SpaceId& space, Addr addr) const;
    const RegisterDesc* find_register(const SpaceId& space, Addr addr) const;
    const RegisterDesc* find_register_by_name(const std::string& name) const;
    const InterruptDesc* find_interrupt(uint32_t number) const;

    size_t mmio_area_size() const;
    size_t pin_area_size() const;
    size_t irq_bitmap_size() const;

    int space_index(const SpaceId& space) const {
        for (size_t i = 0; i < space_order.size(); ++i)
            if (space_order[i] == space) return static_cast<int>(i);
        return -1;
    }

    static std::string reg_key(const SpaceId& space, Addr addr) {
        return space + ":" + std::to_string(addr);
    }
};

} // namespace mcus