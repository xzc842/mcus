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

// include/mcus/isa_plugin.h
#pragma once

#include "instruction.h"

#include <cstdint>

namespace mcus {

// ISA 动态库（.so）的 C ABI 入口。
//
// 插件必须导出以下四个符号：
//   uint32_t                mcus_isa_abi_version();
//   const char*             mcus_isa_arch();
//   InstructionSet*         mcus_isa_create(const Model* model);
//   void                    mcus_isa_destroy(InstructionSet* isa);
//
// 模拟器先校验 ABI 版本和 arch，再调用 mcus_isa_create 创建指令集实例。
inline constexpr uint32_t kIsaAbiVersion = 1;

inline constexpr const char* kIsaAbiVersionSymbol = "mcus_isa_abi_version";
inline constexpr const char* kIsaArchSymbol    = "mcus_isa_arch";
inline constexpr const char* kIsaCreateSymbol  = "mcus_isa_create";
inline constexpr const char* kIsaDestroySymbol = "mcus_isa_destroy";

using IsaAbiVersionFn = uint32_t (*)();
using IsaArchFn    = const char* (*)();
using IsaCreateFn  = InstructionSet* (*)(const Model*);
using IsaDestroyFn = void (*)(InstructionSet*);

} // namespace mcus
