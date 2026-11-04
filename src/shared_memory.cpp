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

#include "mcus/shared_memory.h"
#include "mcus/log.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace mcus {

static size_t align4(size_t x) { return (x + 3) & ~size_t(3); }

SharedMemory::SharedMemory(const Model& model, const std::string& shm_name, bool create)
    : model_(model), name_(shm_name) {
    layout();

    if (!create) {
        // 严格 attach：不创建、不截断、不重置
        int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0600);
        if (fd < 0) throw std::runtime_error("shm not found: " + shm_name);
        struct stat st {};
        if (::fstat(fd, &st) != 0 ||
            st.st_size < static_cast<off_t>(total_size_)) {
            ::close(fd);
            throw std::runtime_error("shm too small: " + shm_name);
        }
        void* p = ::mmap(nullptr, total_size_, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) throw std::runtime_error("mmap failed: " + shm_name);
        base_ = p;
        return;
    }

    int fd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) throw std::runtime_error("shm_open failed: " + shm_name);
    if (::ftruncate(fd, static_cast<off_t>(total_size_)) != 0) {
        ::close(fd);
        throw std::runtime_error("ftruncate failed: " + shm_name);
    }
    void* p = ::mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) throw std::runtime_error("mmap failed: " + shm_name);
    base_ = p;

    std::memset(base_, 0, total_size_);

    auto* ctl = control();
    ctl->command.store(0);
    ctl->status.store(static_cast<uint32_t>(ControlStatus::Running));
    ctl->step_count.store(1);
    ctl->dump_space_idx.store(UINT32_MAX);
    ctl->virtual_time.store(0);
    ctl->pc.store(model_.reset_vector);
    ctl->halted.store(0);

    auto* ih = inject_header();
    ih->capacity.store(static_cast<uint32_t>(kInjectCapacity));
    ih->head.store(0);
    ih->tail.store(0);

    for (const auto& r : model_.regions) {
        if (r.is_mmio &&
            std::find(inject_spaces_.begin(), inject_spaces_.end(), r.space) ==
                inject_spaces_.end()) {
            inject_spaces_.push_back(r.space);
        }
    }

    reset_all();
}

SharedMemory::~SharedMemory() {
    if (base_) ::munmap(base_, total_size_);
}

void SharedMemory::layout() {
    mmio_size_ = model_.mmio_area_size();
    pin_size_ = model_.pin_area_size();
    irq_size_ = model_.irq_bitmap_size();
    inject_slots_size_ = kInjectCapacity * sizeof(InjectSlot);
    dump_size_ = kDumpCapacity;

    control_offset_ = 0;
    mmio_offset_ = align4(control_offset_ + sizeof(ControlBlock));
    pin_offset_ = align4(mmio_offset_ + mmio_size_);
    irq_offset_ = align4(pin_offset_ + pin_size_);
    inject_header_offset_ = align4(irq_offset_ + irq_size_);
    inject_slots_offset_ = align4(inject_header_offset_ + sizeof(InjectHeader));
    dump_offset_ = align4(inject_slots_offset_ + inject_slots_size_);
    breakpoint_header_offset_ = align4(dump_offset_ + dump_size_);
    breakpoint_slots_offset_ = align4(breakpoint_header_offset_ + sizeof(BreakpointHeader));
    breakpoint_slots_size_ = kBreakpointCapacity * sizeof(BreakpointSlot);
    total_size_ = align4(breakpoint_slots_offset_ + breakpoint_slots_size_);

    size_t max_space = 0;
    for (const auto& rd : model_.registers) {
        int sidx = model_.space_index(rd.space);
        if (sidx < 0) continue;
        if (static_cast<size_t>(sidx) > max_space)
            max_space = static_cast<size_t>(sidx);
    }
    reg_offset_fast_.assign((max_space + 2) * 256, SIZE_MAX);

    size_t off = 0;
    for (const auto& rd : model_.registers) {
        std::string key = Model::reg_key(rd.space, rd.addr);
        reg_offset_[key] = off;
        int sidx = model_.space_index(rd.space);
        if (sidx >= 0 && rd.addr < 256) {
            size_t idx = static_cast<size_t>(sidx + 1) * 256 + rd.addr;
            if (idx < reg_offset_fast_.size())
                reg_offset_fast_[idx] = off;
        }
        size_t bytes = (rd.width + 7) / 8;
        off += align4(bytes);
    }
}

ControlBlock* SharedMemory::control() {
    return reinterpret_cast<ControlBlock*>(static_cast<uint8_t*>(base_) + control_offset_);
}
PinState* SharedMemory::pins() {
    return reinterpret_cast<PinState*>(static_cast<uint8_t*>(base_) + pin_offset_);
}
size_t SharedMemory::pin_count() const { return model_.pins.size(); }
uint8_t* SharedMemory::interrupt_bitmap() {
    return static_cast<uint8_t*>(base_) + irq_offset_;
}
size_t SharedMemory::interrupt_bitmap_size() const { return irq_size_; }
InjectHeader* SharedMemory::inject_header() {
    return reinterpret_cast<InjectHeader*>(static_cast<uint8_t*>(base_) + inject_header_offset_);
}
InjectSlot* SharedMemory::inject_slots() {
    return reinterpret_cast<InjectSlot*>(static_cast<uint8_t*>(base_) + inject_slots_offset_);
}
size_t SharedMemory::inject_capacity() const { return kInjectCapacity; }

