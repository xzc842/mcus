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

// 可移植字节码：JIT 插件的输出格式，也是磁盘缓存格式。
//
// 设计原则：
//   - 固定 8 字节指令，顺序扫描，无变长编码；
//   - 只描述 guest 语义，不出现宿主寄存器/地址；
//   - 复杂语义走 CallHelper，保持后端简单；
//   - 每条 guest 指令后跟一条 Check，保证逐指令完整检查。
#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mcus {

inline constexpr uint32_t kBytecodeVersion = 3;

// 字节码操作码。数值一经发布不要改动，新增用新值。
enum class BcOp : uint16_t {
    Nop = 0,
    LoadImm = 1,      // a=dst, imm=value
    LoadReg = 2,      // a=dst, b=src
    StoreReg = 3,     // a=dst, b=src
    LoadMem = 4,      // a=dst, b=space, imm=addr
    StoreMem = 5,     // a=space, b=src, imm=addr
    Add = 6,          // a=dst, b=src, imm=src2(reg) or literal
    Sub = 7,
    And = 8,
    Or = 9,
    Xor = 10,
    Shl = 11,
    Shr = 12,
    Cmp = 13,
    SetFlags = 14,
    Branch = 15,      // imm=target instruction index
    CondBranch = 16,  // a=condition, imm=target instruction index
    CallHelper = 17,  // a=helper id, b=arg1(8bit), imm=arg0(32bit)
    CallIsa = 18,     // 回退到解释器执行一条指令
    AddCycles = 19,   // imm=cycles
    Check = 20,       // imm=guest pc；返回非零退出到慢路径
    Trap = 21,        // a=reason
    End = 22,
    LoadMem8 = 23,
    LoadMem16 = 24,
    LoadMem32 = 25,
    StoreMem8 = 26,
    StoreMem16 = 27,
    StoreMem32 = 28,
    SetPc = 29,       // imm=guest pc
    CheckReg = 30,    // 用 ctx.regs.pc 做 Check（分支后）
    MovAImm = 31,     // a=imm
    IncA = 32,
    DecA = 33,
    MovSfrA = 34,     // imm=sfr addr
    MovIramA = 35,    // imm=iram addr
    MovASfr = 36,     // imm=sfr addr
    MovAIram = 37,    // imm=iram addr
    MovSfrImm = 38,   // imm=addr, a=imm
    MovIramImm = 39,  // imm=addr, a=imm
    IncIram = 40,     // imm=addr
    DecIram = 41,     // imm=addr
    BranchIfPc = 42,  // a=code index, imm=guest pc
    AnlAImm = 43,     // a=imm
    OrlAImm = 44,     // a=imm
    XrlAImm = 45,     // a=imm
    Count
};

enum class BcCond : uint8_t {
    Always = 0,
    Zero = 1,
    NotZero = 2,
    Carry = 3,
    NotCarry = 4,
    Negative = 5,
    Positive = 6,
};

enum class BcWidth : uint8_t {
    W8 = 1,
    W16 = 2,
    W32 = 4,
    W64 = 8,
};

struct BcInsn {
    uint16_t op = 0;
    uint8_t a = 0;
    uint8_t b = 0;
    uint32_t imm = 0;
};

static_assert(sizeof(BcInsn) == 8, "BcInsn must stay 8 bytes");

struct BcLabel {
    uint32_t guest_pc = 0;
    uint32_t code_index = 0;
};

// 一段字节码程序。通常对应一个 guest 基本块；块内每条指令仍要 Check。
struct BcProgram {
    uint32_t version = kBytecodeVersion;
    std::string arch;
    Addr guest_pc = 0;
    uint32_t guest_len = 0;
    uint32_t cycles = 0;
    std::vector<BcInsn> code;
    std::vector<uint32_t> consts;
    std::vector<BcLabel> labels;
    std::vector<std::string> spaces;  // space id 到 BcInsn.b/imm 的映射
    bool valid = false;
    bool fallback = false; // true 表示这里只生成了 CallIsa 回退桩

    size_t size_bytes() const { return code.size() * sizeof(BcInsn); }
};

// 序列化用的简单头，后续磁盘缓存直接复用。所有字段 little-endian。
struct BcFileHeader {
    char magic[4] = {'M', 'C', 'B', 'C'};
    uint32_t version = kBytecodeVersion;
    uint32_t flags = 0;
    uint32_t code_count = 0;
    uint32_t const_count = 0;
    uint32_t label_count = 0;
    uint32_t guest_pc = 0;
    uint32_t guest_len = 0;
    uint32_t cycles = 0;
    uint32_t spaces_count = 0;
    uint64_t payload_hash = 0;
};

} // namespace mcus
