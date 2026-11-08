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

// src/isa_base.cpp
//
// InstructionSet 的默认架构行为。具体架构（8051/ARM/RISC-V）可以覆盖。

#include "mcus/instruction.h"
#include "mcus/cpu.h"

namespace mcus {

void InstructionSet::reset(CpuRegisters& regs, MemoryBus& bus,
                           const Model& model) {
    (void)bus;
    regs.pc = model.arch_desc.reset_vector;
    regs.sp = model.arch_desc.initial_sp;
}

void InstructionSet::save_context(CpuRegisters& regs, MemoryBus& bus,
                                  const Model& model) {
    const uint8_t bytes = model.arch_desc.irq_push_bytes;
    if (bytes == 0) return;

    Addr sp = regs.sp;
    for (uint8_t i = 0; i < bytes; ++i) {
        if (model.arch_desc.stack_grows_down) {
            --sp;
        } else {
            ++sp;
        }
        bus.write8(model.arch_desc.sp_space, sp,
                   static_cast<uint8_t>((regs.pc >> (8 * i)) & 0xFF));
    }
    regs.sp = sp;
}

void InstructionSet::restore_context(CpuRegisters& regs, MemoryBus& bus,
                                     const Model& model) {
    (void)regs;
    (void)bus;
    (void)model;
}

bool InstructionSet::resolve_bit_addr(const Model& model, Addr bit_addr,
                                      SpaceId& space, Addr& byte_addr,
                                      uint8_t& bit) const {
    (void)model;
    (void)bit_addr;
    (void)space;
    (void)byte_addr;
    (void)bit;
    return false;
}

Addr InstructionSet::interrupt_vector(const InterruptDesc& irq, MemoryBus& bus,
                                      const Model& model) const {
    (void)bus;
    (void)model;
    return irq.vector_addr;
}

void InstructionSet::acknowledge_interrupt(const InterruptDesc& irq,
                                           MemoryBus& bus,
                                           const Model& model) {
    const RegisterDesc* reg =
        model.find_register(irq.pending_space, irq.pending_reg);
    const uint8_t width = reg ? reg->width : 8;

    uint32_t value = 0;
    switch (width) {
        case 8: value = bus.read8(irq.pending_space, irq.pending_reg); break;
        case 16: value = bus.read16(irq.pending_space, irq.pending_reg); break;
        default: value = bus.read32(irq.pending_space, irq.pending_reg); break;
    }
    const uint32_t clear_mask = irq.pending_mask
                                    ? irq.pending_mask
                                    : (1u << irq.pending_bit);
    value &= ~clear_mask;
    switch (width) {
        case 8:
            bus.write8(irq.pending_space, irq.pending_reg,
                       static_cast<uint8_t>(value));
            break;
        case 16:
            bus.write16(irq.pending_space, irq.pending_reg,
                        static_cast<uint16_t>(value));
            break;
        default:
            bus.write32(irq.pending_space, irq.pending_reg, value);
            break;
    }
}

void InstructionSet::on_mmio_write(const Model& model, MemoryBus& bus,
                                   const SpaceId& space, Addr addr,
                                   uint32_t value, uint8_t width) {
    (void)model;
    (void)bus;
    (void)space;
    (void)addr;
    (void)value;
    (void)width;
}

void InstructionSet::on_mmio_read(const Model& model, MemoryBus& bus,
                                  const SpaceId& space, Addr addr,
                                  uint8_t width) {
    (void)model;
    (void)bus;
    (void)space;
    (void)addr;
    (void)width;
}

bool InstructionSet::higher_priority(const InterruptDesc& a,
                                     const InterruptDesc& b) const {
    if (a.priority != b.priority) return a.priority < b.priority;
    return a.number < b.number;
}

} // namespace mcus
