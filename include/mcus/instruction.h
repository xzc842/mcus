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
    // 取指快路径：默认走 code 空间；Cpu 会覆盖为模型复位空间的直接块访问。
    virtual uint8_t fetch8(Addr addr) { return read8("code", addr); }
    virtual uint16_t fetch16(Addr addr) { return read16("code", addr); }
    // 架构相关回调：中断返回时恢复优先级状态等，默认空
    virtual void on_reti() {}
};

class InstructionSet {
public:
    virtual ~InstructionSet() = default;

    // 执行一条指令
    virtual ExecResult execute(CpuRegisters& regs, MemoryBus& bus) = 0;

    // ---- 架构钩子：核心 Cpu 不假设具体架构 ----
    // 复位：设置 PC/SP/架构寄存器。默认按 Model::arch_desc。
    virtual void reset(CpuRegisters& regs, MemoryBus& bus, const Model& model);

    // 中断进入前保存上下文（默认按 arch_desc.irq_push_bytes 压 PC）
    virtual void save_context(CpuRegisters& regs, MemoryBus& bus,
                              const Model& model);

    // 中断返回时恢复上下文（默认空；8051 恢复 IP）
    virtual void restore_context(CpuRegisters& regs, MemoryBus& bus,
                                 const Model& model);

    // 中断向量：默认用模型里的 vector_addr；ARM 可读向量表
    virtual Addr interrupt_vector(const InterruptDesc& irq, MemoryBus& bus,
                                  const Model& model) const;

    // 中断应答：默认清 pending 位；ARM/NVIC 可改写 1 清零
    virtual void acknowledge_interrupt(const InterruptDesc& irq,
                                       MemoryBus& bus, const Model& model);

    // 位地址解析（可选；8051 使用）
    virtual bool resolve_bit_addr(const Model& model, Addr bit_addr,
                                  SpaceId& space, Addr& byte_addr,
                                  uint8_t& bit) const;

    // MMIO 读/写后的架构相关副作用（默认空）
    virtual void on_mmio_write(const Model& model, MemoryBus& bus,
                               const SpaceId& space, Addr addr,
                               uint32_t value, uint8_t width);
    virtual void on_mmio_read(const Model& model, MemoryBus& bus,
                              const SpaceId& space, Addr addr, uint8_t width);

    // 中断优先级：返回 true 表示 a 比 b 更优先。默认按 priority 数值小者优先。
    virtual bool higher_priority(const InterruptDesc& a,
                                 const InterruptDesc& b) const;
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