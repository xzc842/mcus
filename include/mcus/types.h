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

// 引脚状态变化。changed 位掩码：1=value，2=function，4=direction
struct PinEvent {
    uint32_t pin_id;
    uint32_t value;
    uint32_t function;
    uint32_t direction;
    uint32_t changed;
};

enum class BusEventKind : uint8_t {
    SpiXfer = 0,
    I2cStart = 1,
    I2cAddr = 2,
    I2cData = 3,
    I2cStop = 4,
    UartTx = 5,
    UartRx = 6,
    PinEdge = 7,
};

enum class BusDirection : uint8_t { ToHost = 0, ToMcu = 1 };

// 总线事务。data 内联 64 字节，超过时分片。
struct BusEvent {
    uint32_t bus_id;
    uint32_t xfer_id;
    uint16_t len;
    uint8_t kind;        // BusEventKind
    uint8_t direction;   // BusDirection
    uint8_t flags;       // CS/ACK/NACK/READ/WRITE 等
    uint8_t reserved;
    uint8_t data[64];
};

struct IrqEvent {
    uint32_t number;
    uint32_t pending;
};

enum class DebugEventType : uint8_t {
    Mmio = 0,
    Pin = 1,
    Bus = 2,
    Irq = 3,
};

// 统一调试事件：MMIO / GPIO / 总线 / 中断。
// seq 由 EventStream::push 分配，保证跨类型顺序。
struct DebugEvent {
    uint64_t seq;
    VirtTime time;
    DebugEventType type;
    uint8_t reserved[7];
    union {
        MmioEvent mmio;
        PinEvent pin;
        BusEvent bus;
        IrqEvent irq;
    } u;
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