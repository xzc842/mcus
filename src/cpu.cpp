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

#include "mcus/cpu.h"
#include "mcus/log.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace mcus {

Cpu::Cpu(const Model& model, SharedMemory& shm, EventStream& stream)
    : model_(model), shm_(shm), stream_(stream) {
    register_all_isas();
    isa_ = IsaRegistry::instance().create(model_.arch, model_);
    if (!isa_) throw std::runtime_error("no ISA for arch: " + model_.arch);

    for (const auto& r : model_.regions) {
        if (r.is_mmio) continue;
        auto& blk = memory_[r.space];
        if (blk.data.empty()) {
            blk.base = r.base;
            blk.data.assign(r.size, 0);
        } else {
            Addr cur_end = blk.base + blk.data.size();
            Addr new_end = r.base + r.size;
            if (r.base < blk.base) {
                size_t delta = blk.base - r.base;
                std::vector<uint8_t> nd(delta + blk.data.size() +
                    (new_end > cur_end ? new_end - cur_end : 0), 0);
                std::memcpy(nd.data() + delta, blk.data.data(), blk.data.size());
                blk.base = r.base;
                blk.data = std::move(nd);
            } else if (new_end > cur_end) {
                blk.data.resize(blk.data.size() + (new_end - cur_end), 0);
            }
        }
    }

    for (const auto& sp : model_.arch_desc.fast_spaces) {
        auto it = memory_.find(sp);
        if (it != memory_.end()) fast_blocks_[sp] = &it->second;
    }
    if (model_.arch_desc.global_irq_enable.present) {
        has_global_irq_enable_ = true;
        global_irq_space_ = model_.arch_desc.global_irq_enable.space;
        global_irq_reg_ = model_.arch_desc.global_irq_enable.reg;
        global_irq_bit_ = model_.arch_desc.global_irq_enable.bit;
    }

    reset();
}

size_t Cpu::mem_offset(const SpaceId& space, Addr addr) const {
    auto it = memory_.find(space);
    if (it == memory_.end()) return SIZE_MAX;
    const MemoryRegion* r = model_.find_region(space, addr);
    if (!r) return SIZE_MAX;
    Addr effective = addr;
    if (r->addr_mask != 0) {
        effective = r->base + ((addr - r->base) & r->addr_mask);
    }
    if (effective < it->second.base) return SIZE_MAX;
    size_t off = effective - it->second.base;
    if (off >= it->second.data.size()) return SIZE_MAX;
    return off;
}

void Cpu::load_firmware(const std::vector<uint8_t>& firmware) {
    const MemoryRegion* tgt = model_.find_region(model_.arch_desc.reset_space,
                                                  model_.arch_desc.reset_vector);
    if (!tgt) throw std::runtime_error("reset_vector region missing");
    size_t off = mem_offset(tgt->space, tgt->base);
    if (off == SIZE_MAX) throw std::runtime_error("firmware target block missing");
    auto& blk = memory_[tgt->space];
    size_t n = std::min<size_t>(firmware.size(), tgt->size);
    n = std::min<size_t>(n, blk.data.size() - off);
    std::memcpy(blk.data.data() + off, firmware.data(), n);
    MCUS_INFO("firmware loaded: " + std::to_string(n) + " bytes at " +
              tgt->space + ":0x" + std::to_string(tgt->base));
}

void Cpu::reset() {
    regs_ = CpuRegisters{};
    regs_.pc = model_.arch_desc.reset_vector;
    halted_.store(false);
    paused_.store(false);
    irq_stack_.clear();
    shm_.reset_all();
    write_sp(static_cast<uint8_t>(model_.arch_desc.initial_sp));
    shm_.control()->pc.store(regs_.pc);
    shm_.control()->virtual_time.store(0);
    MCUS_INFO("cpu reset, pc=0x" + std::to_string(regs_.pc) +
              " sp=0x" + std::to_string(read_sp()));
}

size_t Cpu::export_memory(const SpaceId& space, Addr addr, size_t len,
                          uint8_t* dst) const {
    size_t off = mem_offset(space, addr);
    if (off == SIZE_MAX) return 0;
    const auto& blk = memory_.at(space);
    size_t n = std::min<size_t>(len, blk.data.size() - off);
    std::memcpy(dst, blk.data.data() + off, n);
    return n;
}

