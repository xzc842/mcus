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
    struct IrqFrame { uint8_t saved_ip; };
    std::vector<IrqFrame> irq_stack_;
};

} // namespace mcus