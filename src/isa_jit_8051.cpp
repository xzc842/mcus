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

// 8051 -> 字节码 Translator。
//
// 第一版覆盖常用顺序指令和直接跳转；其余 opcode 让调用方回退解释器。
// 复杂语义（flags/位寻址/中断）通过 CallHelper 逐步补齐。
#include "mcus/jit.h"

#include <cstdint>
#include <unordered_map>

namespace mcus {
namespace {

enum HelperId : uint32_t {
    H_Nop = 0,
    H_Sjmp = 1,
    H_Ajmp = 2,
    H_Ljmp = 3,
    H_IncA = 4,
    H_MovAImm = 5,
    H_MovARn = 6,
    H_MovRnA = 7,
    H_MovADirect = 8,
    H_MovDirectA = 9,
    H_MovDirectImm = 10,
    H_Add = 11,
    H_Addc = 12,
    H_Subb = 13,
    H_Anl = 14,
    H_Orl = 15,
    H_Xrl = 16,
    H_IncOperand = 17,
    H_DecOperand = 18,
    H_IncDptr = 19,
    H_ClrA = 20,
    H_CplA = 21,
    H_RlA = 22,
    H_RrA = 23,
    H_RlcA = 24,
    H_RrcA = 25,
    H_SwapA = 26,
    H_AnlDirectA = 27,
    H_OrlDirectA = 28,
    H_XrlDirectA = 29,
    H_AnlDirectImm = 30,
    H_OrlDirectImm = 31,
    H_XrlDirectImm = 32,
    H_DecA = 33,
    H_Jz = 34,
    H_Jnz = 35,
    H_Jc = 36,
    H_Jnc = 37,
    H_Jb = 38,
    H_Jnb = 39,
    H_Jbc = 40,
    H_Acall = 41,
    H_Lcall = 42,
    H_Ret = 43,
    H_Push = 44,
    H_Pop = 45,
    H_MovDirectDirect = 46,
    H_MovDirectRn = 47,
    H_MovRnDirect = 48,
    H_MovIndirectA = 49,
    H_MovAIndirect = 50,
    H_MovIndirectImm = 51,
    H_MovIndirectDirect = 52,
    H_MovDirectIndirect = 53,
    H_MovDptrImm = 54,
    H_SetbBit = 55,
    H_ClrBit = 56,
    H_CplBit = 57,
    H_MovCBit = 58,
    H_MovBitC = 59,
    H_CjneAImm = 60,
    H_CjneADirect = 61,
    H_CjneAIndirect = 62,
    H_CjneARn = 63,
    H_DjnzDirect = 64,
    H_DjnzRn = 65,
    H_MulAB = 66,
    H_DivAB = 67,
    H_DaA = 68,
    H_MovcApc = 69,
    H_MovcAdptr = 70,
    H_MovxAIndirect = 71,
    H_MovxIndirectA = 72,
    H_MovxAdptr = 73,
    H_MovxDptrA = 74,
    H_XchADirect = 75,
    H_XchAIndirect = 76,
    H_XchARn = 77,
    H_XchdAIndirect = 78,
    H_AnlCBit = 79,
    H_OrlCBit = 80,
    H_AnlCNBit = 81,
    H_OrlCNBit = 82,
    H_ClrC = 83,
    H_SetbC = 84,
    H_CplC = 85,
    H_Reti = 86,
    H_MovRnImm = 87,
    H_JmpADptr = 88,
};

// operand mode 编码在 helper 参数低 8 位，操作数在高 24 位。
enum OperandMode : uint32_t {
    OM_IMM = 0,
    OM_DIRECT = 1,
    OM_INDIRECT = 2,
    OM_RN = 3,
};

uint32_t rd8(JitContext* c, uint8_t space, Addr addr) {
    return c->read ? c->read(c->user, space, addr, 8) : 0;
}

void wr8(JitContext* c, uint8_t space, Addr addr, uint32_t v) {
    if (c->write) c->write(c->user, space, addr, v, 8);
}

uint32_t read_acc(JitContext* c) { return rd8(c, c->sfr_space, 0xE0); }

void write_acc(JitContext* c, uint32_t v) {
    wr8(c, c->sfr_space, 0xE0, v & 0xFFu);
    uint32_t p = 0;
    for (int i = 0; i < 8; ++i) p ^= (v >> i) & 1u;
    uint32_t psw = rd8(c, c->sfr_space, 0xD0);
    wr8(c, c->sfr_space, 0xD0, (psw & ~1u) | p);
}

uint32_t reg_base(JitContext* c) {
    const uint32_t psw = rd8(c, c->sfr_space, 0xD0);
    return ((psw & 0x18u) >> 3) * 8u;
}

uint32_t reg_read(JitContext* c, uint32_t rn) {
    return rd8(c, c->iram_space, reg_base(c) + (rn & 7u));
}

void reg_write(JitContext* c, uint32_t rn, uint32_t v) {
    wr8(c, c->iram_space, reg_base(c) + (rn & 7u), v & 0xFFu);
}

uint32_t direct_read(JitContext* c, uint32_t addr) {
    return addr < 0x80 ? rd8(c, c->iram_space, addr)
                       : rd8(c, c->sfr_space, addr);
}

void direct_write(JitContext* c, uint32_t addr, uint32_t v) {
    if (addr < 0x80) wr8(c, c->iram_space, addr, v & 0xFFu);
    else wr8(c, c->sfr_space, addr, v & 0xFFu);
}


uint32_t read_psw_bit(JitContext* c, uint32_t mask) {
    return rd8(c, c->sfr_space, 0xD0) & mask;
}

void write_psw_bit(JitContext* c, uint32_t mask, bool v) {
    uint32_t psw = rd8(c, c->sfr_space, 0xD0);
    psw = v ? (psw | mask) : (psw & ~mask);
    wr8(c, c->sfr_space, 0xD0, psw);
}

bool read_cy(JitContext* c) { return read_psw_bit(c, 0x80u) != 0; }
void write_cy(JitContext* c, bool v) { write_psw_bit(c, 0x80u, v); }
void write_ac(JitContext* c, bool v) { write_psw_bit(c, 0x40u, v); }
void write_ov(JitContext* c, bool v) { write_psw_bit(c, 0x04u, v); }

uint32_t read_operand(JitContext* c, uint32_t arg) {
    const uint32_t mode = arg & 0xFFu;
    const uint32_t v = (arg >> 8) & 0xFFu;
    switch (mode) {
        case OM_IMM: return v;
        case OM_DIRECT: return direct_read(c, v);
        case OM_INDIRECT: return rd8(c, c->iram_space, reg_read(c, v));
        default: return reg_read(c, v);
    }
}

void write_operand(JitContext* c, uint32_t arg, uint32_t value) {
    const uint32_t mode = arg & 0xFFu;
    const uint32_t v = (arg >> 8) & 0xFFu;
    if (mode == OM_DIRECT) direct_write(c, v, value);
    else if (mode == OM_INDIRECT) wr8(c, c->iram_space, reg_read(c, v), value);
    else reg_write(c, v, value);
}

void add_a(JitContext* c, uint32_t src, bool with_carry) {
    const uint32_t a = read_acc(c);
    const uint32_t cy = with_carry && read_cy(c) ? 1u : 0u;
    const uint32_t r = a + (src & 0xFFu) + cy;
    write_cy(c, r > 0xFFu);
    write_ac(c, ((a & 0x0Fu) + (src & 0x0Fu) + cy) > 0x0Fu);
    const uint32_t result = r & 0xFFu;
    write_ov(c, ((~(a ^ src) & (a ^ result) & 0x80u) != 0));
    write_acc(c, result);
}

void subb_a(JitContext* c, uint32_t src) {
    const uint32_t a = read_acc(c);
    const int cy = read_cy(c) ? 1 : 0;
    const int r = static_cast<int>(a) - static_cast<int>(src & 0xFFu) - cy;
    write_cy(c, r < 0);
    write_ac(c, (static_cast<int>(a & 0x0F) -
                 static_cast<int>(src & 0x0F) - cy) < 0);
    const uint32_t result = static_cast<uint32_t>(r) & 0xFFu;
    write_ov(c, (((a ^ src) & (a ^ result) & 0x80u) != 0));
    write_acc(c, result);
}

void rotate_a(JitContext* c, int kind) {
    const uint32_t a = read_acc(c);
    uint32_t v = a;
    bool cy = read_cy(c);
    if (kind == 0) { // RR
        v = ((a >> 1) | (a << 7)) & 0xFFu;
    } else if (kind == 1) { // RRC
        const bool new_cy = (a & 1u) != 0;
        v = ((a >> 1) | (cy ? 0x80u : 0u)) & 0xFFu;
        write_cy(c, new_cy);
    } else if (kind == 2) { // RL
        v = ((a << 1) | (a >> 7)) & 0xFFu;
    } else { // RLC
        const bool new_cy = (a & 0x80u) != 0;
        v = ((a << 1) | (cy ? 1u : 0u)) & 0xFFu;
        write_cy(c, new_cy);
    }
    write_acc(c, v);
}

uint32_t bit_read(JitContext* c, uint32_t bit_addr) {
    uint8_t space;
    Addr byte_addr;
    uint8_t bit;
    if (bit_addr < 0x80) {
        space = c->iram_space;
        byte_addr = 0x20 + (bit_addr / 8);
        bit = static_cast<uint8_t>(bit_addr % 8);
    } else {
        space = c->sfr_space;
        byte_addr = bit_addr & 0xF8;
        bit = static_cast<uint8_t>(bit_addr & 0x07);
    }
    return (rd8(c, space, byte_addr) >> bit) & 1u;
}

void bit_write(JitContext* c, uint32_t bit_addr, bool v) {
    uint8_t space;
    Addr byte_addr;
    uint8_t bit;
    if (bit_addr < 0x80) {
        space = c->iram_space;
        byte_addr = 0x20 + (bit_addr / 8);
        bit = static_cast<uint8_t>(bit_addr % 8);
    } else {
        space = c->sfr_space;
        byte_addr = bit_addr & 0xF8;
        bit = static_cast<uint8_t>(bit_addr & 0x07);
    }
    uint32_t cur = rd8(c, space, byte_addr);
    cur = v ? (cur | (1u << bit)) : (cur & ~(1u << bit));
    wr8(c, space, byte_addr, cur);
}


void push8(JitContext* c, uint32_t v) {
    uint32_t sp = (rd8(c, c->sfr_space, 0x81) + 1u) & 0xFFu;
    wr8(c, c->sfr_space, 0x81, sp);
    wr8(c, c->iram_space, sp, v & 0xFFu);
}

uint32_t pop8(JitContext* c) {
    const uint32_t sp = rd8(c, c->sfr_space, 0x81);
    const uint32_t v = rd8(c, c->iram_space, sp);
    wr8(c, c->sfr_space, 0x81, (sp - 1u) & 0xFFu);
    return v;
}

uint32_t helper_dispatch(JitContext* c, uint32_t id, uint32_t a, uint32_t b) {
    switch (id) {
        case H_Nop:
            break;
        case H_Sjmp:
        case H_Ajmp:
        case H_Ljmp:
            c->regs.pc = a;
            break;
        case H_IncA:
            write_acc(c, read_acc(c) + 1u);
            break;
        case H_MovAImm:
            write_acc(c, a);
            break;
        case H_MovARn:
            write_acc(c, reg_read(c, a));
            break;
        case H_MovRnA:
            reg_write(c, a, read_acc(c));
            break;
        case H_MovRnImm:
            reg_write(c, a, b);
            break;
        case H_JmpADptr: {
            const uint32_t dptr = (rd8(c, c->sfr_space, 0x83) << 8) |
                                  rd8(c, c->sfr_space, 0x82);
            c->regs.pc = (dptr + read_acc(c)) & 0xFFFFu;
            break;
        }
        case H_MovADirect:
            write_acc(c, direct_read(c, a));
            break;
        case H_MovDirectA:
            direct_write(c, a, read_acc(c));
            break;
        case H_MovDirectImm:
            direct_write(c, a, b);
            break;
        case H_Add:
            add_a(c, read_operand(c, a), false);
            break;
        case H_Addc:
            add_a(c, read_operand(c, a), true);
            break;
        case H_Subb:
            subb_a(c, read_operand(c, a));
            break;
        case H_Anl:
            write_acc(c, read_acc(c) & read_operand(c, a));
            break;
        case H_Orl:
            write_acc(c, read_acc(c) | read_operand(c, a));
            break;
        case H_Xrl:
            write_acc(c, read_acc(c) ^ read_operand(c, a));
            break;
        case H_IncOperand:
            write_operand(c, a, read_operand(c, a) + 1u);
            break;
        case H_DecOperand:
            write_operand(c, a, read_operand(c, a) - 1u);
            break;
        case H_IncDptr: {
            uint32_t lo = rd8(c, c->sfr_space, 0x82);
            uint32_t hi = rd8(c, c->sfr_space, 0x83);
            uint32_t v = ((hi << 8) | lo) + 1u;
            wr8(c, c->sfr_space, 0x83, (v >> 8) & 0xFFu);
            wr8(c, c->sfr_space, 0x82, v & 0xFFu);
            break;
        }
        case H_ClrA:
            write_acc(c, 0);
            break;
        case H_CplA:
            write_acc(c, ~read_acc(c) & 0xFFu);
            break;
        case H_RlA: rotate_a(c, 2); break;
        case H_RrA: rotate_a(c, 0); break;
        case H_RlcA: rotate_a(c, 3); break;
        case H_RrcA: rotate_a(c, 1); break;
        case H_SwapA: {
            const uint32_t v = read_acc(c);
            write_acc(c, ((v << 4) | (v >> 4)) & 0xFFu);
            break;
        }
        case H_AnlDirectA:
            direct_write(c, a, direct_read(c, a) & read_acc(c));
            break;
        case H_OrlDirectA:
            direct_write(c, a, direct_read(c, a) | read_acc(c));
            break;
        case H_XrlDirectA:
            direct_write(c, a, direct_read(c, a) ^ read_acc(c));
            break;
        case H_AnlDirectImm:
            direct_write(c, a, direct_read(c, a) & b);
            break;
        case H_OrlDirectImm:
            direct_write(c, a, direct_read(c, a) | b);
            break;
        case H_XrlDirectImm:
            direct_write(c, a, direct_read(c, a) ^ b);
            break;
        case H_DecA:
            write_acc(c, read_acc(c) - 1u);
            break;
        case H_Jz:
            c->regs.pc = read_acc(c) == 0 ? a : (c->regs.pc + 2);
            break;
        case H_Jnz:
            c->regs.pc = read_acc(c) != 0 ? a : (c->regs.pc + 2);
            break;
        case H_Jc:
            c->regs.pc = read_cy(c) ? a : (c->regs.pc + 2);
            break;
        case H_Jnc:
            c->regs.pc = !read_cy(c) ? a : (c->regs.pc + 2);
            break;
        case H_Jb:
            c->regs.pc = bit_read(c, b) ? a : (c->regs.pc + 3);
            break;
        case H_Jnb:
            c->regs.pc = !bit_read(c, b) ? a : (c->regs.pc + 3);
            break;
        case H_Jbc:
            if (bit_read(c, b)) {
                bit_write(c, b, false);
                c->regs.pc = a;
            } else {
                c->regs.pc += 3;
            }
            break;
        case H_Acall:
        case H_Lcall:
            push8(c, (b >> 8) & 0xFFu);
            push8(c, b & 0xFFu);
            c->regs.pc = a;
            break;
        case H_Ret:
        case H_Reti: {
            const uint32_t hi = pop8(c);
            const uint32_t lo = pop8(c);
            c->regs.pc = (hi << 8) | lo;
            if (id == H_Reti && c->reti) c->reti(c->user);
            break;
        }
        case H_Push:
            push8(c, direct_read(c, a));
            break;
        case H_Pop:
            direct_write(c, a, pop8(c));
            break;
        case H_MovDirectDirect: {
            const uint32_t src = a & 0xFFu;
            const uint32_t dst = (a >> 8) & 0xFFu;
            direct_write(c, dst, direct_read(c, src));
            break;
        }
        case H_MovDirectRn:
            direct_write(c, a, reg_read(c, b));
            break;
        case H_MovRnDirect:
            reg_write(c, a, direct_read(c, b));
            break;
        case H_MovIndirectA:
            wr8(c, c->iram_space, reg_read(c, a), read_acc(c));
            break;
        case H_MovAIndirect:
            write_acc(c, rd8(c, c->iram_space, reg_read(c, a)));
            break;
        case H_MovIndirectImm:
            wr8(c, c->iram_space, reg_read(c, a), b & 0xFFu);
            break;
        case H_MovIndirectDirect:
            wr8(c, c->iram_space, reg_read(c, a), direct_read(c, b));
            break;
        case H_MovDirectIndirect:
            direct_write(c, a, rd8(c, c->iram_space, reg_read(c, b)));
            break;
        case H_MovDptrImm:
            wr8(c, c->sfr_space, 0x83, (a >> 8) & 0xFFu);
            wr8(c, c->sfr_space, 0x82, a & 0xFFu);
            break;
        case H_SetbBit:
            bit_write(c, a, true);
            break;
        case H_ClrBit:
            bit_write(c, a, false);
            break;
        case H_CplBit:
            bit_write(c, a, !bit_read(c, a));
            break;
        case H_MovCBit:
            write_cy(c, bit_read(c, a) != 0);
            break;
        case H_MovBitC:
            bit_write(c, a, read_cy(c));
            break;
        case H_CjneAImm:
        case H_CjneADirect:
        case H_CjneAIndirect:
        case H_CjneARn: {
            const uint32_t v = id == H_CjneAImm ? b
                              : (id == H_CjneADirect ? direct_read(c, b)
                              : (id == H_CjneAIndirect
                                     ? rd8(c, c->iram_space, reg_read(c, b))
                                     : reg_read(c, b)));
            const uint32_t acc = read_acc(c);
            write_cy(c, acc < v);
            c->regs.pc = acc != v ? a : (c->regs.pc + 3);
            break;
        }
        case H_DjnzDirect: {
            uint32_t v = (direct_read(c, b) - 1u) & 0xFFu;
            direct_write(c, b, v);
            c->regs.pc = v != 0 ? a : (c->regs.pc + 3);
            break;
        }
        case H_DjnzRn: {
            uint32_t v = (reg_read(c, b) - 1u) & 0xFFu;
            reg_write(c, b, v);
            c->regs.pc = v != 0 ? a : (c->regs.pc + 2);
            break;
        }
        case H_MulAB: {
            const uint32_t r = read_acc(c) * rd8(c, c->sfr_space, 0xF0);
            write_acc(c, r & 0xFFu);
            wr8(c, c->sfr_space, 0xF0, (r >> 8) & 0xFFu);
            write_cy(c, false);
            write_ov(c, r > 0xFFu);
            break;
        }
        case H_DivAB: {
            const uint32_t a = read_acc(c);
            const uint32_t b = rd8(c, c->sfr_space, 0xF0);
            if (b == 0) {
                write_cy(c, true);
                write_ov(c, true);
            } else {
                write_acc(c, a / b);
                wr8(c, c->sfr_space, 0xF0, a % b);
                write_cy(c, false);
                write_ov(c, false);
            }
            break;
        }
        case H_DaA: {
            uint32_t a = read_acc(c);
            bool cy = read_cy(c);
            const bool ac = (rd8(c, c->sfr_space, 0xD0) & 0x40u) != 0;
            bool new_ac = false;
            if ((a & 0x0Fu) > 0x09u || ac) {
                const uint32_t lo = a + 0x06u;
                new_ac = ((lo & 0xF0u) != (a & 0xF0u));
                a = lo & 0xFFu;
            }
            if ((a & 0xF0u) > 0x90u || cy) {
                a = (a + 0x60u) & 0xFFu;
                cy = true;
            }
            write_cy(c, cy);
            write_ac(c, new_ac);
            write_acc(c, a);
            break;
        }
        case H_MovcApc:
            write_acc(c, rd8(c, c->code_space,
                             (c->regs.pc + 1u + read_acc(c)) & 0xFFFFu));
            break;
        case H_MovcAdptr: {
            const uint32_t dptr = (rd8(c, c->sfr_space, 0x83) << 8) |
                                  rd8(c, c->sfr_space, 0x82);
            write_acc(c, rd8(c, c->code_space,
                             (dptr + read_acc(c)) & 0xFFFFu));
            break;
        }
        case H_MovxAIndirect: {
            const uint32_t p2 = rd8(c, c->sfr_space, 0xA0);
            const uint32_t addr = (p2 << 8) | reg_read(c, b);
            write_acc(c, rd8(c, c->xram_space, addr));
            break;
        }
        case H_MovxIndirectA: {
            const uint32_t p2 = rd8(c, c->sfr_space, 0xA0);
            const uint32_t addr = (p2 << 8) | reg_read(c, b);
            wr8(c, c->xram_space, addr, read_acc(c));
            break;
        }
        case H_MovxAdptr: {
            const uint32_t dptr = (rd8(c, c->sfr_space, 0x83) << 8) |
                                  rd8(c, c->sfr_space, 0x82);
            write_acc(c, rd8(c, c->xram_space, dptr));
            break;
        }
        case H_MovxDptrA: {
            const uint32_t dptr = (rd8(c, c->sfr_space, 0x83) << 8) |
                                  rd8(c, c->sfr_space, 0x82);
            wr8(c, c->xram_space, dptr, read_acc(c));
            break;
        }
        case H_XchADirect: {
            const uint32_t a = read_acc(c);
            const uint32_t v = direct_read(c, b);
            direct_write(c, b, a);
            write_acc(c, v);
            break;
        }
        case H_XchAIndirect: {
            const uint32_t addr = reg_read(c, b);
            const uint32_t a = read_acc(c);
            const uint32_t v = rd8(c, c->iram_space, addr);
            wr8(c, c->iram_space, addr, a);
            write_acc(c, v);
            break;
        }
        case H_XchARn: {
            const uint32_t a = read_acc(c);
            const uint32_t v = reg_read(c, b);
            reg_write(c, b, a);
            write_acc(c, v);
            break;
        }
        case H_XchdAIndirect: {
            const uint32_t addr = reg_read(c, b);
            const uint32_t a = read_acc(c);
            const uint32_t d = rd8(c, c->iram_space, addr);
            wr8(c, c->iram_space, addr, (d & 0xF0u) | (a & 0x0Fu));
            write_acc(c, (a & 0xF0u) | (d & 0x0Fu));
            break;
        }
        case H_AnlCBit:
            write_cy(c, read_cy(c) && bit_read(c, b));
            break;
        case H_OrlCBit:
            write_cy(c, read_cy(c) || bit_read(c, b));
            break;
        case H_AnlCNBit:
            write_cy(c, read_cy(c) && !bit_read(c, b));
            break;
        case H_OrlCNBit:
            write_cy(c, read_cy(c) || !bit_read(c, b));
            break;
        case H_ClrC:
            write_cy(c, false);
            break;
        case H_SetbC:
            write_cy(c, true);
            break;
        case H_CplC:
            write_cy(c, !read_cy(c));
            break;
        default:
            break;
    }
    return 0;
}

class Jit8051 final : public JitTranslator {
public:
    explicit Jit8051(const Model& model) : model_(model) {}

