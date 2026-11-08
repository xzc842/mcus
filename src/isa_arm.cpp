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

// ARM Cortex-M3 (Thumb/Thumb-2) ISA 插件。
// 不是完整 ARMv7-M 实现，覆盖启动、GPIO/USART/SPI/定时器中断和常规 C 代码所需的指令。

#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"
#include "mcus/cpu.h"
#include "mcus/log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace mcus {
namespace {

constexpr uint32_t kN = 1u << 31;
constexpr uint32_t kZ = 1u << 30;
constexpr uint32_t kC = 1u << 29;
constexpr uint32_t kV = 1u << 28;
constexpr uint32_t kT = 1u << 24;
constexpr uint32_t kExcReturn = 0xFFFFFFF9u;

class IsaArm : public InstructionSet {
public:
    explicit IsaArm(const Model& model) : model_(model) {}

    ExecResult execute(CpuRegisters& regs, MemoryBus& bus) override {
        ExecResult res;
        res.cycles = 1;
        regs.flags |= kT;

        const Addr pc = regs.pc & ~1u;
        const uint16_t hw1 = fetch16(bus, pc);
        if (hw1 == 0xFFFF) {
            MCUS_WARN("ARM invalid opcode 0x" + hex16(hw1) + " at 0x" + hex32(pc));
            res.halted = true;
            return res;
        }

        if (is32bit(hw1)) {
            const uint16_t hw2 = fetch16(bus, pc + 2);
            if (!execute32(regs, bus, pc, hw1, hw2, res)) {
                MCUS_WARN("ARM Thumb-2 opcode unimplemented: " + hex16(hw1) +
                          " " + hex16(hw2) + " at 0x" + hex32(pc));
                res.halted = true;
            }
            return res;
        }

        if (!execute16(regs, bus, pc, hw1, res)) {
            MCUS_WARN("ARM Thumb opcode unimplemented: " + hex16(hw1) +
                      " at 0x" + hex32(pc));
            res.halted = true;
        }
        return res;
    }

    // ---- 架构钩子 ----

    void reset(CpuRegisters& regs, MemoryBus& bus, const Model& model) override {
        regs = CpuRegisters{};
        regs.flags = kT;
        const Addr vt = model.arch_desc.reset_vector;
        const uint32_t sp = read32(bus, vt);
        const uint32_t entry = read32(bus, vt + 4) & ~1u;
        regs.sp = sp ? sp : model.arch_desc.initial_sp;
        regs.r[13] = regs.sp;
        regs.pc = entry;
        regs.r[14] = 0xFFFFFFFFu;
        regs.r[15] = entry;
    }

    void save_context(CpuRegisters& regs, MemoryBus& bus,
                      const Model& model) override {
        (void)model;
        uint32_t sp = regs.sp - 32;
        const uint32_t frame[8] = {
            regs.r[0], regs.r[1], regs.r[2], regs.r[3],
            regs.r[12], regs.r[14], regs.pc, regs.flags};
        for (int i = 0; i < 8; ++i)
            write32(bus, sp + static_cast<Addr>(i * 4), frame[i]);
        regs.sp = sp;
        regs.r[13] = sp;
        regs.r[14] = kExcReturn;
    }

    void restore_context(CpuRegisters& regs, MemoryBus& bus,
                         const Model& model) override {
        (void)model;
        const uint32_t sp = regs.sp;
        regs.r[0] = read32(bus, sp + 0);
        regs.r[1] = read32(bus, sp + 4);
        regs.r[2] = read32(bus, sp + 8);
        regs.r[3] = read32(bus, sp + 12);
        regs.r[12] = read32(bus, sp + 16);
        regs.r[14] = read32(bus, sp + 20);
        regs.pc = read32(bus, sp + 24) & ~1u;
        regs.flags = read32(bus, sp + 28) | kT;
        regs.sp = sp + 32;
        regs.r[13] = regs.sp;
        regs.r[15] = regs.pc;
    }

    Addr interrupt_vector(const InterruptDesc& irq, MemoryBus& bus,
                          const Model& model) const override {
        (void)model;
        // irq.vector_addr 是向量表表项地址；ARM 需要再取表项里的 handler 地址。
        return read32(bus, irq.vector_addr) & ~1u;
    }

    void acknowledge_interrupt(const InterruptDesc& irq, MemoryBus& bus,
                               const Model& model) override {
        const RegisterDesc* reg =
            model.find_register(irq.pending_space, irq.pending_reg);
        if (!reg) return;
        const uint32_t mask = irq.pending_mask
                                  ? irq.pending_mask
                                  : (1u << irq.pending_bit);
        if (reg->semantics == RegSemantics::WriteOneClear) {
            write_reg(bus, *reg, mask);
        } else {
            uint32_t v = read_reg(bus, *reg) & ~mask;
            write_reg(bus, *reg, v);
        }
    }