BreakpointHeader* SharedMemory::breakpoint_header() {
    return reinterpret_cast<BreakpointHeader*>(
        static_cast<uint8_t*>(base_) + breakpoint_header_offset_);
}
BreakpointSlot* SharedMemory::breakpoints() {
    return reinterpret_cast<BreakpointSlot*>(
        static_cast<uint8_t*>(base_) + breakpoint_slots_offset_);
}
size_t SharedMemory::breakpoint_capacity() const { return kBreakpointCapacity; }

uint32_t SharedMemory::read_reg(const SpaceId& space, Addr addr, uint8_t width) const {
    size_t off = SIZE_MAX;
    int sidx = model_.space_index(space);
    if (sidx >= 0 && addr < 256) {
        size_t idx = static_cast<size_t>(sidx + 1) * 256 + addr;
        if (idx < reg_offset_fast_.size())
            off = reg_offset_fast_[idx];
    }
    if (off == SIZE_MAX) {
        auto it = reg_offset_.find(Model::reg_key(space, addr));
        if (it == reg_offset_.end()) return 0;
        off = it->second;
    }
    const uint8_t* p = static_cast<const uint8_t*>(base_) + mmio_offset_ + off;
    switch (width) {
        case 8: return *p;
        case 16: { uint16_t v; std::memcpy(&v, p, 2); return v; }
        default: { uint32_t v; std::memcpy(&v, p, 4); return v; }
    }
}

void SharedMemory::write_reg(const SpaceId& space, Addr addr, uint32_t value, uint8_t width) {
    size_t off = SIZE_MAX;
    int sidx = model_.space_index(space);
    if (sidx >= 0 && addr < 256) {
        size_t idx = static_cast<size_t>(sidx + 1) * 256 + addr;
        if (idx < reg_offset_fast_.size())
            off = reg_offset_fast_[idx];
    }
    if (off == SIZE_MAX) {
        auto it = reg_offset_.find(Model::reg_key(space, addr));
        if (it == reg_offset_.end()) return;
        off = it->second;
    }
    uint8_t* p = static_cast<uint8_t*>(base_) + mmio_offset_ + off;
    switch (width) {
        case 8: { uint8_t v = static_cast<uint8_t>(value); std::memcpy(p, &v, 1); break; }
        case 16: { uint16_t v = static_cast<uint16_t>(value); std::memcpy(p, &v, 2); break; }
        default: { uint32_t v = value; std::memcpy(p, &v, 4); break; }
    }
}

void SharedMemory::reset_all() {
    for (const auto& rd : model_.registers)
        write_reg(rd.space, rd.addr, rd.reset_value, rd.width);
    auto* p = pins();
    for (size_t i = 0; i < model_.pins.size(); ++i) {
        p[i].function.store(model_.pins[i].reset_function);
        p[i].value.store(0);
        p[i].direction.store(0);
    }
    std::memset(interrupt_bitmap(), 0, irq_size_);
    auto* ih = inject_header();
    ih->head.store(0);
    ih->tail.store(0);
    for (size_t i = 0; i < kInjectCapacity; ++i)
        inject_slots()[i].ready.store(0);
    auto* bh = breakpoint_header();
    bh->count.store(0);
    bh->sorted.store(0);
    bh->min_addr.store(0);
    bh->max_addr.store(0);
    bh->hit.store(0);
    bh->hit_addr.store(0);
    for (size_t i = 0; i < kBreakpointCapacity; ++i) {
        breakpoints()[i].addr.store(0);
        breakpoints()[i].enabled.store(0);
    }
    auto* ctl = control();
    ctl->dump_ready.store(0);
    ctl->reset_pin.store(0);
    ctl->halted.store(0);
}

uint8_t* SharedMemory::dump_buffer() {
    return static_cast<uint8_t*>(base_) + dump_offset_;
}
size_t SharedMemory::dump_buffer_size() const { return dump_size_; }

size_t SharedMemory::export_mmio(const SpaceId& space, Addr addr, size_t len,
                                 uint8_t* dst) const {
    size_t copied = 0;
    for (const auto& rd : model_.registers) {
        if (rd.space != space) continue;
        if (rd.addr < addr || rd.addr >= addr + len) continue;
        auto it = reg_offset_.find(Model::reg_key(rd.space, rd.addr));
        if (it == reg_offset_.end()) continue;
        size_t bytes = (rd.width + 7) / 8;
        size_t pos = rd.addr - addr;
        if (pos + bytes > len) bytes = len - pos;
        std::memcpy(dst + pos,
                    static_cast<const uint8_t*>(base_) + mmio_offset_ + it->second,
                    bytes);
        copied = std::max(copied, pos + bytes);
    }
    return copied;
}

} // namespace mcus