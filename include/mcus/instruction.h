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
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace mcus {

struct CpuRegisters;

struct ExecResult {
    VirtTime cycles = 0;
    bool halted = false;
};

// 内存总线抽象：按空间访问普通内存和 MMIO
class MemoryBus {
public:
    virtual ~MemoryBus() = default;
    virtual uint8_t read8(const SpaceId& space, Addr addr) = 0;
    virtual uint16_t read16(const SpaceId& space, Addr addr) = 0;
    virtual uint32_t read32(const SpaceId& space, Addr addr) = 0;
    virtual void write8(const SpaceId& space, Addr addr, uint8_t v) = 0;
    virtual void write16(const SpaceId& space, Addr addr, uint16_t v) = 0;
    virtual void write32(const SpaceId& space, Addr addr, uint32_t v) = 0;
    // 架构相关回调：中断返回时恢复优先级状态等，默认空
    virtual void on_reti() {}
};

class InstructionSet {
public:
    virtual ~InstructionSet() = default;
    virtual ExecResult execute(CpuRegisters& regs, MemoryBus& bus) = 0;
};

// ISA 实例销毁器：外部 ISA 库可以导出自己的 destroy，避免跨库 delete
struct IsaDeleter {
    void (*destroy)(InstructionSet*) = nullptr;

    void operator()(InstructionSet* isa) const {
        if (!isa) return;
        if (destroy) destroy(isa);
        else delete isa;
    }
};

using IsaPtr = std::unique_ptr<InstructionSet, IsaDeleter>;
using IsaFactory = std::function<IsaPtr(const Model&)>;

class IsaRegistry {
public:
    static IsaRegistry& instance();
    void register_isa(const std::string& arch, IsaFactory f);
    IsaPtr create(const std::string& arch, const Model& model) const;
private:
    std::unordered_map<std::string, IsaFactory> factories_;
};

// 注册内置 ISA（mcus-minimal，用于测试和最小模型）。
// 具体架构的 ISA（如 8051）由外部动态库提供，见 isa_plugin.h。
void register_all_isas();

} // namespace mcus