    JitHelperFn helper() const override { return helper_dispatch; }

    bool translate(const Model& model, const JitCodeSource& code, Addr pc,
                   BcProgram& out) override {
        (void)model;
        out = BcProgram{};
        out.arch = "8051";
        out.guest_pc = pc;
        out.cycles = 0;

        auto emit = [&](BcOp op, uint32_t a = 0, uint32_t b = 0,
                        uint32_t imm = 0) {
            BcInsn in{};
            in.op = static_cast<uint16_t>(op);
            in.a = static_cast<uint8_t>(a);
            in.b = static_cast<uint8_t>(b);
            in.imm = imm;
            out.code.push_back(in);
        };

        const Addr start = pc;
        size_t count = 0;
        std::unordered_map<Addr, uint32_t> block_index;
        while (count < 32) {
            uint8_t op = 0;
            if (!code.fetch8(pc, op)) break;
            block_index[pc] = static_cast<uint32_t>(out.code.size());

            uint32_t helper_id = H_Nop;
            uint32_t arg0 = 0, arg1 = 0;
            BcOp native_op = BcOp::CallHelper;
            uint32_t native_a = 0, native_imm = 0;
            Addr next = pc + 1;
            bool branch = false;
            bool dynamic_branch = false;
            Addr target = 0;
            bool supported = true;

            if (op == 0x00) { // NOP
            } else if (op == 0x80) { // SJMP rel
                uint8_t rel = 0;
                if (!code.fetch8(pc + 1, rel)) { supported = false; }
                else {
                    target = static_cast<Addr>(
                        static_cast<uint16_t>(pc + 2) +
                        static_cast<int8_t>(rel));
                    next = pc + 2;
                    helper_id = H_Sjmp;
                    arg0 = target;
                    branch = true;
                }
            } else if (op == 0x02) { // LJMP addr16
                uint8_t hi = 0, lo = 0;
                if (!code.fetch8(pc + 1, hi) ||
                    !code.fetch8(pc + 2, lo)) {
                    supported = false;
                } else {
                    target = (static_cast<Addr>(hi) << 8) | lo;
                    next = pc + 3;
                    helper_id = H_Ljmp;
                    arg0 = target;
                    branch = true;
                }
            } else if ((op & 0x1F) == 0x01) { // AJMP addr11
                uint8_t lo = 0;
                if (!code.fetch8(pc + 1, lo)) {
                    supported = false;
                } else {
                    target = static_cast<Addr>(
                        ((pc + 2) & 0xF800u) |
                        (static_cast<uint32_t>(op & 0xE0u) << 3) | lo);
                    next = pc + 2;
                    helper_id = H_Ajmp;
                    arg0 = target;
                    branch = true;
                }
            } else if (op >= 0x78 && op <= 0x7F) { // MOV Rn,#imm
                uint8_t imm = 0;
                if (!code.fetch8(pc + 1, imm)) supported = false;
                else {
                    helper_id = H_MovRnImm;
                    arg0 = op & 7;
                    arg1 = imm;
                    next = pc + 2;
                }
            } else if (op == 0x73) { // JMP @A+DPTR
                helper_id = H_JmpADptr;
                branch = true;
                dynamic_branch = true;
            } else if (op == 0x04) { // INC A
                native_op = BcOp::IncA;
            } else if (op == 0x54 || op == 0x44 || op == 0x64) {
                // ANL/ORL/XRL A,#imm
                uint8_t imm = 0;
                if (!code.fetch8(pc + 1, imm)) supported = false;
                else {
                    native_op = op == 0x54 ? BcOp::AnlAImm
                              : (op == 0x44 ? BcOp::OrlAImm : BcOp::XrlAImm);
                    native_a = imm;
                    next = pc + 2;
                }
            } else if (op == 0x74) { // MOV A,#imm
                uint8_t imm = 0;
                if (!code.fetch8(pc + 1, imm)) supported = false;
                else { native_op = BcOp::MovAImm; native_a = imm; next = pc + 2; }
            } else if (op >= 0xE8 && op <= 0xEF) { // MOV A,Rn
                helper_id = H_MovARn;
                arg0 = op & 0x07;
            } else if (op >= 0xF8) { // MOV Rn,A
                helper_id = H_MovRnA;
                arg0 = op & 0x07;
            } else if (op == 0xE5) { // MOV A,direct
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else if (addr < 0x80) {
                    native_op = BcOp::MovAIram;
                    native_imm = addr;
                    next = pc + 2;
                } else if (addr == 0xE0) {
                    native_op = BcOp::MovASfr;
                    next = pc + 2;
                } else {
                    helper_id = H_MovADirect;
                    arg0 = addr;
                    next = pc + 2;
                }
            } else if (op == 0xF5) { // MOV direct,A
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else if (addr < 0x80) {
                    native_op = BcOp::MovIramA;
                    native_imm = addr;
                    next = pc + 2;
                } else if (addr == 0xE0) {
                    native_op = BcOp::MovSfrA;
                    next = pc + 2;
                } else {
                    helper_id = H_MovDirectA;
                    arg0 = addr;
                    next = pc + 2;
                }
            } else if (op == 0x75) { // MOV direct,#imm
                uint8_t addr = 0, imm = 0;
                if (!code.fetch8(pc + 1, addr) ||
                    !code.fetch8(pc + 2, imm)) {
                    supported = false;
                } else if (addr < 0x80) {
                    native_op = BcOp::MovIramImm;
                    native_a = imm;
                    native_imm = addr;
                    next = pc + 3;
                } else if (addr == 0xE0) {
                    native_op = BcOp::MovSfrImm;
                    native_a = imm;
                    next = pc + 3;
                } else {
                    helper_id = H_MovDirectImm;
                    arg0 = addr;
                    arg1 = imm;
                    next = pc + 3;
                }
            } else if ((op & 0x1F) == 0x11) { // ACALL addr11
                uint8_t lo = 0;
                if (!code.fetch8(pc + 1, lo)) supported = false;
                else {
                    target = static_cast<Addr>(
                        ((pc + 2) & 0xF800u) |
                        (static_cast<uint32_t>(op & 0xE0u) << 3) | lo);
                    helper_id = H_Acall;
                    arg0 = target;
                    arg1 = pc + 2;
                    next = pc + 2;
                    branch = true;
                    dynamic_branch = true;
                }
            } else if (op == 0x12) { // LCALL addr16
                uint8_t hi = 0, lo = 0;
                if (!code.fetch8(pc + 1, hi) ||
                    !code.fetch8(pc + 2, lo)) supported = false;
                else {
                    target = (static_cast<Addr>(hi) << 8) | lo;
                    helper_id = H_Lcall;
                    arg0 = target;
                    arg1 = pc + 3;
                    next = pc + 3;
                    branch = true;
                    dynamic_branch = true;
                }
            } else if (op == 0x22) { // RET
                helper_id = H_Ret;
                next = pc + 1;
                branch = true;
                dynamic_branch = true;
            } else if (op == 0x32) { // RETI
                helper_id = H_Reti;
                next = pc + 1;
                branch = true;
                dynamic_branch = true;
            } else if (op == 0xC0) { // PUSH direct
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else { helper_id = H_Push; arg0 = addr; next = pc + 2; }
            } else if (op == 0xD0) { // POP direct
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else { helper_id = H_Pop; arg0 = addr; next = pc + 2; }
            } else if (op == 0x85) { // MOV direct,direct
                uint8_t src = 0, dst = 0;
                if (!code.fetch8(pc + 1, src) ||
                    !code.fetch8(pc + 2, dst)) supported = false;
                else {
                    helper_id = H_MovDirectDirect;
                    arg0 = static_cast<uint32_t>(src) |
                           (static_cast<uint32_t>(dst) << 8);
                    next = pc + 3;
                }
            } else if (op >= 0x88 && op <= 0x8F) { // MOV direct,Rn
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else {
                    helper_id = H_MovDirectRn;
                    arg0 = addr;
                    arg1 = op & 7;
                    next = pc + 2;
                }
            } else if (op >= 0xA8 && op <= 0xAF) { // MOV Rn,direct
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else {
                    helper_id = H_MovRnDirect;
                    arg0 = op & 7;
                    arg1 = addr;
                    next = pc + 2;
                }
            } else if (op == 0xF6 || op == 0xF7) { // MOV @Ri,A
                helper_id = H_MovIndirectA;
                arg0 = op & 1;
            } else if (op == 0xE6 || op == 0xE7) { // MOV A,@Ri
                helper_id = H_MovAIndirect;
                arg0 = op & 1;
            } else if (op == 0x76 || op == 0x77) { // MOV @Ri,#imm
                uint8_t imm = 0;
                if (!code.fetch8(pc + 1, imm)) supported = false;
                else {
                    helper_id = H_MovIndirectImm;
                    arg0 = op & 1;
                    arg1 = imm;
                    next = pc + 2;
                }
            } else if (op == 0xA6 || op == 0xA7) { // MOV @Ri,direct
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else {
                    helper_id = H_MovIndirectDirect;
                    arg0 = op & 1;
                    arg1 = addr;
                    next = pc + 2;
                }
            } else if (op == 0x86 || op == 0x87) { // MOV direct,@Ri
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else {
                    helper_id = H_MovDirectIndirect;
                    arg0 = addr;
                    arg1 = op & 1;
                    next = pc + 2;
                }
            } else if (op == 0x90) { // MOV DPTR,#imm16
                uint8_t hi = 0, lo = 0;
                if (!code.fetch8(pc + 1, hi) ||
                    !code.fetch8(pc + 2, lo)) supported = false;
                else {
                    helper_id = H_MovDptrImm;
                    arg0 = (static_cast<uint32_t>(hi) << 8) | lo;
                    next = pc + 3;
                }
            } else if (op == 0xD2 || op == 0xC2 || op == 0xB2 ||
                       op == 0xA2 || op == 0x92) { // bit ops
                uint8_t bit = 0;
                if (!code.fetch8(pc + 1, bit)) supported = false;
                else {
                    helper_id = op == 0xD2 ? H_SetbBit
                              : (op == 0xC2 ? H_ClrBit
                              : (op == 0xB2 ? H_CplBit
                              : (op == 0xA2 ? H_MovCBit : H_MovBitC)));
                    arg0 = bit;
                    next = pc + 2;
                }
            } else if (op >= 0xB4 && op <= 0xB7) { // CJNE A,...; rel
                uint8_t operand = 0, rel = 0;
                if (!code.fetch8(pc + 1, operand) ||
                    !code.fetch8(pc + 2, rel)) supported = false;
                else {
                    helper_id = op == 0xB4 ? H_CjneAImm
                              : (op == 0xB5 ? H_CjneADirect
                              : H_CjneAIndirect);
                    target = static_cast<Addr>(
                        static_cast<uint16_t>(pc + 3) +
                        static_cast<int8_t>(rel));
                    arg0 = target;
                    arg1 = op == 0xB5 ? operand : (operand & 1);
                    next = pc + 3;
                    branch = true;
                    dynamic_branch = true;
                }
            } else if (op >= 0xB8 && op <= 0xBF) { // CJNE A,Rn,rel
                uint8_t rel = 0;
                if (!code.fetch8(pc + 2, rel)) supported = false;
                else {
                    helper_id = H_CjneARn;
                    target = static_cast<Addr>(
                        static_cast<uint16_t>(pc + 3) +
                        static_cast<int8_t>(rel));
                    arg0 = target;
                    arg1 = op & 7;
                    next = pc + 3;
                    branch = true;
                    dynamic_branch = true;
                }
            } else if (op == 0xD5) { // DJNZ direct,rel
                uint8_t addr = 0, rel = 0;
                if (!code.fetch8(pc + 1, addr) ||
                    !code.fetch8(pc + 2, rel)) supported = false;
                else {
                    helper_id = H_DjnzDirect;
                    target = static_cast<Addr>(
                        static_cast<uint16_t>(pc + 3) +
                        static_cast<int8_t>(rel));
                    arg0 = target;
                    arg1 = addr;
                    next = pc + 3;
                    branch = true;
                    dynamic_branch = true;
                }
            } else if (op >= 0xD8 && op <= 0xDF) { // DJNZ Rn,rel
                uint8_t rel = 0;
                if (!code.fetch8(pc + 1, rel)) supported = false;
                else {
                    helper_id = H_DjnzRn;
                    target = static_cast<Addr>(
                        static_cast<uint16_t>(pc + 2) +
                        static_cast<int8_t>(rel));
                    arg0 = target;
                    arg1 = op & 7;
                    next = pc + 2;
                    branch = true;
                    dynamic_branch = true;
                }
            } else if (op == 0xA4) {
                helper_id = H_MulAB;
            } else if (op == 0x84) {
                helper_id = H_DivAB;
            } else if (op == 0xD4) {
                helper_id = H_DaA;
            } else if (op == 0xE0) { // MOVX A,@DPTR
                helper_id = H_MovxAdptr;
            } else if (op == 0xF0) { // MOVX @DPTR,A
                helper_id = H_MovxDptrA;
            } else if (op == 0xE2 || op == 0xE3) { // MOVX A,@Ri
                helper_id = H_MovxAIndirect;
                arg1 = op & 1;
            } else if (op == 0xF2 || op == 0xF3) { // MOVX @Ri,A
                helper_id = H_MovxIndirectA;
                arg1 = op & 1;
            } else if (op == 0x83) { // MOVC A,@A+PC
                helper_id = H_MovcApc;
            } else if (op == 0x93) { // MOVC A,@A+DPTR
                helper_id = H_MovcAdptr;
            } else if (op == 0xC5) { // XCH A,direct
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else { helper_id = H_XchADirect; arg1 = addr; next = pc + 2; }
            } else if (op == 0xC6 || op == 0xC7) { // XCH A,@Ri
                helper_id = H_XchAIndirect;
                arg1 = op & 1;
            } else if (op >= 0xC8 && op <= 0xCF) { // XCH A,Rn
                helper_id = H_XchARn;
                arg1 = op & 7;
            } else if (op == 0xD6 || op == 0xD7) { // XCHD A,@Ri
                helper_id = H_XchdAIndirect;
                arg1 = op & 1;
            } else if (op == 0x82) { // ANL C,bit
                uint8_t bit = 0;
                if (!code.fetch8(pc + 1, bit)) supported = false;
                else { helper_id = H_AnlCBit; arg1 = bit; next = pc + 2; }
            } else if (op == 0x72) { // ORL C,bit
                uint8_t bit = 0;
                if (!code.fetch8(pc + 1, bit)) supported = false;
                else { helper_id = H_OrlCBit; arg1 = bit; next = pc + 2; }
            } else if (op == 0xB0) { // ANL C,/bit
                uint8_t bit = 0;
                if (!code.fetch8(pc + 1, bit)) supported = false;
                else { helper_id = H_AnlCNBit; arg1 = bit; next = pc + 2; }
            } else if (op == 0xA0) { // ORL C,/bit
                uint8_t bit = 0;
                if (!code.fetch8(pc + 1, bit)) supported = false;
                else { helper_id = H_OrlCNBit; arg1 = bit; next = pc + 2; }
            } else if (op == 0xC3) { // CLR C
                helper_id = H_ClrC;
            } else if (op == 0xD3) { // SETB C
                helper_id = H_SetbC;
            } else if (op == 0xB3) { // CPL C
                helper_id = H_CplC;
            } else if (op == 0x03) { // RR A
                helper_id = H_RrA;
            } else if (op == 0x13) { // RRC A
                helper_id = H_RrcA;
            } else if (op == 0x23) { // RL A
                helper_id = H_RlA;
            } else if (op == 0x33) { // RLC A
                helper_id = H_RlcA;
            } else if (op == 0xE4) { // CLR A
                helper_id = H_ClrA;
            } else if (op == 0xF4) { // CPL A
                helper_id = H_CplA;
            } else if (op == 0xC4) { // SWAP A
                helper_id = H_SwapA;
            } else if (op == 0x14) { // DEC A
                native_op = BcOp::DecA;
            } else if (op == 0x05) { // INC direct
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else if (addr < 0x80) {
                    native_op = BcOp::IncIram;
                    native_imm = addr;
                    next = pc + 2;
                } else {
                    helper_id = H_IncOperand;
                    arg0 = OM_DIRECT | (static_cast<uint32_t>(addr) << 8);
                    next = pc + 2;
                }
            } else if (op == 0x06 || op == 0x07) { // INC @Ri
                helper_id = H_IncOperand;
                arg0 = OM_INDIRECT | (static_cast<uint32_t>(op & 1) << 8);
            } else if (op >= 0x08 && op <= 0x0F) { // INC Rn
                helper_id = H_IncOperand;
                arg0 = OM_RN | (static_cast<uint32_t>(op & 7) << 8);
            } else if (op == 0xA3) { // INC DPTR
                helper_id = H_IncDptr;
            } else if (op == 0x15) { // DEC direct
                uint8_t addr = 0;
                if (!code.fetch8(pc + 1, addr)) supported = false;
                else if (addr < 0x80) {
                    native_op = BcOp::DecIram;
                    native_imm = addr;
                    next = pc + 2;
                } else {
                    helper_id = H_DecOperand;
                    arg0 = OM_DIRECT | (static_cast<uint32_t>(addr) << 8);
                    next = pc + 2;
                }
            } else if (op == 0x16 || op == 0x17) { // DEC @Ri
                helper_id = H_DecOperand;
                arg0 = OM_INDIRECT | (static_cast<uint32_t>(op & 1) << 8);
            } else if (op >= 0x18 && op <= 0x1F) { // DEC Rn
                helper_id = H_DecOperand;
                arg0 = OM_RN | (static_cast<uint32_t>(op & 7) << 8);
            } else if ((op >= 0x24 && op <= 0x2F) ||
                       (op >= 0x34 && op <= 0x3F) ||
                       (op >= 0x94 && op <= 0x9F)) {
                uint32_t base = op & 0xF0u;
                uint32_t low = op & 0x0Fu;
                helper_id = base == 0x20 ? H_Add
                            : (base == 0x30 ? H_Addc : H_Subb);
                if (low == 0x04) { // #imm
                    uint8_t imm = 0;
                    if (!code.fetch8(pc + 1, imm)) supported = false;
                    else { arg0 = OM_IMM | (uint32_t(imm) << 8); next = pc + 2; }
                } else if (low == 0x05) { // direct
                    uint8_t addr = 0;
                    if (!code.fetch8(pc + 1, addr)) supported = false;
                    else { arg0 = OM_DIRECT | (uint32_t(addr) << 8); next = pc + 2; }
                } else if (low == 0x06 || low == 0x07) {
                    arg0 = OM_INDIRECT | ((low & 1u) << 8);
                } else {
                    arg0 = OM_RN | ((low & 7u) << 8);
                }
            } else if ((op >= 0x42 && op <= 0x4F) ||
                       (op >= 0x52 && op <= 0x5F) ||
                       (op >= 0x62 && op <= 0x6F)) {
                uint32_t base = op & 0xF0u;
                uint32_t low = op & 0x0Fu;
                const bool is_orl = base == 0x40;
                const bool is_anl = base == 0x50;
                if (low == 0x02) { // direct,A
                    uint8_t addr = 0;
                    if (!code.fetch8(pc + 1, addr)) supported = false;
                    else {
                        helper_id = is_orl ? H_OrlDirectA
                                   : (is_anl ? H_AnlDirectA : H_XrlDirectA);
                        arg0 = addr;
                        next = pc + 2;
                    }
                } else if (low == 0x03) { // direct,#imm
                    uint8_t addr = 0, imm = 0;
                    if (!code.fetch8(pc + 1, addr) ||
                        !code.fetch8(pc + 2, imm)) supported = false;
                    else {
                        helper_id = is_orl ? H_OrlDirectImm
                                   : (is_anl ? H_AnlDirectImm : H_XrlDirectImm);
                        arg0 = addr;
                        arg1 = imm;
                        next = pc + 3;
                    }
                } else {
                    helper_id = is_orl ? H_Orl : (is_anl ? H_Anl : H_Xrl);
                    if (low == 0x04) {
                        uint8_t imm = 0;
                        if (!code.fetch8(pc + 1, imm)) supported = false;
                        else { arg0 = OM_IMM | (uint32_t(imm) << 8); next = pc + 2; }
                    } else if (low == 0x05) {
                        uint8_t addr = 0;
                        if (!code.fetch8(pc + 1, addr)) supported = false;
                        else { arg0 = OM_DIRECT | (uint32_t(addr) << 8); next = pc + 2; }
                    } else if (low == 0x06 || low == 0x07) {
                        arg0 = OM_INDIRECT | ((low & 1u) << 8);
                    } else {
                        arg0 = OM_RN | ((low & 7u) << 8);
                    }
                }
            } else if (op == 0x40 || op == 0x50 || op == 0x60 || op == 0x70) {
                uint8_t rel = 0;
                if (!code.fetch8(pc + 1, rel)) supported = false;
                else {
                    helper_id = op == 0x40 ? H_Jc
                              : (op == 0x50 ? H_Jnc
                              : (op == 0x60 ? H_Jz : H_Jnz));
                    target = static_cast<Addr>(
                        static_cast<uint16_t>(pc + 2) +
                        static_cast<int8_t>(rel));
                    arg0 = target;
                    arg1 = 0;
                    next = pc + 2;
                    branch = true;
                    dynamic_branch = true;
                }
            } else if (op == 0x10 || op == 0x20 || op == 0x30) {
                uint8_t bit = 0, rel = 0;
                if (!code.fetch8(pc + 1, bit) ||
                    !code.fetch8(pc + 2, rel)) supported = false;
                else {
                    helper_id = op == 0x10 ? H_Jbc
                              : (op == 0x20 ? H_Jb : H_Jnb);
                    target = static_cast<Addr>(
                        static_cast<uint16_t>(pc + 3) +
                        static_cast<int8_t>(rel));
                    arg0 = target;
                    arg1 = bit;
                    next = pc + 3;
                    branch = true;
                    dynamic_branch = true;
                }
            } else {
                supported = false;
            }

            if (!supported) break;

            if (native_op != BcOp::CallHelper)
                emit(native_op, native_a, 0, native_imm);
            else
                emit(BcOp::CallHelper, helper_id, arg1, arg0);
            emit(BcOp::AddCycles, 0, 0, model.cycles_per_instruction);
            bool linked = false;
            if (branch) {
                emit(BcOp::CheckReg);
                auto bi = block_index.find(target);
                if (dynamic_branch) {
                    if (bi != block_index.end()) {
                        emit(BcOp::BranchIfPc, bi->second, 0, target);
                        linked = true;
                    } else {
                        emit(BcOp::End);
                    }
                } else if (target == start) {
                    emit(BcOp::Branch, 0, 0, 0);
                    linked = true;
                } else if (bi != block_index.end()) {
                    emit(BcOp::Branch, 0, 0, bi->second);
                    linked = true;
                } else {
                    emit(BcOp::End);
                }
            } else {
                emit(BcOp::SetPc, 0, 0, next);
                emit(BcOp::Check, 0, 0, next);
            }

            pc = next;
            ++count;
            if (branch && !linked) break;
            if (branch && linked && !dynamic_branch) break;
        }

        if (count == 0) {
            // 当前 opcode 还没有原生 helper：交给解释器执行一条，保证完整覆盖。
            emit(BcOp::CallIsa);
            emit(BcOp::CheckReg);
            emit(BcOp::End);
            out.guest_len = 1;
            out.cycles = 0;
            out.valid = true;
            out.fallback = true;
            return true;
        }
        emit(BcOp::End);
        out.guest_len = static_cast<uint32_t>(pc - start);
        out.cycles = static_cast<uint32_t>(count);
        out.valid = true;
        return true;
    }

private:
    const Model& model_;
};

} // namespace

extern "C" uint32_t mcus_jit_abi_version() { return kJitAbiVersion; }

extern "C" const char* mcus_jit_arch() { return "8051"; }

extern "C" uint32_t mcus_jit_layout_hash() { return kJitContextLayoutHash; }

extern "C" JitTranslator* mcus_jit_create(const Model* model) {
    return new Jit8051(*model);
}

extern "C" void mcus_jit_destroy(JitTranslator* translator) {
    delete translator;
}

} // namespace mcus