    void on_mmio_write(const Model& model, MemoryBus& bus,
                       const SpaceId& space, Addr addr, uint32_t value,
                       uint8_t width) override {
        (void)width;
        // NVIC ICER/ICPR 是写 1 清零；同步到 ISER/ISPR。
        const RegisterDesc* r = model.find_register(space, addr);
        if (!r) return;
        auto clear_bits = [&](const char* iser_name) {
            const RegisterDesc* iser = model.find_register_by_name(iser_name);
            if (!iser || iser->space != space) return;
            uint32_t v = read_reg(bus, *iser);
            v &= ~value;
            write_reg(bus, *iser, v);
        };
        if (r->name == "NVIC_ICER0") clear_bits("NVIC_ISER0");
        else if (r->name == "NVIC_ICER1") clear_bits("NVIC_ISER1");
        else if (r->name == "NVIC_ICPR0") clear_bits("NVIC_ISPR0");
        else if (r->name == "NVIC_ICPR1") clear_bits("NVIC_ISPR1");
    }

private:
    const Model& model_;

    // ---- 地址空间与内存 ----

    const MemoryRegion* region_for(Addr addr) const {
        for (const auto& r : model_.regions)
            if (r.contains(addr)) return &r;
        return nullptr;
    }

    uint8_t read8(MemoryBus& bus, Addr addr) const {
        const auto* r = region_for(addr);
        return r ? bus.read8(r->space, addr) : 0;
    }
    uint16_t read16(MemoryBus& bus, Addr addr) const {
        const auto* r = region_for(addr);
        return r ? bus.read16(r->space, addr) : 0;
    }
    uint32_t read32(MemoryBus& bus, Addr addr) const {
        const auto* r = region_for(addr);
        return r ? bus.read32(r->space, addr) : 0;
    }
    void write8(MemoryBus& bus, Addr addr, uint8_t v) const {
        const auto* r = region_for(addr);
        if (r) bus.write8(r->space, addr, v);
    }
    void write16(MemoryBus& bus, Addr addr, uint16_t v) const {
        const auto* r = region_for(addr);
        if (r) bus.write16(r->space, addr, v);
    }
    void write32(MemoryBus& bus, Addr addr, uint32_t v) const {
        const auto* r = region_for(addr);
        if (r) bus.write32(r->space, addr, v);
    }
    uint16_t fetch16(MemoryBus& bus, Addr addr) const {
        const auto* r = region_for(addr);
        return r ? bus.read16(r->space, addr) : 0xFFFF;
    }

    static uint32_t read_reg(MemoryBus& bus, const RegisterDesc& r) {
        switch (r.width) {
            case 8: return bus.read8(r.space, r.addr);
            case 16: return bus.read16(r.space, r.addr);
            default: return bus.read32(r.space, r.addr);
        }
    }
    static void write_reg(MemoryBus& bus, const RegisterDesc& r, uint32_t v) {
        switch (r.width) {
            case 8: bus.write8(r.space, r.addr, static_cast<uint8_t>(v)); break;
            case 16: bus.write16(r.space, r.addr, static_cast<uint16_t>(v)); break;
            default: bus.write32(r.space, r.addr, v); break;
        }
    }

    static bool is32bit(uint16_t hw) {
        // 11101/11110/11111 开头为 32-bit Thumb-2；0xE000-0xE7FF 是 16-bit B。
        return hw >= 0xE800;
    }

    static std::string hex16(uint16_t v) {
        char b[8];
        std::snprintf(b, sizeof(b), "%04X", v);
        return b;
    }
    static std::string hex32(uint32_t v) {
        char b[12];
        std::snprintf(b, sizeof(b), "%08X", v);
        return b;
    }

    // ---- 寄存器 ----

    static uint32_t get_r(const CpuRegisters& r, uint32_t n) {
        if (n == 15) return r.pc;
        if (n == 13) return r.sp;
        return r.r[n & 0x1F];
    }
    static void set_r(CpuRegisters& r, uint32_t n, uint32_t v) {
        if (n == 15) { r.pc = v & ~1u; r.r[15] = r.pc; }
        else if (n == 13) { r.sp = v; r.r[13] = v; }
        else r.r[n & 0x1F] = v;
    }

    static bool cond_passed(uint32_t cond, uint32_t flags) {
        switch (cond & 0xF) {
            case 0: return (flags & kZ) != 0;                 // EQ
            case 1: return (flags & kZ) == 0;                 // NE
            case 2: return (flags & kC) != 0;                 // CS
            case 3: return (flags & kC) == 0;                 // CC
            case 4: return (flags & kN) != 0;                 // MI
            case 5: return (flags & kN) == 0;                 // PL
            case 6: return (flags & kV) != 0;                 // VS
            case 7: return (flags & kV) == 0;                 // VC
            case 8: return ((flags & kC) != 0) && ((flags & kZ) == 0); // HI
            case 9: return ((flags & kC) == 0) || ((flags & kZ) != 0); // LS
            case 10: return ((flags & kN) != 0) == ((flags & kV) != 0); // GE
            case 11: return ((flags & kN) != 0) != ((flags & kV) != 0); // LT
            case 12: return ((flags & kZ) == 0) &&
                            (((flags & kN) != 0) == ((flags & kV) != 0)); // GT
            case 13: return ((flags & kZ) != 0) ||
                            (((flags & kN) != 0) != ((flags & kV) != 0)); // LE
            default: return true;
        }
    }

    static void set_nz(CpuRegisters& r, uint32_t v, uint32_t sign_bit) {
        r.flags &= ~(kN | kZ);
        if (v & sign_bit) r.flags |= kN;
        if (v == 0) r.flags |= kZ;
    }

