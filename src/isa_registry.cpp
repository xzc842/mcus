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

#include "mcus/instruction.h"
#include "mcus/cpu.h"
#include "mcus/log.h"
#include <algorithm>
#include <stdexcept>

namespace mcus {

IsaRegistry& IsaRegistry::instance() {
    static IsaRegistry r;
    return r;
}

void register_all_isas() {
    static bool done = false;
    if (done) return;
    done = true;
    register_isa_8051();
}

void IsaRegistry::register_isa(const std::string& arch, IsaFactory f) {
    factories_[arch] = std::move(f);
}

std::unique_ptr<InstructionSet> IsaRegistry::create(
    const std::string& arch, const Model& model) const {
    auto it = factories_.find(arch);
    if (it == factories_.end()) return nullptr;
    return it->second(model);
}

// ---- 最小通用 ISA（mcus-minimal） ----
// 编码：
//   0x00 NOP
//   0x01 space_id(1B) addr32 value32  写
//   0x02 space_id(1B) addr32          读到 r0
//   0x03 target32                     跳转
//   0x04                              停机
//   0x05 target32                     条件跳转（flags != 0）
//   0x06 space_id(1B) addr32          读 32 位到 r0
//   0x07 space_id(1B) addr32 value32  写 32 位
//   0x08 reg(1B) imm32                加载立即数
//   0x09 reg(1B)                      寄存器跳转
//   0x0A                              flags = r0
//
// space_id 由模型 regions 中的空间名按首次出现顺序编号，
// 具体编号在模型里通过 "space_id" 字段固定更稳妥；
// 这里简化为：0=code, 1=iram, 2=sfr, 3=xram, 4=mmio，
// 模型若使用其他名字，需在 regions 里用 "space_id" 显式指定。

namespace {

class MinimalIsa : public InstructionSet {
public:
    explicit MinimalIsa(const Model& model) : model_(model) {
        // 收集空间名按 regions 顺序去重
        for (const auto& r : model_.regions) {
            if (std::find(spaces_.begin(), spaces_.end(), r.space) == spaces_.end())
                spaces_.push_back(r.space);
        }
    }

    ExecResult execute(CpuRegisters& regs, MemoryBus& bus) override {
        ExecResult res;
        res.cycles = model_.cycles_per_instruction;

        SpaceId pc_space = model_.reset_space;
        Addr pc = regs.pc;
        auto fetch8 = [&](Addr a) { return bus.read8(pc_space, a); };
        auto fetch32 = [&](Addr a) {
            uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= (uint32_t)fetch8(a + i) << (8 * i);
            return v;
        };

        uint8_t op = fetch8(pc++);
        auto space_of = [&](uint8_t id) -> SpaceId {
            if (id < spaces_.size()) return spaces_[id];
            return "mmio";
        };

        switch (op) {
            case 0x00: break;
            case 0x01: {
                uint8_t sid = fetch8(pc++);
                Addr a = fetch32(pc); pc += 4;
                uint32_t v = fetch32(pc); pc += 4;
                bus.write32(space_of(sid), a, v);
                break;
            }
            case 0x02: {
                uint8_t sid = fetch8(pc++);
                Addr a = fetch32(pc); pc += 4;
                regs.r[0] = bus.read32(space_of(sid), a);
                break;
            }
            case 0x03: pc = fetch32(pc); break;
            case 0x04: res.halted = true; break;
            case 0x05: {
                Addr t = fetch32(pc); pc += 4;
                if (regs.flags != 0) pc = t;
                break;
            }
            case 0x06: {
                uint8_t sid = fetch8(pc++);
                Addr a = fetch32(pc); pc += 4;
                regs.r[0] = bus.read32(space_of(sid), a);
                break;
            }
            case 0x07: {
                uint8_t sid = fetch8(pc++);
                Addr a = fetch32(pc); pc += 4;
                uint32_t v = fetch32(pc); pc += 4;
                bus.write32(space_of(sid), a, v);
                break;
            }
            case 0x08: {
                uint8_t reg = fetch8(pc++);
                uint32_t imm = fetch32(pc); pc += 4;
                if (reg < 16) regs.r[reg] = imm;
                break;
            }
            case 0x09: {
                uint8_t reg = fetch8(pc++);
                if (reg < 16) pc = regs.r[reg];
                break;
            }
            case 0x0A: regs.flags = regs.r[0]; break;
            default: res.halted = true; break;
        }
        regs.pc = pc;
        return res;
    }

private:
    const Model& model_;
    std::vector<SpaceId> spaces_;
};

struct MinimalRegistrar {
    MinimalRegistrar() {
        IsaRegistry::instance().register_isa(
            "mcus-minimal",
            [](const Model& m) -> std::unique_ptr<InstructionSet> {
                return std::make_unique<MinimalIsa>(m);
            });
    }
};
MinimalRegistrar g_minimal_registrar;

} // namespace
} // namespace mcus