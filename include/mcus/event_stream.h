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
#include <atomic>
#include <cstddef>
#include <string>

namespace mcus {

// 统一调试/总线事件流：MMIO / PIN / BUS / IRQ / USB。
//
// 一个共享内存对象里包含两个独立的 SPSC 环：
//   out: 模拟器 -> 外部程序（调试事件、总线请求）
//   in : 外部程序 -> 模拟器（总线响应、UART RX、USB 包）
//
// 两个环共用一个全局 seq 分配器，保证跨方向的事件顺序可比较。
//
// create=true  ：创建并初始化共享内存（模拟器侧）
// create=false ：attach 已存在的共享内存，不重置 head/tail（外部程序侧）
class EventStream {
public:
    EventStream(const std::string& shm_name, size_t capacity,
                bool create = true);
    ~EventStream();

    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    const std::string& name() const { return name_; }
    bool attached() const { return attached_; }

    // 模拟器侧：推事件到 out 环。返回 false 表示流满，调用方应触发背压暂停。
    bool push(const DebugEvent& ev);

    // 外部程序侧：从 out 环取事件。返回 false 表示当前为空。
    bool pop(DebugEvent& ev);

    // 外部程序侧：把总线响应/UART RX/USB 包推回模拟器。
    // 返回 false 表示 in 环满，调用方应重试或背压。
    bool push_in(const DebugEvent& ev);

    // 模拟器侧：从 in 环取事件。返回 false 表示当前为空。
    bool pop_in(DebugEvent& ev);

    // out 环状态
    bool full() const;
    bool empty() const;
    size_t capacity() const;
    size_t size() const;

    // in 环状态
    bool in_full() const;
    bool in_empty() const;
    size_t in_size() const;

    // 背压标志：模拟器置位，外部程序可读，用于诊断
    bool backpressure() const;
    void clear_backpressure();
    bool in_backpressure() const;
    void clear_in_backpressure();

    // 是否有外部程序 attach 到本事件流。
    // 同步总线（SPI）据此决定是否等待外部响应；没有外部程序时使用默认 MISO。
    bool host_attached() const;
    void mark_host_attached();

private:
    struct Header {
        std::atomic<uint64_t> out_head;      // 模拟器写位置
        std::atomic<uint64_t> out_tail;      // 外部程序读位置
        std::atomic<uint64_t> in_head;       // 外部程序写位置
        std::atomic<uint64_t> in_tail;       // 模拟器读位置
        std::atomic<uint64_t> capacity;      // 每个环的容量
        std::atomic<uint64_t> seq;           // 全局事件序号
        std::atomic<uint32_t> backpressure_out;
        std::atomic<uint32_t> backpressure_in;
        std::atomic<uint32_t> version;       // 流格式版本
        std::atomic<uint32_t> host_attached; // 1=已有外部程序 attach
    };
    static_assert(sizeof(Header) % alignof(DebugEvent) == 0,
                  "EventStream header must keep DebugEvent alignment");

    void* base_ = nullptr;
    size_t total_size_ = 0;
    std::string name_;
    Header* header_ = nullptr;
    DebugEvent* out_slots_ = nullptr;
    DebugEvent* in_slots_ = nullptr;
    bool attached_ = false;
};

} // namespace mcus
