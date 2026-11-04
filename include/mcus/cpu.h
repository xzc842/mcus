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
#include "firmware.h"
#include "shared_memory.h"
#include "event_stream.h"
#include "instruction.h"
#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mcus {

struct CpuRegisters {
    uint32_t r[32]{};
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
    uint32_t pc() const { return regs_.pc; }

    void load_firmware(const FirmwareImage& firmware);
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
    IsaPtr isa_;
    CpuRegisters regs_;
    std::atomic<bool> paused_{false};
    std::atomic<bool> halted_{false};

    struct MemBlock { Addr base; std::vector<uint8_t> data; };
    std::unordered_map<SpaceId, MemBlock> memory_;
    std::unordered_map<SpaceId, MemBlock*> fast_blocks_;

    bool has_global_irq_enable_ = false;
    SpaceId global_irq_space_;
    Addr global_irq_reg_ = 0;
    uint8_t global_irq_bit_ = 0;

    size_t mem_offset(const SpaceId& space, Addr addr) const;
    void handle_mmio_write(const SpaceId& space, Addr addr, uint32_t v, uint8_t w);
    uint32_t handle_mmio_read(const SpaceId& space, Addr addr, uint8_t w);
    bool check_interrupt();
    bool check_breakpoint();
    void enter_interrupt(const InterruptDesc& irq);
    void check_reset_pin();
    void check_inject_slots();

    uint32_t last_bp_pc_ = 0xFFFFFFFF;
};

} // namespace mcus