    static uint32_t add_flags(CpuRegisters& r, uint32_t a, uint32_t b,
                              uint32_t carry) {
        uint64_t u = static_cast<uint64_t>(a) + b + carry;
        uint32_t res = static_cast<uint32_t>(u);
        r.flags &= ~(kN | kZ | kC | kV);
        if (u > 0xFFFFFFFFu) r.flags |= kC;
        if (res & 0x80000000u) r.flags |= kN;
        if (res == 0) r.flags |= kZ;
        if ((~(a ^ b) & (a ^ res) & 0x80000000u) != 0) r.flags |= kV;
        return res;
    }

    static uint32_t sub_flags(CpuRegisters& r, uint32_t a, uint32_t b,
                              uint32_t borrow) {
        uint64_t u = static_cast<uint64_t>(a) - b - borrow;
        uint32_t res = static_cast<uint32_t>(u);
        r.flags &= ~(kN | kZ | kC | kV);
        if (a >= b + borrow) r.flags |= kC;
        if (res & 0x80000000u) r.flags |= kN;
        if (res == 0) r.flags |= kZ;
        if (((a ^ b) & (a ^ res) & 0x80000000u) != 0) r.flags |= kV;
        return res;
    }

    static uint32_t shift_c(uint32_t value, int type, uint32_t amount,
                            bool& carry) {
        if (amount == 0) return value;
        switch (type) {
            case 0: // LSL
                if (amount >= 32) { carry = amount == 32 ? (value & 1) : false; return 0; }
                carry = ((value >> (32 - amount)) & 1) != 0;
                return value << amount;
            case 1: // LSR
                if (amount >= 32) { carry = amount == 32 ? ((value >> 31) & 1) : false; return 0; }
                carry = ((value >> (amount - 1)) & 1) != 0;
                return value >> amount;
            case 2: // ASR
                if (amount >= 32) { carry = (value >> 31) & 1; return (value & 0x80000000u) ? 0xFFFFFFFFu : 0; }
                carry = ((value >> (amount - 1)) & 1) != 0;
                return static_cast<uint32_t>(static_cast<int32_t>(value) >> amount);
            default: // ROR
                amount &= 31;
                if (amount == 0) { carry = (value >> 31) & 1; return value; }
                carry = ((value >> (amount - 1)) & 1) != 0;
                return (value >> amount) | (value << (32 - amount));
        }
    }

    // ---- 16 位指令 ----

