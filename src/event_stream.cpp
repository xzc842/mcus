#include "mcus/event_stream.h"
#include "mcus/log.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

namespace mcus {

EventStream::EventStream(const std::string& shm_name, size_t capacity)
    : name_(shm_name) {
    if (capacity == 0) capacity = 1;
    total_size_ = sizeof(Header) + capacity * sizeof(MmioEvent);

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
    slots_ = reinterpret_cast<MmioEvent*>(
        static_cast<uint8_t*>(base_) + sizeof(Header));

    // 初始化头（覆盖旧状态）
    header_->head.store(0);
    header_->tail.store(0);
    header_->capacity.store(capacity);
    header_->backpressure.store(0);
}

EventStream::~EventStream() {
    if (base_) {
        ::munmap(base_, total_size_);
    }
    // 不 unlink，便于外部程序仍可打开；由使用者决定何时清理
}

bool EventStream::push(const MmioEvent& ev) {
    uint64_t head = header_->head.load(std::memory_order_relaxed);
    uint64_t tail = header_->tail.load(std::memory_order_acquire);
    uint64_t cap = header_->capacity.load(std::memory_order_relaxed);
    if (head - tail >= cap) {
        header_->backpressure.store(1, std::memory_order_release);
        return false;
    }
    slots_[head % cap] = ev;
    header_->head.store(head + 1, std::memory_order_release);
    return true;
}

bool EventStream::pop(MmioEvent& ev) {
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
    MCUS_DEBUG("stream full check: head=" + std::to_string(head) +
               " tail=" + std::to_string(tail) +
               " cap=" + std::to_string(cap));
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