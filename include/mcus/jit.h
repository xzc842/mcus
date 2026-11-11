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

// JIT 插件接口：每个 guest 架构一个 Translator 动态库。
//
// Translator 只负责 guest 指令 -> 可移植字节码；
// 宿主代码生成由 mcus_core 的后端完成，因此同一个插件可在不同电脑复用。
#pragma once

#include "bytecode.h"
#include "instruction.h"
#include "model.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mcus {

inline constexpr uint32_t kJitAbiVersion = 1;

inline constexpr const char* kJitAbiVersionSymbol = "mcus_jit_abi_version";
inline constexpr const char* kJitArchSymbol = "mcus_jit_arch";
inline constexpr const char* kJitCreateSymbol = "mcus_jit_create";
inline constexpr const char* kJitDestroySymbol = "mcus_jit_destroy";

// guest 代码源。code 指向代码空间基址，pc 是绝对地址。
struct JitCodeSource {
    const uint8_t* code = nullptr;
    size_t size = 0;
    Addr base = 0;

    bool fetch8(Addr addr, uint8_t& out) const {
        if (!code || addr < base) return false;
        const size_t off = static_cast<size_t>(addr - base);
        if (off >= size) return false;
        out = code[off];
        return true;
    }
};

struct JitContext;
using JitHelperFn = uint32_t (*)(JitContext* ctx, uint32_t id, uint32_t a,
                                uint32_t b);

class JitTranslator {
public:
    virtual ~JitTranslator() = default;

    // 翻译 pc 开始的一条/一段 guest 指令。成功返回 true，out 至少包含
    // AddCycles / Check / End，并且 guest_pc/guest_len/cycles 有效。
    virtual bool translate(const Model& model, const JitCodeSource& code,
                           Addr pc, BcProgram& out) = 0;

    // CallHelper 的统一入口；返回 nullptr 表示不支持 helper。
    virtual JitHelperFn helper() const { return nullptr; }
};

using JitCreateFn = JitTranslator* (*)(const Model*);
using JitDestroyFn = void (*)(JitTranslator*);

// 字节码执行上下文。JIT 后端/解释器都通过它访问 guest 状态。
using JitReadFn = uint32_t (*)(void* user, uint8_t space, Addr addr,
                               uint8_t width);
using JitWriteFn = void (*)(void* user, uint8_t space, Addr addr,
                            uint32_t value, uint8_t width);
using JitCheckFn = uint32_t (*)(void* user, uint32_t guest_pc);
using JitCallIsaFn = bool (*)(void* user, uint32_t guest_pc);
using JitRetiFn = void (*)(void* user);

// 已编译的宿主机器码。
struct NativeProgram {
    void* code = nullptr;
    size_t size = 0;
    uint32_t guest_pc = 0;
    bool valid = false;
};

struct JitContext {
    CpuRegisters regs;
    VirtTime now = 0;
    uint64_t steps = 0;
    uint64_t max_steps = 0;
    bool halted = false;

    // 模型空间索引，由 Cpu 填入。
    uint8_t code_space = 0;
    uint8_t iram_space = 0;
    uint8_t sfr_space = 0;
    uint8_t xram_space = 0;

    // 直接内存基址，供 native 快速路径使用。
    uint8_t* iram_data = nullptr;
    int32_t iram_bias = 0;
    uint8_t* sfr_data = nullptr;
    int32_t sfr_bias = 0;
    uint8_t* acc_ptr = nullptr;
    uint8_t* psw_ptr = nullptr;
    uint8_t* sfr_mmio = nullptr;
    int32_t sfr_offsets[256]{}; // SFR 地址 -> mmio 偏移，-1 表示不存在
    uint8_t* xram_data = nullptr;
    int32_t xram_bias = 0;

    void* user = nullptr;
    JitReadFn read = nullptr;
    JitWriteFn write = nullptr;
    JitCheckFn check = nullptr;
    JitCallIsaFn call_isa = nullptr;
    JitRetiFn reti = nullptr;
    JitHelperFn helper = nullptr;
};

// 核心侧 JIT 引擎：加载插件、翻译、执行字节码。
class JitEngine {
public:
    JitEngine();
    ~JitEngine();

    JitEngine(const JitEngine&) = delete;
    JitEngine& operator=(const JitEngine&) = delete;

    bool load_plugin(const std::string& path, const std::string& arch);
    void unload();

    bool loaded() const { return handle_ != nullptr && create_ != nullptr; }
    const std::string& plugin_arch() const { return arch_; }

    bool translate(const Model& model, const JitCodeSource& code, Addr pc,
                   BcProgram& out) const;

    JitHelperFn helper() const {
        return translator_ ? translator_->helper() : nullptr;
    }

    // 执行一段字节码直到 End、Check 返回非零、或达到 max_steps。
    bool execute(const BcProgram& program, JitContext& ctx) const;

    // 把字节码编译成当前宿主的机器码。不支持的 op 返回 false。
    bool compile(const BcProgram& program, NativeProgram& out) const;
    void execute_native(const NativeProgram& program, JitContext& ctx) const;
    void release(NativeProgram& program) const;

private:
    void* handle_ = nullptr;
    JitCreateFn create_ = nullptr;
    JitTranslator* translator_ = nullptr;
    JitDestroyFn destroy_ = nullptr;
    std::string arch_;
};

} // namespace mcus