    bool execute16(CpuRegisters& regs, MemoryBus& bus, Addr pc, uint16_t hw,
                   ExecResult& res) {
        Addr next = pc + 2;

        // 000xx: shift immediate / add-sub
        if ((hw & 0xE000) == 0x0000) {
            const uint32_t op = (hw >> 11) & 0x3;
            const uint32_t imm5 = (hw >> 6) & 0x1F;
            const uint32_t rm = (hw >> 3) & 0x7;
            const uint32_t rd = hw & 0x7;
            if (op <= 2) {
                bool c = false;
                uint32_t v = shift_c(get_r(regs, rm), static_cast<int>(op), imm5, c);
                set_r(regs, rd, v);
                if (imm5 != 0) {
                    regs.flags &= ~(kN | kZ | kC);
                    if (c) regs.flags |= kC;
                    set_nz(regs, v, 0x80000000u);
                }
                regs.pc = next;
                return true;
            }
            // 00011: ADD/SUB register or imm3
            const uint32_t sub = (hw >> 9) & 1;
            const uint32_t is_imm = (hw >> 10) & 1;
            const uint32_t rn = (hw >> 3) & 0x7;
            const uint32_t rd2 = hw & 0x7;
            uint32_t b = is_imm ? ((hw >> 6) & 0x7) : get_r(regs, (hw >> 6) & 0x7);
            uint32_t a = get_r(regs, rn);
            if (is_imm) {
                // ADD/SUB imm3 不更新 flags
                set_r(regs, rd2, sub ? a - b : a + b);
            } else {
                set_r(regs, rd2, sub ? sub_flags(regs, a, b, 0)
                                     : add_flags(regs, a, b, 0));
            }
            regs.pc = next;
            return true;
        }

        // 001: MOV/CMP/ADD/SUB imm8
        if ((hw & 0xE000) == 0x2000) {
            const uint32_t op = (hw >> 11) & 0x3;
            const uint32_t rd = (hw >> 8) & 0x7;
            const uint32_t imm = hw & 0xFF;
            switch (op) {
                case 0: set_r(regs, rd, imm); set_nz(regs, imm, 0x80000000u); break;
                case 1: sub_flags(regs, get_r(regs, rd), imm, 0); break;
                case 2: set_r(regs, rd, add_flags(regs, get_r(regs, rd), imm, 0)); break;
                default: set_r(regs, rd, sub_flags(regs, get_r(regs, rd), imm, 0)); break;
            }
            regs.pc = next;
            return true;
        }

        // 010000: data processing
        if ((hw & 0xFC00) == 0x4000) {
            const uint32_t op = (hw >> 6) & 0xF;
            const uint32_t rm = hw & 0x7;
            const uint32_t rd = (hw >> 3) & 0x7;
            const uint32_t a = get_r(regs, rd);
            const uint32_t b = get_r(regs, rm);
            bool c = false;
            switch (op) {
                case 0x0: set_r(regs, rd, a & b); set_nz(regs, a & b, 0x80000000u); break;
                case 0x1: set_r(regs, rd, a ^ b); set_nz(regs, a ^ b, 0x80000000u); break;
                case 0x2: set_r(regs, rd, shift_c(a, 0, b & 0xFF, c));
                          regs.flags = (regs.flags & ~(kC | kN | kZ)) |
                                       (c ? kC : 0); set_nz(regs, get_r(regs, rd), 0x80000000u); break;
                case 0x3: set_r(regs, rd, shift_c(a, 1, b & 0xFF, c));
                          regs.flags = (regs.flags & ~(kC | kN | kZ)) |
                                       (c ? kC : 0); set_nz(regs, get_r(regs, rd), 0x80000000u); break;
                case 0x4: set_r(regs, rd, shift_c(a, 2, b & 0xFF, c));
                          regs.flags = (regs.flags & ~(kC | kN | kZ)) |
                                       (c ? kC : 0); set_nz(regs, get_r(regs, rd), 0x80000000u); break;
                case 0x5: set_r(regs, rd, add_flags(regs, a, b, (regs.flags & kC) ? 1 : 0)); break;
                case 0x6: set_r(regs, rd, sub_flags(regs, a, b, (regs.flags & kC) ? 1 : 0)); break;
                case 0x7: set_r(regs, rd, shift_c(a, 3, b & 0xFF, c));
                          regs.flags = (regs.flags & ~(kC | kN | kZ)) |
                                       (c ? kC : 0); set_nz(regs, get_r(regs, rd), 0x80000000u); break;
                case 0x8: sub_flags(regs, a, b, 0); break; // TST
                case 0x9: set_r(regs, rd, sub_flags(regs, 0, b, 0)); break; // RSB/NEG
                case 0xA: sub_flags(regs, a, b, 0); break; // CMP
                case 0xB: add_flags(regs, a, b, 0); break; // CMN
                case 0xC: set_r(regs, rd, a | b); set_nz(regs, a | b, 0x80000000u); break;
                case 0xD: {
                    uint32_t v = a * b;
                    set_r(regs, rd, v);
                    regs.flags &= ~(kN | kZ | kC);
                    if (v & 0x80000000u) regs.flags |= kN;
                    if (v == 0) regs.flags |= kZ;
                    break;
                }
                case 0xE: set_r(regs, rd, a & ~b); set_nz(regs, a & ~b, 0x80000000u); break;
                case 0xF: set_r(regs, rd, ~b); set_nz(regs, ~b, 0x80000000u); break;
            }
            regs.pc = next;
            return true;
        }

        // 010001: high register / BX
        if ((hw & 0xFC00) == 0x4400) {
            const uint32_t op = (hw >> 8) & 0x3;
            const uint32_t rm = (hw >> 3) & 0xF;
            const uint32_t rdn = (hw & 0x7) | ((hw >> 4) & 0x8);
            const uint32_t a = get_r(regs, rdn);
            const uint32_t b = get_r(regs, rm);
            if (op == 0) {
                set_r(regs, rdn, a + b);
            } else if (op == 1) {
                sub_flags(regs, a, b, 0);
            } else if (op == 2) {
                set_r(regs, rdn, b);
            } else {
                if ((hw & 0x0080) != 0) {
                    // BLX register
                    set_r(regs, 14, (next | 1u));
                    set_r(regs, 15, b & ~1u);
                } else {
                    const uint32_t target = b;
                    if ((target & 0xFFFFFFF0u) == 0xFFFFFFF0u) {
                        bus.on_reti();
                        return true;
                    }
                    set_r(regs, 15, target & ~1u);
                }
            }
            if (op != 3) regs.pc = next;
            return true;
        }

        // 01001: LDR literal
        if ((hw & 0xF800) == 0x4800) {
            const uint32_t rd = (hw >> 8) & 0x7;
            const Addr addr = ((pc + 4) & ~3u) + ((hw & 0xFF) << 2);
            set_r(regs, rd, read32(bus, addr));
            regs.pc = next;
            return true;
        }

        // 0101: load/store register offset
        if ((hw & 0xF000) == 0x5000) {
            const uint32_t op = (hw >> 9) & 0x7;
            const uint32_t rm = hw & 0x7;
            const uint32_t rn = (hw >> 3) & 0x7;
            const uint32_t rd = hw & 0x7;
            const Addr addr = get_r(regs, rn) + get_r(regs, rm);
            uint32_t v = 0;
            switch (op) {
                case 0: write32(bus, addr, get_r(regs, rd)); break;
                case 1: write16(bus, addr, static_cast<uint16_t>(get_r(regs, rd))); break;
                case 2: write8(bus, addr, static_cast<uint8_t>(get_r(regs, rd))); break;
                case 3: v = static_cast<uint32_t>(static_cast<int8_t>(read8(bus, addr))); set_r(regs, rd, v); break;
                case 4: set_r(regs, rd, read32(bus, addr)); break;
                case 5: set_r(regs, rd, read16(bus, addr)); break;
                case 6: set_r(regs, rd, read8(bus, addr)); break;
                default: v = static_cast<uint32_t>(static_cast<int16_t>(read16(bus, addr))); set_r(regs, rd, v); break;
            }
            regs.pc = next;
            return true;
        }

        // 011: word/byte immediate offset
        if ((hw & 0xE000) == 0x6000) {
            const bool byte = (hw & 0x1000) != 0;
            const bool load = (hw & 0x0800) != 0;
            const uint32_t imm5 = (hw >> 6) & 0x1F;
            const uint32_t rn = (hw >> 3) & 0x7;
            const uint32_t rd = hw & 0x7;
            const Addr addr = get_r(regs, rn) + (byte ? imm5 : (imm5 << 2));
            if (load) set_r(regs, rd, byte ? read8(bus, addr) : read32(bus, addr));
            else if (byte) write8(bus, addr, static_cast<uint8_t>(get_r(regs, rd)));
            else write32(bus, addr, get_r(regs, rd));
            regs.pc = next;
            return true;
        }

        // 1000: halfword immediate
        if ((hw & 0xF000) == 0x8000) {
            const bool load = (hw & 0x0800) != 0;
            const uint32_t imm5 = (hw >> 6) & 0x1F;
            const uint32_t rn = (hw >> 3) & 0x7;
            const uint32_t rd = hw & 0x7;
            const Addr addr = get_r(regs, rn) + (imm5 << 1);
            if (load) set_r(regs, rd, read16(bus, addr));
            else write16(bus, addr, static_cast<uint16_t>(get_r(regs, rd)));
            regs.pc = next;
            return true;
        }

        // 1001: SP-relative load/store
        if ((hw & 0xF000) == 0x9000) {
            const bool load = (hw & 0x0800) != 0;
            const uint32_t rd = (hw >> 8) & 0x7;
            const Addr addr = get_r(regs, 13) + ((hw & 0xFF) << 2);
            if (load) set_r(regs, rd, read32(bus, addr));
            else write32(bus, addr, get_r(regs, rd));
            regs.pc = next;
            return true;
        }

        // 1010: ADR / ADD SP
        if ((hw & 0xF000) == 0xA000) {
            const uint32_t rd = (hw >> 8) & 0x7;
            const uint32_t imm = (hw & 0xFF) << 2;
            if (hw & 0x0800) set_r(regs, 13, get_r(regs, 13) + imm);
            else set_r(regs, rd, ((pc + 4) & ~3u) + imm);
            regs.pc = next;
            return true;
        }

        // 1011: misc
        if ((hw & 0xF000) == 0xB000) {
            if ((hw & 0xFF80) == 0xB000) { // ADD SP,#imm7
                set_r(regs, 13, get_r(regs, 13) + ((hw & 0x7F) << 2));
            } else if ((hw & 0xFF80) == 0xB080) { // SUB SP,#imm7
                set_r(regs, 13, get_r(regs, 13) - ((hw & 0x7F) << 2));
            } else if ((hw & 0xF500) == 0xB100) { // CBZ/CBNZ
                const bool nz = (hw & 0x0800) != 0;
                const uint32_t rn = hw & 0x7;
                const uint32_t off = (((hw >> 9) & 1) << 6) |
                                     (((hw >> 3) & 0x1F) << 1);
                const bool zero = get_r(regs, rn) == 0;
                if (nz ? !zero : zero) next = pc + 4 + off;
            } else if ((hw & 0xFF00) == 0xB200) { // extend
                const uint32_t op = (hw >> 6) & 0x3;
                const uint32_t rm = (hw >> 3) & 0x7;
                const uint32_t rd = hw & 0x7;
                uint32_t v = get_r(regs, rm);
                if (op == 0) v = static_cast<uint32_t>(static_cast<int16_t>(v));
                else if (op == 1) v = static_cast<uint32_t>(static_cast<int8_t>(v));
                else if (op == 2) v = v & 0xFFFFu;
                else v = v & 0xFFu;
                set_r(regs, rd, v);
            } else if ((hw & 0xFE00) == 0xB400) { // PUSH
                uint32_t sp = get_r(regs, 13);
                uint32_t list = hw & 0xFF;
                if (hw & 0x0100) list |= (1u << 14);
                for (int i = 15; i >= 0; --i)
                    if (list & (1u << i)) { sp -= 4; write32(bus, sp, get_r(regs, i)); }
                set_r(regs, 13, sp);
            } else if ((hw & 0xFE00) == 0xBC00) { // POP
                uint32_t sp = get_r(regs, 13);
                uint32_t list = hw & 0xFF;
                if (hw & 0x0100) list |= (1u << 15);
                for (int i = 0; i < 16; ++i)
                    if (list & (1u << i)) { set_r(regs, i, read32(bus, sp)); sp += 4; }
                set_r(regs, 13, sp);
                if (list & (1u << 15)) return true;
            } else if (hw == 0xB672) { // CPSID i
                set_primask(bus, true);
            } else if (hw == 0xB662) { // CPSIE i
                set_primask(bus, false);
            } else if ((hw & 0xFF00) == 0xBA00) { // REV/REV16/REVSH
                const uint32_t op = (hw >> 6) & 0x3;
                const uint32_t rm = (hw >> 3) & 0x7;
                const uint32_t rd = hw & 0x7;
                uint32_t v = get_r(regs, rm);
                if (op == 0) {
                    v = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) |
                        ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
                } else if (op == 1) {
                    v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
                } else if (op == 3) {
                    v = static_cast<uint32_t>(static_cast<int16_t>(
                        static_cast<uint16_t>((v << 8) | (v >> 8))));
                } else {
                    return false;
                }
                set_r(regs, rd, v);
            } else if (hw == 0xBF00) { // NOP
            } else if ((hw & 0xFF00) == 0xBF00) { // IT（当前实现为 NOP）
                // 未实现 IT 条件块；先当作 NOP，避免直接停机。
            } else {
                return false;
            }
            regs.pc = next;
            return true;
        }

