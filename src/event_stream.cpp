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

// src/event_stream.cpp
#include "mcus/event_stream.h"
#include "mcus/log.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

namespace mcus {

namespace {
constexpr uint32_t kStreamVersion = 3;
} // namespace

EventStream::EventStream(const std::string& shm_name, size_t capacity,
                         bool create)
    : name_(shm_name) {
    if (create && capacity == 0) capacity = 1;

    if (create) {
        total_size_ = sizeof(Header) + 2 * capacity * sizeof(DebugEvent);

        int fd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            throw std::runtime_error("shm_open failed: " + shm_name);
        }
        if (::ftruncate(fd, static_cast<off_t>(total_size_)) != 0) {
            ::close(fd);
            throw std::runtime_error("ftruncate failed: " + shm_name);
        }
        void* p = ::mmap(nullptr, total_size_, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) {
            throw std::runtime_error("mmap failed: " + shm_name);
        }
        base_ = p;
        header_ = static_cast<Header*>(base_);
        out_slots_ = reinterpret_cast<DebugEvent*>(
            static_cast<uint8_t*>(base_) + sizeof(Header));
        in_slots_ = out_slots_ + capacity;

        header_->out_head.store(0);
        header_->out_tail.store(0);
        header_->in_head.store(0);
        header_->in_tail.store(0);
        header_->capacity.store(capacity);
        header_->seq.store(0);
        header_->backpressure_out.store(0);
        header_->backpressure_in.store(0);
        header_->version.store(kStreamVersion);
        header_->host_attached.store(0);
        attached_ = false;
        return;
    }

    // attach：只映射，不重置
    int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0600);
    if (fd < 0) {
        throw std::runtime_error("event stream not found: " + shm_name);
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 ||
        st.st_size < static_cast<off_t>(sizeof(Header))) {
        ::close(fd);
        throw std::runtime_error("invalid event stream: " + shm_name);
    }

    total_size_ = static_cast<size_t>(st.st_size);
    void* p = ::mmap(nullptr, total_size_, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        throw std::runtime_error("mmap failed: " + shm_name);
    }
    base_ = p;
    header_ = static_cast<Header*>(base_);
    out_slots_ = reinterpret_cast<DebugEvent*>(
        static_cast<uint8_t*>(base_) + sizeof(Header));

    const uint64_t cap = header_->capacity.load();
    const uint64_t needed = sizeof(Header) + 2 * cap * sizeof(DebugEvent);
    if (header_->version.load() != kStreamVersion ||
        cap == 0 ||
        total_size_ < needed) {
        ::munmap(base_, total_size_);
        base_ = nullptr;
        throw std::runtime_error("incompatible event stream: " + shm_name);
    }
    in_slots_ = out_slots_ + cap;
    attached_ = true;
    header_->host_attached.store(1, std::memory_order_release);
}

EventStream::~EventStream() {
    if (base_) {
        ::munmap(base_, total_size_);
    }
    // 不 unlink，便于外部程序仍可打开；由使用者决定何时清理
}

bool EventStream::push(const DebugEvent& ev) {
    uint64_t head = header_->out_head.load(std::memory_order_relaxed);
    uint64_t tail = header_->out_tail.load(std::memory_order_acquire);
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    if (head - tail >= cap) {
        header_->backpressure_out.store(1, std::memory_order_release);
        return false;
    }

    DebugEvent copy = ev;
    copy.seq = header_->seq.fetch_add(1, std::memory_order_relaxed);
    out_slots_[head % cap] = copy;
    header_->out_head.store(head + 1, std::memory_order_release);
    return true;
}

bool EventStream::pop(DebugEvent& ev) {
    uint64_t tail = header_->out_tail.load(std::memory_order_relaxed);
    uint64_t head = header_->out_head.load(std::memory_order_acquire);
    if (tail == head) return false;
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    ev = out_slots_[tail % cap];
    header_->out_tail.store(tail + 1, std::memory_order_release);
    return true;
}

bool EventStream::push_in(const DebugEvent& ev) {
    uint64_t head = header_->in_head.load(std::memory_order_relaxed);
    uint64_t tail = header_->in_tail.load(std::memory_order_acquire);
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    if (head - tail >= cap) {
        header_->backpressure_in.store(1, std::memory_order_release);
        return false;
    }

    DebugEvent copy = ev;
    copy.seq = header_->seq.fetch_add(1, std::memory_order_relaxed);
    in_slots_[head % cap] = copy;
    header_->in_head.store(head + 1, std::memory_order_release);
    return true;
}

bool EventStream::pop_in(DebugEvent& ev) {
    uint64_t tail = header_->in_tail.load(std::memory_order_relaxed);
    uint64_t head = header_->in_head.load(std::memory_order_acquire);
    if (tail == head) return false;
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    ev = in_slots_[tail % cap];
    header_->in_tail.store(tail + 1, std::memory_order_release);
    return true;
}

bool EventStream::full() const {
    uint64_t head = header_->out_head.load(std::memory_order_acquire);
    uint64_t tail = header_->out_tail.load(std::memory_order_acquire);
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    return (head - tail) >= cap;
}

bool EventStream::empty() const {
    uint64_t head = header_->out_head.load(std::memory_order_acquire);
    uint64_t tail = header_->out_tail.load(std::memory_order_acquire);
    return head == tail;
}

size_t EventStream::capacity() const {
    return static_cast<size_t>(header_->capacity.load());
}

size_t EventStream::size() const {
    uint64_t head = header_->out_head.load(std::memory_order_acquire);
    uint64_t tail = header_->out_tail.load(std::memory_order_acquire);
    return static_cast<size_t>(head - tail);
}

bool EventStream::in_full() const {
    uint64_t head = header_->in_head.load(std::memory_order_acquire);
    uint64_t tail = header_->in_tail.load(std::memory_order_acquire);
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    return (head - tail) >= cap;
}

bool EventStream::in_empty() const {
    uint64_t head = header_->in_head.load(std::memory_order_acquire);
    uint64_t tail = header_->in_tail.load(std::memory_order_acquire);
    return head == tail;
}

size_t EventStream::in_size() const {
    uint64_t head = header_->in_head.load(std::memory_order_acquire);
    uint64_t tail = header_->in_tail.load(std::memory_order_acquire);
    return static_cast<size_t>(head - tail);
}

bool EventStream::backpressure() const {
    return header_->backpressure_out.load(std::memory_order_acquire) != 0;
}

void EventStream::clear_backpressure() {
    header_->backpressure_out.store(0, std::memory_order_release);
}

bool EventStream::in_backpressure() const {
    return header_->backpressure_in.load(std::memory_order_acquire) != 0;
}

void EventStream::clear_in_backpressure() {
    header_->backpressure_in.store(0, std::memory_order_release);
}

bool EventStream::host_attached() const {
    return header_->host_attached.load(std::memory_order_acquire) != 0;
}

void EventStream::mark_host_attached() {
    header_->host_attached.store(1, std::memory_order_release);
}

} // namespace mcus
