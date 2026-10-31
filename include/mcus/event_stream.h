#pragma once
#include "types.h"
#include <atomic>
#include <cstddef>
#include <string>

namespace mcus {

// 环形队列 + 背压标志。模拟器写满时置 backpressure，CPU 线程暂停推进。
// 外部程序通过同一个共享内存对象消费事件。
class EventStream {
public:
    EventStream(const std::string& shm_name, size_t capacity);
    ~EventStream();

    EventStream(const EventStream&) = delete;
    EventStream& operator=(const EventStream&) = delete;

    const std::string& name() const { return name_; }

    // 模拟器侧：推事件。返回 false 表示流满，调用方应触发背压暂停。
    bool push(const MmioEvent& ev);

    // 外部程序侧：取事件。返回 false 表示当前为空。
    bool pop(MmioEvent& ev);

    bool full() const;
    bool empty() const;
    size_t capacity() const;
    size_t size() const;

    // 背压标志：模拟器置位，外部程序可读，用于诊断
    bool backpressure() const;
    void clear_backpressure();

private:
    struct Header {
        std::atomic<uint64_t> head;         // 模拟器写位置
        std::atomic<uint64_t> tail;         // 外部程序读位置
        std::atomic<uint64_t> capacity;
        std::atomic<uint32_t> backpressure; // 1 表示流满过
    };

    void* base_ = nullptr;
    size_t total_size_ = 0;
    std::string name_;
    Header* header_ = nullptr;
    MmioEvent* slots_ = nullptr;
};

} // namespace mcus