        // 1100: LDM/STM
        if ((hw & 0xF000) == 0xC000) {
            const bool load = (hw & 0x0800) != 0;
            const uint32_t rn = (hw >> 8) & 0x7;
            const uint32_t list = hw & 0xFF;
            uint32_t addr = get_r(regs, rn);
            if (load) {
                for (int i = 0; i < 8; ++i)
                    if (list & (1u << i)) { set_r(regs, i, read32(bus, addr)); addr += 4; }
                if (!(list & (1u << rn))) set_r(regs, rn, addr);
            } else {
                for (int i = 0; i < 8; ++i)
                    if (list & (1u << i)) { write32(bus, addr, get_r(regs, i)); addr += 4; }
                set_r(regs, rn, addr);
            }
            regs.pc = next;
            return true;
        }

        // 1101: conditional branch / SVC
        if ((hw & 0xF000) == 0xD000) {
            const uint32_t cond = (hw >> 8) & 0xF;
            if (cond == 0xE) { // SVC
                MCUS_WARN("ARM SVC not implemented");
                res.halted = true;
                regs.pc = next;
                return true;
            }
            if (cond == 0xF) return false;
            if (cond_passed(cond, regs.flags)) {
                const int32_t off = static_cast<int8_t>(hw & 0xFF) << 1;
                next = pc + 4 + static_cast<Addr>(off);
            }
            regs.pc = next;
            return true;
        }

