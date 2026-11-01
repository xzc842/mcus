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
#include <cstdint>
#include <string>

namespace mcus {

using VirtTime = uint64_t;
using Addr = uint32_t;

using SpaceId = std::string;

enum class MmioAccess : uint8_t { Read = 0, Write = 1 };

// 事件流中的记录。不能包含 std::string 等带堆指针的成员，
// 因为事件流放在 POSIX 共享内存里跨进程共享。
struct MmioEvent {
    VirtTime time;
    Addr addr;
    uint32_t value;
    uint8_t width;
    MmioAccess access;
    uint32_t space_idx;   // 索引到 Model::space_order
};

enum class RegSemantics : uint8_t {
    Plain = 0,
    WriteTrigger = 1,
    ReadClear = 2,
    WriteOneClear = 3,
    ReadOnly = 4,
    WriteOnly = 5,
    Fifo = 6,
};

enum class CpuState : uint8_t { Running = 0, Paused = 1, Halted = 2 };

enum class ControlCommand : uint32_t {
    None = 0, Pause = 1, Resume = 2, Step = 3, Dump = 4, Reset = 5,
};

enum class ControlStatus : uint32_t {
    Running = 0, Paused = 1, DumpDone = 2, Halted = 3,
};

} // namespace mcus