#pragma once
#include "types.h"
#include "model.h"
#include "shared_memory.h"
#include "event_stream.h"
#include "instruction.h"
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mcus {

struct CpuRegisters {
    uint32_t r[16]{};
    uint32_t pc = 0;
    // 注意：8051 的 SP 权威来源是 SFR 0x81，不是这个字段。
    // 该字段仅为通用 ISA 预留，8051 ISA 不应读写它。
    uint32_t sp = 0;
    uint32_t flags = 0;
};

class Cpu : public MemoryBus {
public:
    Cpu(const Model& model, SharedMemory& shm, EventStream& stream);
    VirtTime step();
    bool halted() const { return halted_.load(); }
    bool paused() const { return paused_.load(); }
    void set_paused(bool p) { paused_.store(p); }
    const CpuRegisters& regs() const { return regs_; }

    void load_firmware(const std::vector<uint8_t>& firmware);
    void reset();
    size_t export_memory(const SpaceId& space, Addr addr, size_t len, uint8_t* dst) const;

    uint8_t read8(const SpaceId& space, Addr addr) override;
    uint16_t read16(const SpaceId& space, Addr addr) override;
    uint32_t read32(const SpaceId& space, Addr addr) override;
    void write8(const SpaceId& space, Addr addr, uint8_t v) override;
    void write16(const SpaceId& space, Addr addr, uint16_t v) override;
    void write32(const SpaceId& space, Addr addr, uint32_t v) override;
    void on_reti() override;

private:
    const Model& model_;
    SharedMemory& shm_;
    EventStream& stream_;
    std::unique_ptr<InstructionSet> isa_;
    CpuRegisters regs_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> halted_{false};

    struct MemBlock { Addr base; std::vector<uint8_t> data; };
    std::unordered_map<SpaceId, MemBlock> memory_;

    size_t mem_offset(const SpaceId& space, Addr addr) const;
    void handle_mmio_write(const SpaceId& space, Addr addr, uint32_t v, uint8_t w);
    uint32_t handle_mmio_read(const SpaceId& space, Addr addr, uint8_t w);
    bool check_interrupt();
    void enter_interrupt(const InterruptDesc& irq);
    void check_reset_pin();
    void check_inject_slots();

    // SP 唯一入口：8051 的 SP 在 SFR 0x81，必须经由这两个函数读写，
    // 避免 regs_.sp 与 SFR SP 分叉。
    uint8_t read_sp();
    void write_sp(uint8_t v);

    struct IrqFrame { uint8_t saved_ip; };
    std::vector<IrqFrame> irq_stack_;
    MemBlock* code_blk_ = nullptr;
    MemBlock* iram_blk_ = nullptr;
    MemBlock* xram_blk_ = nullptr;
};

} // namespace mcus