        // 11100: unconditional branch
        if ((hw & 0xF800) == 0xE000) {
            int32_t off = static_cast<int32_t>(hw & 0x7FF);
            if (off & 0x400) off |= ~0x7FF;
            off <<= 1;
            regs.pc = pc + 4 + static_cast<Addr>(off);
            return true;
        }

        return false;
    }

    static uint32_t thumb_expand_imm(uint32_t imm12) {
        if ((imm12 & 0xC00) == 0x000) {
            const uint32_t imm8 = imm12 & 0xFF;
            switch ((imm12 >> 8) & 0x3) {
                case 0: return imm8;
                case 1: return (imm8 << 16) | imm8;
                case 2: return (imm8 << 24) | (imm8 << 8);
                default: return (imm8 << 24) | (imm8 << 16) | (imm8 << 8) | imm8;
            }
        }
        uint32_t value = 0x80u | (imm12 & 0x7Fu);
        const uint32_t rotate = (imm12 >> 7) & 0x1F;
        if (rotate == 0) return value;
        return (value >> rotate) | (value << (32 - rotate));
    }

    // ---- 32 位指令 ----

    bool execute32(CpuRegisters& regs, MemoryBus& bus, Addr pc, uint16_t hw1,
                   uint16_t hw2, ExecResult& res) {
        (void)res;
        Addr next = pc + 4;

        // BL / B.W
        if ((hw1 & 0xF800) == 0xF000 &&
            ((hw2 & 0xD000) == 0xD000 || (hw2 & 0xD000) == 0x9000)) {
            const bool bl = (hw2 & 0xD000) == 0xD000;
            uint32_t s = (hw1 >> 10) & 1;
            uint32_t j1 = (hw2 >> 13) & 1;
            uint32_t j2 = (hw2 >> 11) & 1;
            uint32_t i1 = 1u ^ (j1 ^ s);
            uint32_t i2 = 1u ^ (j2 ^ s);
            uint32_t imm10 = hw1 & 0x3FF;
            uint32_t imm11 = hw2 & 0x7FF;
            uint32_t off = (s << 24) | (i1 << 23) | (i2 << 22) |
                           (imm10 << 12) | (imm11 << 1);
            if (off & 0x01000000u) off |= 0xFE000000u;
            if (bl) {
                set_r(regs, 14, next | 1u);
                next = pc + 4 + off;
            } else {
                next = pc + 4 + off;
            }
            regs.pc = next;
            return true;
        }

        // MOVW / MOVT
        if ((hw1 & 0xFB00) == 0xF200) {
            const bool top = (hw1 & 0x0080) != 0;
            const uint32_t i = (hw1 >> 10) & 1;
            const uint32_t imm4 = hw1 & 0xF;
            const uint32_t imm3 = (hw2 >> 12) & 0x7;
            const uint32_t rd = (hw2 >> 8) & 0xF;
            const uint32_t imm8 = hw2 & 0xFF;
            const uint32_t imm = (imm4 << 12) | (i << 11) | (imm3 << 8) | imm8;
            uint32_t v = get_r(regs, rd);
            if (top) v = (v & 0x0000FFFFu) | (imm << 16);
            else v = imm;
            set_r(regs, rd, v);
            regs.pc = next;
            return true;
        }

        // LDR.W / STR.W immediate (including PC literal)
        if ((hw1 & 0xFFF0) == 0xF8D0 || (hw1 & 0xFFF0) == 0xF8C0) {
            const bool load = (hw1 & 0x00F0) == 0x00D0;
            const uint32_t rn = hw1 & 0xF;
            const uint32_t rt = (hw2 >> 12) & 0xF;
            const Addr imm = hw2 & 0xFFF;
            Addr base = get_r(regs, rn);
            if (rn == 15) base = (pc + 4) & ~3u;
            const Addr addr = base + imm;
            if (load) set_r(regs, rt, read32(bus, addr));
            else write32(bus, addr, get_r(regs, rt));
            regs.pc = next;
            return true;
        }

        // 数据处理的 modified immediate（MOV.W/ADD.W/SUB.W/ORR.W/EOR.W/CMP.W 等）
        if ((hw1 & 0xFA00) == 0xF000) {
            const uint32_t op = (hw1 >> 5) & 0xF;
            const bool setflags = (hw1 & 0x0010) != 0;
            const uint32_t rn = hw1 & 0xF;
            const uint32_t i = (hw1 >> 10) & 1;
            const uint32_t imm3 = (hw2 >> 12) & 0x7;
            const uint32_t rd = (hw2 >> 8) & 0xF;
            const uint32_t imm8 = hw2 & 0xFF;
            const uint32_t imm = thumb_expand_imm((i << 11) | (imm3 << 8) | imm8);
            const uint32_t a = get_r(regs, rn);
            uint32_t result = 0;
            bool write_rd = true;
            switch (op) {
                case 0x0: result = a & imm; if (rd == 15) write_rd = false; break; // AND/TST
                case 0x1: result = a & ~imm; break;                              // BIC
                case 0x2: result = (rn == 15) ? imm : (a | imm); break;          // ORR/MOV
                case 0x3: result = (rn == 15) ? ~imm : (a | ~imm); break;        // ORN/MVN
                case 0x4: result = a ^ imm; if (rd == 15) write_rd = false; break; // EOR/TEQ
                case 0x8:
                    if (rd == 15) { add_flags(regs, a, imm, 0); write_rd = false; }
                    else { result = a + imm; if (setflags) add_flags(regs, a, imm, 0); }
                    break;                                                        // ADD/CMN
                case 0xA: result = add_flags(regs, a, imm, (regs.flags & kC) ? 1 : 0); break; // ADC
                case 0xB: result = sub_flags(regs, a, imm, (regs.flags & kC) ? 1 : 0); break; // SBC
                case 0xD:
                    if (rd == 15) { sub_flags(regs, a, imm, 0); write_rd = false; }
                    else { result = a - imm; if (setflags) sub_flags(regs, a, imm, 0); }
                    break;                                                        // SUB/CMP
                case 0xE: result = sub_flags(regs, imm, a, 0); break;              // RSB
                default: return false;
            }
            if (write_rd) set_r(regs, rd, result);
            // 逻辑类指令在 S=1 或 Rd=PC（TST/TEQ）时更新 N/Z。
            if (op <= 0x4 && (setflags || rd == 15))
                set_nz(regs, result, 0x80000000u);
            regs.pc = next;
            return true;
        }

        // 逻辑/算术寄存器指令（AND/BIC/ORR/ORN/EOR/ADD/SUB/CMP）
        if ((hw1 & 0xFF00) == 0xEA00 || (hw1 & 0xFF00) == 0xEB00) {
            const uint32_t group = hw1 & 0x00E0;
            const bool is_eb = (hw1 & 0xFF00) == 0xEB00;
            const bool setflags = (hw1 & 0x0010) != 0;
            const uint32_t rn = hw1 & 0xF;
            const uint32_t rd = (hw2 >> 8) & 0xF;
            const uint32_t rm = hw2 & 0xF;
            const uint32_t a = get_r(regs, rn);
            bool carry = false;
            const uint32_t b = shift_c(get_r(regs, rm), (hw2 >> 4) & 0x3,
                                       (hw2 >> 6) & 0x3, carry);
            uint32_t result = 0;
            bool write_rd = true;
            if (!is_eb) {
                switch (group) {
                    case 0x00: result = a & b; break;
                    case 0x20: result = a & ~b; break;
                    case 0x40: result = (rn == 15) ? b : (a | b); break;
                    case 0x60: result = (rn == 15) ? ~b : (a | ~b); break;
                    case 0x80: result = a ^ b; break;
                    default: return false;
                }
                if (rd == 15) write_rd = false;
                if (write_rd) set_r(regs, rd, result);
                if (setflags || rd == 15)
                    set_nz(regs, result, 0x80000000u);
            } else if (group == 0x00) {
                const uint32_t r = a + b;
                if (setflags) add_flags(regs, a, b, 0);
                set_r(regs, rd, r);
            } else if (group == 0xA0) {
                const uint32_t r = a - b;
                if (setflags) sub_flags(regs, a, b, 0);
                set_r(regs, rd, r);
            } else if (group == 0xB0) {
                sub_flags(regs, a, b, 0);
            } else {
                return false;
            }
            regs.pc = next;
            return true;
        }

        // LDR/STR.W 寄存器偏移
        {
            const uint32_t g = hw1 & 0xFFF0;
            bool known = true, load = false, sign = false, half = false;
            uint32_t size = 4;
            switch (g) {
                case 0xF850: load = true; size = 4; break;
                case 0xF840: size = 4; break;
                case 0xF830: load = true; size = 2; break;
                case 0xF820: size = 2; break;
                case 0xF810: load = true; size = 1; break;
                case 0xF800: size = 1; break;
                case 0xF930: load = true; sign = true; size = 2; break;
                case 0xF910: load = true; sign = true; size = 1; break;
                default: known = false; break;
            }
            if (known) {
                const uint32_t rn = hw1 & 0xF;
                const uint32_t rt = (hw2 >> 12) & 0xF;
                const uint32_t rm = hw2 & 0xF;
                const uint32_t shift = (hw2 >> 4) & 0x3;
                const Addr addr = get_r(regs, rn) + (get_r(regs, rm) << shift);
                if (load) {
                    uint32_t v = 0;
                    if (size == 4) v = read32(bus, addr);
                    else if (size == 2) {
                        v = read16(bus, addr);
                        if (sign) v = static_cast<uint32_t>(static_cast<int16_t>(v));
                    } else {
                        v = read8(bus, addr);
                        if (sign) v = static_cast<uint32_t>(static_cast<int8_t>(v));
                    }
                    set_r(regs, rt, v);
                } else {
                    if (size == 4) write32(bus, addr, get_r(regs, rt));
                    else if (size == 2) write16(bus, addr, static_cast<uint16_t>(get_r(regs, rt)));
                    else write8(bus, addr, static_cast<uint8_t>(get_r(regs, rt)));
                }
                (void)half;
                regs.pc = next;
                return true;
            }
        }

        // STM/LDM.W（含 PUSH.W/POP.W）
        {
            const uint32_t g = hw1 & 0xFFE0;
            if (g == 0xE900 || g == 0xE920 || g == 0xE8A0 || g == 0xE8B0 || g == 0xE930) {
                const bool load = (g == 0xE8B0 || g == 0xE930);
                const bool decrement = (g == 0xE900 || g == 0xE920 || g == 0xE930);
                const uint32_t rn = hw1 & 0xF;
                const uint32_t list = hw2 & 0xFFFF;
                uint32_t count = 0;
                for (int i = 0; i < 16; ++i) if (list & (1u << i)) ++count;
                uint32_t addr = get_r(regs, rn);
                if (decrement) addr -= count * 4;
                if (load) {
                    for (int i = 0; i < 16; ++i)
                        if (list & (1u << i)) { set_r(regs, i, read32(bus, addr)); addr += 4; }
                } else {
                    for (int i = 0; i < 16; ++i)
                        if (list & (1u << i)) { write32(bus, addr, get_r(regs, i)); addr += 4; }
                }
                if (!(list & (1u << rn)))
                    set_r(regs, rn, decrement ? get_r(regs, rn) - count * 4
                                               : get_r(regs, rn) + count * 4);
                if (load && (list & (1u << 15))) return true;
                regs.pc = next;
                return true;
            }
        }

        // UMULL/SMULL（32x32 -> 64）
        if ((hw1 & 0xFFF0) == 0xFBA0) {
            const uint32_t rdlo = (hw2 >> 12) & 0xF;
            const uint32_t rdhi = (hw2 >> 8) & 0xF;
            const uint32_t rn = hw1 & 0xF;
            const uint32_t rm = hw2 & 0xF;
            const uint64_t p = static_cast<uint64_t>(get_r(regs, rn)) *
                               static_cast<uint64_t>(get_r(regs, rm));
            set_r(regs, rdlo, static_cast<uint32_t>(p));
            set_r(regs, rdhi, static_cast<uint32_t>(p >> 32));
            regs.pc = next;
            return true;
        }

        // MRS/MSR PRIMASK
        if (hw1 == 0xF3EF && (hw2 & 0xFF00) == 0x8000) {
            const uint32_t rd = (hw2 >> 8) & 0xF;
            set_r(regs, rd, primask(bus) ? 1u : 0u);
            regs.pc = next;
            return true;
        }
        if (hw1 == 0xF380 && (hw2 & 0xFF00) == 0x8800) {
            const uint32_t rn = hw2 & 0xF;
            set_primask(bus, (get_r(regs, rn) & 1u) != 0);
            regs.pc = next;
            return true;
        }

        return false;
    }

