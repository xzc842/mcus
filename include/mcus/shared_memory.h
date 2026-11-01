#pragma once
#include "types.h"
#include "model.h"
#include <atomic>
#include <cstddef>
#include <string>

namespace mcus {

struct ControlBlock {
    std::atomic<uint32_t> command;
    std::atomic<uint32_t> status;
    std::atomic<uint32_t> step_count;
    std::atomic<uint32_t> dump_addr;
    std::atomic<uint32_t> dump_len;
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
    // space 用固定长度字符串不现实，注入区只支持 SFR/MMIO，
    // 具体空间由模型决定：约定注入区的空间名由 slot.space_idx 索引
    std::atomic<uint32_t> space_idx;
};

struct InjectHeader {
    std::atomic<uint32_t> capacity;
    std::atomic<uint32_t> head;
    std::atomic<uint32_t> tail;
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
    uint8_t* interrupt_bitmap();
    size_t interrupt_bitmap_size() const;
    InjectHeader* inject_header();
    InjectSlot* inject_slots();
    size_t inject_capacity() const;

    uint32_t read_reg(const SpaceId& space, Addr addr, uint8_t width) const;
    void write_reg(const SpaceId& space, Addr addr, uint32_t value, uint8_t width);
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

    size_t mmio_size_ = 0;
    size_t pin_size_ = 0;
    size_t irq_size_ = 0;
    size_t inject_slots_size_ = 0;
    size_t dump_size_ = 0;

    std::unordered_map<std::string, size_t> reg_offset_;   // 改 key 为 string
    std::vector<size_t> reg_offset_fast_;
    std::vector<SpaceId> inject_spaces_;

    static constexpr size_t kInjectCapacity = 64;
    static constexpr size_t kDumpCapacity = 256 * 1024;
};

} // namespace mcus