uint8_t Cpu::read8(const SpaceId& space, Addr addr) {
    auto it = fast_blocks_.find(space);
    if (it != fast_blocks_.end()) {
        size_t off = addr;
        if (space == "iram" || space == "xram" || space == "ram")
            off = addr & (it->second->data.size() - 1);
        if (off < it->second->data.size()) return it->second->data[off];
        return 0;
    }
    const MemoryRegion* r = model_.find_region(space, addr);
    if (r && r->is_mmio) return static_cast<uint8_t>(handle_mmio_read(space, addr, 8));
    size_t off = mem_offset(space, addr);
    if (off == SIZE_MAX) return 0;
    return memory_.at(space).data[off];
}

uint16_t Cpu::read16(const SpaceId& space, Addr addr) {
    const MemoryRegion* r = model_.find_region(space, addr);
    if (r && r->is_mmio) return static_cast<uint16_t>(handle_mmio_read(space, addr, 16));
    uint16_t v = 0;
    for (int i = 0; i < 2; ++i)
        v |= static_cast<uint16_t>(read8(space, addr + i)) << (8 * i);
    return v;
}

uint32_t Cpu::read32(const SpaceId& space, Addr addr) {
    const MemoryRegion* r = model_.find_region(space, addr);
    if (r && r->is_mmio) return handle_mmio_read(space, addr, 32);
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<uint32_t>(read8(space, addr + i)) << (8 * i);
    return v;
}

void Cpu::write8(const SpaceId& space, Addr addr, uint8_t v) {
    if (space == model_.arch_desc.reset_space) {
        MCUS_WARN("write to reset space at 0x" + std::to_string(addr) +
                  " ignored (firmware protection)");
        return;
    }
    auto it = fast_blocks_.find(space);
    if (it != fast_blocks_.end()) {
        size_t off = addr;
        if (space == "iram" || space == "xram" || space == "ram")
            off = addr & (it->second->data.size() - 1);
        if (off < it->second->data.size()) it->second->data[off] = v;
        return;
    }
    const MemoryRegion* r = model_.find_region(space, addr);
    if (r && r->is_mmio) { handle_mmio_write(space, addr, v, 8); return; }
    size_t off = mem_offset(space, addr);
    if (off == SIZE_MAX) return;
    memory_[space].data[off] = v;
}

void Cpu::write16(const SpaceId& space, Addr addr, uint16_t v) {
    const MemoryRegion* r = model_.find_region(space, addr);
    if (r && r->is_mmio) { handle_mmio_write(space, addr, v, 16); return; }
    write8(space, addr, static_cast<uint8_t>(v & 0xFF));
    write8(space, addr + 1, static_cast<uint8_t>((v >> 8) & 0xFF));
}

