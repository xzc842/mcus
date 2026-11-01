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
    Addr addr_mask = 0;
    bool contains(Addr addr) const {
        return addr >= base && (addr - base) < size;
    }
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
};

struct PinDesc {
    std::string name;
    uint32_t number = 0;
    std::vector<std::string> functions;
    uint32_t reset_function = 0;
    SpaceId source_space;
    Addr source_reg = 0;
    uint8_t source_bit = 0;
};

struct InterruptDesc {
    std::string name;
    uint32_t number = 0;
    Addr vector_addr = 0;
    SpaceId vector_space = "code";
    Addr enable_reg = 0;
    SpaceId enable_space = "sfr";
    uint8_t enable_bit = 0;
    Addr pending_reg = 0;
    SpaceId pending_space = "sfr";
    uint8_t pending_bit = 0;
    bool clear_pending = true;
};

struct CounterDesc {
    uint8_t bits = 16;
    Addr lo_reg = 0;
    Addr hi_reg = 0;
    SpaceId reg_space = "sfr";
    Addr reload_lo_reg = 0;
    Addr reload_hi_reg = 0;
    bool up = true;
};

struct TimerDesc {
    std::string name;
    CounterDesc counter;
    struct CtrlBit { Addr reg; uint8_t bit; SpaceId space = "sfr"; } enable;
    struct FlagBit { Addr reg; uint8_t bit; SpaceId space = "sfr"; } overflow;
    uint32_t irq = 0;
    std::string overflow_action = "reload";
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
    uint8_t irq_push_bytes = 2;

    // 快速空间（code/iram/xram，别的架构 flash/ram/eeprom）
    std::vector<SpaceId> fast_spaces;

    // 复位向量空间/地址
    SpaceId reset_space = "code";
    Addr reset_vector = 0;

    // 栈空间/初始 SP
    SpaceId sp_space = "iram";
    Addr initial_sp = 0x07;
    Addr sp_reg = 0x81;          // SP 在 sp_space 里的地址（8051 是 sfr 0x81）
    SpaceId sp_reg_space = "sfr";

    // PC 寄存器（如果架构有外部 PC 寄存器，8051 没有，用 regs_.pc）
    bool has_pc_reg = false;
    SpaceId pc_reg_space = "sfr";
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
    SpaceId reset_space = "code";
    Addr initial_sp = 0;
    SpaceId sp_space = "iram";

    std::vector<MemoryRegion> regions;
    std::vector<RegisterDesc> registers;
    std::vector<PinDesc> pins;
    std::vector<InterruptDesc> interrupts;
    std::vector<TimerDesc> timers;
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
    bool resolve_bit_addr(Addr bit_addr, SpaceId& space,
                          Addr& byte_addr, uint8_t& bit) const;

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