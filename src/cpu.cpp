#include "mcus/cpu.h"
#include "mcus/log.h"
#include <algorithm>
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
    const MemoryRegion* tgt = model_.find_region(model_.reset_space, model_.reset_vector);
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
    regs_.pc = model_.reset_vector;
    regs_.sp = model_.initial_sp;
    halted_.store(false);
    paused_.store(false);
    irq_stack_.clear();
    shm_.reset_all();
    shm_.control()->pc.store(regs_.pc);
    shm_.control()->virtual_time.store(0);
    MCUS_INFO("cpu reset, pc=0x" + std::to_string(regs_.pc));
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
    if (space == "iram" && (addr == 0x61 || addr == 0x62)) {
        MCUS_WARN("write iram 0x" + std::to_string(addr) +
                  " = 0x" + std::to_string(v));
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
    if (reg->semantics == RegSemantics::WriteOnly ||
        reg->semantics == RegSemantics::WriteTrigger) return reg->reset_value;
    uint32_t v = shm_.read_reg(space, addr, width);
    if (reg->semantics == RegSemantics::ReadClear) shm_.write_reg(space, addr, 0, width);
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

bool Cpu::check_interrupt() {
    uint8_t* bitmap = shm_.interrupt_bitmap();
    for (const auto& irq : model_.interrupts) {
        uint32_t pending = shm_.read_reg(irq.pending_space, irq.pending_reg, 32);
        bool bp = false;
        size_t byte = irq.number / 8, bit = irq.number % 8;
        if (byte < shm_.interrupt_bitmap_size())
            bp = (bitmap[byte] & (1u << bit)) != 0;
        if (!bp && !(pending & (1u << irq.pending_bit))) continue;
        uint32_t enable = shm_.read_reg(irq.enable_space, irq.enable_reg, 32);
        if (!(enable & (1u << irq.enable_bit))) continue;
        enter_interrupt(irq);
        return true;
    }
    return false;
}

void Cpu::enter_interrupt(const InterruptDesc& irq) {
    MCUS_DEBUG("enter interrupt " + irq.name);
    irq_stack_.push_back({ static_cast<uint8_t>(shm_.read_reg("sfr", 0xB8, 8)) });
    regs_.sp -= 4;
    write32(model_.sp_space, regs_.sp, regs_.pc);
    uint32_t pending = shm_.read_reg(irq.pending_space, irq.pending_reg, 32);
    pending &= ~(1u << irq.pending_bit);
    shm_.write_reg(irq.pending_space, irq.pending_reg, pending, 32);
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
    shm_.write_reg("sfr", 0xB8, frame.saved_ip, 8);
}

void Cpu::check_reset_pin() {
    auto* ctl = shm_.control();
    if (ctl->reset_pin.load() != 0) {
        MCUS_INFO("reset pin asserted");
        reset();
        ctl->reset_pin.store(0);
    }
}

VirtTime Cpu::step() {
    if (halted_.load()) return 0;
    if (paused_.load()) return 0;
    check_reset_pin();
    check_inject_slots();
    if (check_interrupt()) return model_.cycles_per_instruction;

    ExecResult r = isa_->execute(regs_, *this);
    shm_.control()->pc.store(regs_.pc);
    if (r.halted) {
        halted_.store(true);
        shm_.control()->halted.store(1);
        shm_.control()->status.store(static_cast<uint32_t>(ControlStatus::Halted));
    }
    return r.cycles == 0 ? model_.cycles_per_instruction : r.cycles;
}

} // namespace mcus