void Cpu::write32(const SpaceId& space, Addr addr, uint32_t v) {
    const MemoryRegion* r = model_.find_region(space, addr);
    if (r && r->is_mmio) { handle_mmio_write(space, addr, v, 32); return; }
    for (int i = 0; i < 4; ++i)
        write8(space, addr + i, static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

uint8_t Cpu::read_sp() {
    return read8(model_.arch_desc.sp_reg_space, model_.arch_desc.sp_reg);
}

void Cpu::write_sp(uint8_t v) {
    write8(model_.arch_desc.sp_reg_space, model_.arch_desc.sp_reg, v);
}

void Cpu::handle_mmio_write(const SpaceId& space, Addr addr, uint32_t value, uint8_t width) {
    const RegisterDesc* reg = model_.find_register(space, addr);
    if (!reg) {
        MCUS_WARN("invalid MMIO write at " + space + ":0x" + std::to_string(addr));
        return;
    }
    if (!reg->write_alias.empty()) {
        const RegisterDesc* alias = model_.find_register_by_name(reg->write_alias);
        if (alias) reg = alias;
    }
    if (reg->width != width) {
        MCUS_WARN("MMIO width mismatch at " + space + ":0x" + std::to_string(addr));
        return;
    }
    if (reg->semantics == RegSemantics::ReadOnly ||
        reg->semantics == RegSemantics::ReadClear) {
        MCUS_WARN("write to read-only register " + reg->name);
        return;
    }
    uint32_t old = shm_.read_reg(space, addr, width);
    uint32_t nv = value;
    if (reg->semantics == RegSemantics::WriteOneClear) nv = old & ~value;
    shm_.write_reg(space, addr, nv, width);

    PinState* pins = shm_.pins();
    for (size_t i = 0; i < model_.pins.size(); ++i) {
        const PinDesc& pd = model_.pins[i];
        if (pd.source_space == space && pd.source_reg == addr && pd.source_bit < 32) {
            uint32_t bv = (nv >> pd.source_bit) & 1u;
            pins[i].value.store(bv);
            pins[i].function.store(0);
        }
    }

    if (reg->semantics == RegSemantics::WriteTrigger ||
        reg->semantics == RegSemantics::Fifo) {
        VirtTime now = shm_.control()->virtual_time.load();
        int sidx = model_.space_index(space);
        if (sidx < 0) sidx = 0;
        MmioEvent ev{now, addr, value, width, MmioAccess::Write,
                     static_cast<uint32_t>(sidx)};
        if (!stream_.push(ev)) MCUS_WARN("event stream full");

        // SBUF 写：置 TI（SCON bit1），触发 UART0 中断
        if (space == "sfr" && addr == 0x99) {
            uint8_t scon = static_cast<uint8_t>(shm_.read_reg("sfr", 0x98, 8));
            scon |= 0x02;
            shm_.write_reg("sfr", 0x98, scon, 8);
        }
        // SPI0_DATA 写：置 SPIF（SPI0_STAT bit4）
        if (space == "sfr" && addr == 0xF9) {
            uint8_t stat = static_cast<uint8_t>(shm_.read_reg("sfr", 0xF8, 8));
            stat |= 0x10;
            shm_.write_reg("sfr", 0xF8, stat, 8);
        }
    }
}

uint32_t Cpu::handle_mmio_read(const SpaceId& space, Addr addr, uint8_t width) {
    const RegisterDesc* reg = model_.find_register(space, addr);
    if (!reg) return 0;
    if (!reg->read_alias.empty()) {
        const RegisterDesc* alias = model_.find_register_by_name(reg->read_alias);
        if (alias) reg = alias;
    }
    if (reg->width != width) return 0;
    if (reg->semantics == RegSemantics::WriteOnly)
        return reg->reset_value;
    uint32_t v = shm_.read_reg(space, addr, width);
    if (reg->semantics == RegSemantics::ReadClear)
        shm_.write_reg(space, addr, 0, width);
    return v;
}

void Cpu::check_inject_slots() {
    auto* ih = shm_.inject_header();
    auto* slots = shm_.inject_slots();
    uint32_t cap = ih->capacity.load();
    uint32_t tail = ih->tail.load(std::memory_order_acquire);
    uint32_t head = ih->head.load(std::memory_order_acquire);
    while (tail != head) {
        InjectSlot& s = slots[tail % cap];
        if (s.ready.load(std::memory_order_acquire)) {
            uint32_t idx = s.space_idx.load();
            std::vector<SpaceId> mmio_spaces;
            for (const auto& r : model_.regions)
                if (r.is_mmio &&
                    std::find(mmio_spaces.begin(), mmio_spaces.end(), r.space) ==
                        mmio_spaces.end())
                    mmio_spaces.push_back(r.space);
            if (idx < mmio_spaces.size()) {
                const auto& sp = mmio_spaces[idx];
                Addr addr = s.addr.load();
                uint32_t value = s.value.load();
                uint8_t width = static_cast<uint8_t>(s.width.load());
                if (model_.find_register(sp, addr))
                    shm_.write_reg(sp, addr, value, width);
            }
            s.ready.store(0, std::memory_order_release);
        }
        tail++;
        ih->tail.store(tail, std::memory_order_release);
    }
}

static uint32_t read_reg_by_model(const Model& model, SharedMemory& shm,
                                  const SpaceId& space, Addr addr) {
    const RegisterDesc* r = model.find_register(space, addr);
    uint8_t w = r ? r->width : 8;
    return shm.read_reg(space, addr, w);
}

static void write_reg_by_model(const Model& model, SharedMemory& shm,
                               const SpaceId& space, Addr addr, uint32_t v) {
    const RegisterDesc* r = model.find_register(space, addr);
    uint8_t w = r ? r->width : 8;
    shm.write_reg(space, addr, v, w);
}

bool Cpu::check_interrupt() {
    if (has_global_irq_enable_) {
        uint32_t v = read_reg_by_model(model_, shm_, global_irq_space_, global_irq_reg_);
        if (!(v & (1u << global_irq_bit_))) return false;
    }
    uint8_t* bitmap = shm_.interrupt_bitmap();
    for (const auto& irq : model_.interrupts) {
        uint32_t pending = read_reg_by_model(model_, shm_, irq.pending_space, irq.pending_reg);
        bool bp = false;
        size_t byte = irq.number / 8, bit = irq.number % 8;
        if (byte < shm_.interrupt_bitmap_size())
            bp = (bitmap[byte] & (1u << bit)) != 0;
        if (!bp && !(pending & (1u << irq.pending_bit))) continue;
        uint32_t enable = read_reg_by_model(model_, shm_, irq.enable_space, irq.enable_reg);
        if (!(enable & (1u << irq.enable_bit))) continue;
        enter_interrupt(irq);
        return true;
    }
    return false;
}

void Cpu::enter_interrupt(const InterruptDesc& irq) {
    MCUS_DEBUG("enter interrupt " + irq.name);
    irq_stack_.push_back({ read8("sfr", 0xB8) });

    uint8_t sp = read_sp();
    for (int i = 0; i < model_.arch_desc.irq_push_bytes; ++i) {
        sp = static_cast<uint8_t>(sp + 1);
        write_sp(sp);
        write8(model_.arch_desc.sp_space, sp,
               static_cast<uint8_t>((regs_.pc >> (8 * i)) & 0xFF));
    }

    if (irq.clear_pending) {
        uint32_t pending = read_reg_by_model(model_, shm_, irq.pending_space, irq.pending_reg);
        pending &= ~(1u << irq.pending_bit);
        write_reg_by_model(model_, shm_, irq.pending_space, irq.pending_reg, pending);
    }

    uint8_t* bitmap = shm_.interrupt_bitmap();
    size_t byte = irq.number / 8, bit = irq.number % 8;
    if (byte < shm_.interrupt_bitmap_size())
        bitmap[byte] &= static_cast<uint8_t>(~(1u << bit));

    regs_.pc = irq.vector_addr;
}

void Cpu::on_reti() {
    if (irq_stack_.empty()) return;
    IrqFrame frame = irq_stack_.back();
    irq_stack_.pop_back();
    write8("sfr", 0xB8, frame.saved_ip);
}

void Cpu::check_reset_pin() {
    auto* ctl = shm_.control();
    if (ctl->reset_pin.load() != 0) {
        MCUS_INFO("reset pin asserted");
        reset();
        ctl->reset_pin.store(0);
    }
}
bool Cpu::check_breakpoint() {
    auto* bh = shm_.breakpoint_header();
    if (bh->hit.load(std::memory_order_relaxed) != 0) return true;

    // pc 没变，不重复查（Resume 后跳过当前断点）
    if (regs_.pc == last_bp_pc_) return false;
    last_bp_pc_ = regs_.pc;

    uint32_t count = bh->count.load(std::memory_order_relaxed);
    if (count == 0) return false;
    if (count > shm_.breakpoint_capacity()) count = shm_.breakpoint_capacity();

    uint32_t pc = regs_.pc;
    bool sorted = bh->sorted.load(std::memory_order_relaxed) != 0;
    auto* slots = shm_.breakpoints();

    bool hit = false;
    if (sorted) {
        uint32_t min_addr = bh->min_addr.load(std::memory_order_relaxed);
        uint32_t max_addr = bh->max_addr.load(std::memory_order_relaxed);
        if (pc < min_addr || pc > max_addr) return false;

        uint32_t lo = 0, hi = count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint32_t addr = slots[mid].addr.load(std::memory_order_relaxed);
            if (addr == pc) {
                if (slots[mid].enabled.load(std::memory_order_relaxed)) hit = true;
                break;
            } else if (addr < pc) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            if (slots[i].enabled.load(std::memory_order_relaxed) &&
                slots[i].addr.load(std::memory_order_relaxed) == pc) {
                hit = true;
                break;
            }
        }
    }

    if (hit) {
        bh->hit.store(1, std::memory_order_relaxed);
        bh->hit_addr.store(pc, std::memory_order_relaxed);
        paused_.store(true);
        shm_.control()->status.store(static_cast<uint32_t>(ControlStatus::Paused));
        MCUS_INFO("breakpoint hit at 0x" + std::to_string(pc));
        return true;
    }
    return false;
}
VirtTime Cpu::step() {
    if (halted_.load()) return 0;
    if (paused_.load()) return 0;

    check_reset_pin();
    check_inject_slots();
    if (check_breakpoint()) return 0;
    if (check_interrupt()) return model_.cycles_per_instruction;

    ExecResult r = isa_->execute(regs_, *this);
    shm_.control()->pc.store(regs_.pc, std::memory_order_relaxed);
    if (r.halted) {
        halted_.store(true);
        shm_.control()->halted.store(1);
        shm_.control()->status.store(static_cast<uint32_t>(ControlStatus::Halted));
    }
    return r.cycles == 0 ? model_.cycles_per_instruction : r.cycles;
}

} // namespace mcus