    // PRIMASK 由模型里的全局中断使能寄存器表示（invert=true）。
    bool primask(MemoryBus& bus) const {
        const auto& g = model_.arch_desc.global_irq_enable;
        if (!g.present || g.space.empty()) return false;
        const RegisterDesc* r = model_.find_register(g.space, g.reg);
        if (!r) return false;
        uint32_t v = read_reg(bus, *r);
        return (v & (1u << g.bit)) != 0;
    }
    void set_primask(MemoryBus& bus, bool masked) const {
        const auto& g = model_.arch_desc.global_irq_enable;
        if (!g.present || g.space.empty()) return;
        const RegisterDesc* r = model_.find_register(g.space, g.reg);
        if (!r) return;
        uint32_t v = read_reg(bus, *r);
        if (masked) v |= (1u << g.bit);
        else v &= ~(1u << g.bit);
        write_reg(bus, *r, v);
    }
};

} // namespace

// ---- 外部 ISA 动态库 ABI ----

extern "C" uint32_t mcus_isa_abi_version() {
    return kIsaAbiVersion;
}

extern "C" const char* mcus_isa_arch() {
    return "arm";
}

extern "C" InstructionSet* mcus_isa_create(const Model* model) {
    if (!model) return nullptr;
    return new IsaArm(*model);
}

extern "C" void mcus_isa_destroy(InstructionSet* isa) {
    delete isa;
}

} // namespace mcus
