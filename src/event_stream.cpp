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
constexpr uint32_t kStreamVersion = 2;
} // namespace

EventStream::EventStream(const std::string& shm_name, size_t capacity,
                         bool create)
    : name_(shm_name) {
    if (create && capacity == 0) capacity = 1;

    if (create) {
        total_size_ = sizeof(Header) + capacity * sizeof(DebugEvent);

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
        slots_ = reinterpret_cast<DebugEvent*>(
            static_cast<uint8_t*>(base_) + sizeof(Header));

        header_->head.store(0);
        header_->tail.store(0);
        header_->capacity.store(capacity);
        header_->backpressure.store(0);
        header_->version.store(kStreamVersion);
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
    slots_ = reinterpret_cast<DebugEvent*>(
        static_cast<uint8_t*>(base_) + sizeof(Header));
    attached_ = true;

    const uint64_t cap = header_->capacity.load();
    if (header_->version.load() != kStreamVersion ||
        cap == 0 ||
        total_size_ < sizeof(Header) + cap * sizeof(DebugEvent)) {
        ::munmap(base_, total_size_);
        base_ = nullptr;
        throw std::runtime_error("incompatible event stream: " + shm_name);
    }
}

EventStream::~EventStream() {
    if (base_) {
        ::munmap(base_, total_size_);
    }
    // 不 unlink，便于外部程序仍可打开；由使用者决定何时清理
}

bool EventStream::push(const DebugEvent& ev) {
    uint64_t head = header_->head.load(std::memory_order_relaxed);
    uint64_t tail = header_->tail.load(std::memory_order_acquire);
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    if (head - tail >= cap) {
        header_->backpressure.store(1, std::memory_order_release);
        return false;
    }

    DebugEvent copy = ev;
    copy.seq = next_seq_++;
    slots_[head % cap] = copy;
    header_->head.store(head + 1, std::memory_order_release);
    return true;
}

bool EventStream::pop(DebugEvent& ev) {
    uint64_t tail = header_->tail.load(std::memory_order_relaxed);
    uint64_t head = header_->head.load(std::memory_order_acquire);
    if (tail == head) return false;
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    ev = slots_[tail % cap];
    header_->tail.store(tail + 1, std::memory_order_release);
    return true;
}

bool EventStream::full() const {
    uint64_t head = header_->head.load(std::memory_order_acquire);
    uint64_t tail = header_->tail.load(std::memory_order_acquire);
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    if (head - tail >= cap) {
        MCUS_WARN("event stream full: head=" + std::to_string(head) +
                  " tail=" + std::to_string(tail) +
                  " cap=" + std::to_string(cap));
    }
    return (head - tail) >= cap;
}

bool EventStream::empty() const {
    uint64_t head = header_->head.load(std::memory_order_acquire);
    uint64_t tail = header_->tail.load(std::memory_order_acquire);
    return head == tail;
}

size_t EventStream::capacity() const {
    return static_cast<size_t>(header_->capacity.load());
}

size_t EventStream::size() const {
    uint64_t head = header_->head.load(std::memory_order_acquire);
    uint64_t tail = header_->tail.load(std::memory_order_acquire);
    return static_cast<size_t>(head - tail);
}

bool EventStream::backpressure() const {
    return header_->backpressure.load(std::memory_order_acquire) != 0;
}

void EventStream::clear_backpressure() {
    header_->backpressure.store(0, std::memory_order_release);
}

} // namespace mcus
