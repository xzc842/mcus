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
#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>

namespace mcus {

struct ControlBlock {
    std::atomic<uint32_t> command;
    std::atomic<uint32_t> status;
    std::atomic<uint32_t> step_count;
    std::atomic<uint32_t> dump_addr;
    std::atomic<uint32_t> dump_len;       // 入参请求长度，dump 完成后写回实际长度
    std::atomic<uint32_t> dump_space_idx; // Model::space_order 索引；UINT32_MAX=自动
    std::atomic<uint32_t> dump_ready;
    std::atomic<uint32_t> reset_pin;
    std::atomic<uint64_t> virtual_time;
    std::atomic<uint32_t> pc;
    std::atomic<uint32_t> halted;
};

struct PinState {
    std::atomic<uint32_t> function;
    std::atomic<uint32_t> value;
    std::atomic<uint32_t> direction;
};

struct InjectSlot {
    std::atomic<uint32_t> addr;
    std::atomic<uint32_t> value;
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> ready;
    std::atomic<uint32_t> space_idx;
};

struct InjectHeader {
    std::atomic<uint32_t> capacity;
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
};

// 断点：外部程序写 addr/enabled，按 addr 升序，设 sorted=1，
// 模拟器二分查找。min_addr/max_addr 用于快速范围过滤。
struct BreakpointHeader {
    std::atomic<uint32_t> count;      // 当前断点数
    std::atomic<uint32_t> sorted;     // 1=按 addr 升序
    std::atomic<uint32_t> min_addr;   // 最小断点地址
    std::atomic<uint32_t> max_addr;   // 最大断点地址
    std::atomic<uint32_t> hit;        // 0=未命中，1=命中
    std::atomic<uint32_t> hit_addr;   // 命中的地址
};

struct BreakpointSlot {
    std::atomic<uint32_t> addr;       // 断点地址
    std::atomic<uint32_t> enabled;    // 1=启用
};

class SharedMemory {
public:
    SharedMemory(const Model& model, const std::string& shm_name, bool create = true);
    ~SharedMemory();
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;

    const std::string& name() const { return name_; }
    size_t total_size() const { return total_size_; }

    ControlBlock* control();
    PinState* pins();
    size_t pin_count() const;
    // 外部驱动输入：写值并在尾部 hold 位图中标记，防止 CPU 端口写覆盖。
    void set_pin_value(size_t pin, uint32_t value, bool hold = true) {
        if (pin >= pin_count()) return;
        pins()[pin].value.store(value);
        pin_hold()[pin] = hold ? 1u : 0u;
    }
    void release_pin(size_t pin) {
        if (pin >= pin_count()) return;
        pin_hold()[pin] = 0;
    }
    uint8_t* pin_hold();
    size_t pin_hold_size() const { return model_.pins.size(); }
    uint8_t* interrupt_bitmap();
    size_t interrupt_bitmap_size() const;
    InjectHeader* inject_header();
    InjectSlot* inject_slots();
    size_t inject_capacity() const;

    BreakpointHeader* breakpoint_header();
    BreakpointSlot* breakpoints();
    size_t breakpoint_capacity() const;

    uint32_t read_reg(const SpaceId& space, Addr addr, uint8_t width) const;
    void write_reg(const SpaceId& space, Addr addr, uint32_t value, uint8_t width);
    // 热路径：先用 reg_offset() 查一次偏移，之后直接按偏移读写。
    size_t reg_offset(const SpaceId& space, Addr addr) const;
    uint32_t read_reg_at(size_t off, uint8_t width) const {
        const uint8_t* p = static_cast<const uint8_t*>(base_) + mmio_offset_ + off;
        switch (width) {
            case 8: return *p;
            case 16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
            default: { uint32_t v; std::memcpy(&v, p, 4); return v; }
        }
    }
    void write_reg_at(size_t off, uint32_t value, uint8_t width) {
        uint8_t* p = static_cast<uint8_t*>(base_) + mmio_offset_ + off;
        switch (width) {
            case 8: { uint8_t v = static_cast<uint8_t>(value); std::memcpy(p, &v, 1); break; }
            case 16: { uint16_t v = static_cast<uint16_t>(value); std::memcpy(p, &v, 2); break; }
            default: { uint32_t v = value; std::memcpy(p, &v, 4); break; }
        }
    }
    void reset_all();

    uint8_t* dump_buffer();
    size_t dump_buffer_size() const;
    size_t export_mmio(const SpaceId& space, Addr addr, size_t len,
                       uint8_t* dst) const;

private:
    void layout();

    const Model& model_;
    std::string name_;
    void* base_ = nullptr;
    size_t total_size_ = 0;

    size_t control_offset_ = 0;
    size_t mmio_offset_ = 0;
    size_t pin_offset_ = 0;
    size_t irq_offset_ = 0;
    size_t inject_header_offset_ = 0;
    size_t inject_slots_offset_ = 0;
    size_t dump_offset_ = 0;
    size_t breakpoint_header_offset_ = 0;
    size_t breakpoint_slots_offset_ = 0;

    size_t mmio_size_ = 0;
    size_t pin_size_ = 0;
    size_t irq_size_ = 0;
    size_t inject_slots_size_ = 0;
    size_t dump_size_ = 0;
    size_t breakpoint_slots_size_ = 0;
    size_t pin_hold_offset_ = 0;
    size_t pin_hold_size_ = 0;

    std::unordered_map<std::string, size_t> reg_offset_;
    std::unordered_map<uint64_t, size_t> reg_offset_u64_; // (space_idx<<32)|addr
    std::vector<size_t> reg_offset_fast_;
    std::vector<SpaceId> inject_spaces_;

    static constexpr size_t kInjectCapacity = 64;
    static constexpr size_t kDumpCapacity = 256 * 1024;
    static constexpr size_t kBreakpointCapacity = 32;
};

} // namespace mcus