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

namespace {

void apply_actions(const Model& model, SharedMemory& shm,
                   const std::vector<RegisterAction>& actions) {
    for (const auto& act : actions) {
        const RegisterDesc* target = model.find_register(act.space, act.addr);
        if (!target) continue;

        const uint8_t width = target->width;
        uint32_t value = shm.read_reg(act.space, act.addr, width);
        switch (act.kind) {
            case RegisterAction::Kind::SetBit:
                value |= (1u << act.bit);
                break;
            case RegisterAction::Kind::ClearBit:
                value &= ~(1u << act.bit);
                break;
            case RegisterAction::Kind::SetValue:
                value = act.value;
                break;
            case RegisterAction::Kind::ClearValue:
                value = 0;
                break;
        }

        if (width < 32) {
            value &= (1u << width) - 1u;
        }
        shm.write_reg(act.space, act.addr, value, width);
    }
}

} // namespace

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

void Cpu::load_firmware(const FirmwareImage& firmware) {
    Addr load_addr = firmware.load_addr;

    // 先找镜像起始地址所在的普通内存区域；找不到则按“相对复位区域基址”处理
    const MemoryRegion* tgt = nullptr;
    for (const auto& r : model_.regions) {
        if (!r.is_mmio && r.contains(load_addr)) {
            tgt = &r;
            break;
        }
    }

    if (!tgt) {
        tgt = model_.find_region(model_.arch_desc.reset_space,
                                 model_.arch_desc.reset_vector);
        if (!tgt) throw std::runtime_error("reset_vector region missing");
        load_addr = tgt->base + load_addr;
    }

    size_t off = mem_offset(tgt->space, load_addr);
    if (off == SIZE_MAX) throw std::runtime_error("firmware target block missing");

    auto& blk = memory_[tgt->space];
    size_t region_off = load_addr - tgt->base;
    size_t avail = tgt->size > region_off ? tgt->size - region_off : 0;
    size_t n = std::min<size_t>(firmware.data.size(), avail);
    n = std::min<size_t>(n, blk.data.size() - off);

    std::memcpy(blk.data.data() + off, firmware.data.data(), n);
    MCUS_INFO("firmware loaded: " + std::to_string(n) + " bytes at " +
              tgt->space + ":0x" + std::to_string(load_addr));
}

void Cpu::load_firmware(const std::vector<uint8_t>& firmware) {
    load_firmware(FirmwareImage{0, firmware});
}

void Cpu::reset() {
    regs_ = CpuRegisters{};
    halted_.store(false);
    paused_.store(false);
    irq_in_service_.clear();
    irq_service_priority_.clear();
    now_ = 0;
    last_bp_pc_ = 0xFFFFFFFF;
    next_bus_xfer_id_ = 1;
    uart_rt_.assign(model_.uarts.size(), UartRuntime{});
    spi_rt_.assign(model_.spis.size(), SpiRuntime{});
    pwm_rt_.assign(model_.pwms.size(), PwmRuntime{});
    adc_rt_.assign(model_.adcs.size(), AdcRuntime{});
    tkey_rt_.assign(model_.tkeys.size(), TkeyRuntime{});
    usb_tx_pending_.assign(model_.usb.endpoints.size(), 0);
    shm_.reset_all();

    // 架构相关的 PC/SP/寄存器初始化由 ISA 负责
    isa_->reset(regs_, *this, model_);

    // 引脚初值必须从端口锁存器/方向寄存器同步，否则外部监视器会看到
    // 端口复位值非 0 但 PinState.value 仍为 0。
    size_t n = model_.pins.size();
    last_pin_value_.assign(n, 0);
    last_pin_function_.assign(n, 0);
    last_pin_direction_.assign(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const PinDesc& pd = model_.pins[i];
        uint32_t value = 0;
        if (!pd.source_space.empty() && pd.source_reg != 0) {
            const RegisterDesc* rd =
                model_.find_register(pd.source_space, pd.source_reg);
            const uint8_t width = rd ? rd->width : 8;
            uint32_t reg = shm_.read_reg(pd.source_space, pd.source_reg, width);
            value = (reg >> pd.source_bit) & 1u;
        }
        uint32_t direction = 0;
        if (pd.has_direction && pd.direction_reg != 0) {
            const RegisterDesc* rd =
                model_.find_register(pd.direction_space, pd.direction_reg);
            const uint8_t width = rd ? rd->width : 8;
            uint32_t reg =
                shm_.read_reg(pd.direction_space, pd.direction_reg, width);
            direction = (reg >> pd.direction_bit) & 1u;
        }
        PinState* writable = shm_.pins();
        writable[i].value.store(value);
        writable[i].direction.store(direction);
        writable[i].function.store(pd.reset_function);
        last_pin_value_[i] = value;
        last_pin_function_[i] = pd.reset_function;
        last_pin_direction_[i] = direction;
    }

    shm_.control()->pc.store(regs_.pc);
    shm_.control()->virtual_time.store(0);
    MCUS_INFO("cpu reset, pc=0x" + std::to_string(regs_.pc) +
              " sp=0x" + std::to_string(regs_.sp));
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
        // 完全由模型的 region/base/addr_mask 计算偏移，不假设空间名叫 iram/xram。
        size_t off = mem_offset(space, addr);
        if (off != SIZE_MAX) return it->second->data[off];
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
    const MemoryRegion* region = model_.find_region(space, addr);
    if (region && region->read_only) {
        MCUS_WARN("write to read-only region at 0x" + std::to_string(addr) +
                  " ignored");
        return;
    }
    auto it = fast_blocks_.find(space);
    if (it != fast_blocks_.end()) {
        size_t off = mem_offset(space, addr);
        if (off != SIZE_MAX) it->second->data[off] = v;
        return;
    }
    if (region && region->is_mmio) { handle_mmio_write(space, addr, v, 8); return; }
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

void Cpu::emit_mmio_event(const SpaceId& space, Addr addr, uint32_t value,
                          uint8_t width) {
    int sidx = model_.space_index(space);
    if (sidx < 0) sidx = 0;

    DebugEvent ev{};
    ev.time = now_;
    ev.type = DebugEventType::Mmio;
    ev.u.mmio = MmioEvent{ev.time, addr, value, width, MmioAccess::Write,
                          static_cast<uint32_t>(sidx)};
    if (!stream_.push(ev)) MCUS_WARN("event stream full");
}

void Cpu::update_pin(size_t index, uint32_t value, uint32_t function,
                     uint32_t direction) {
    PinState* pins = shm_.pins();
    const uint32_t old_value = pins[index].value.load();
    const uint32_t old_function = pins[index].function.load();
    const uint32_t old_direction = pins[index].direction.load();

    uint32_t changed = 0;
    if (old_value != value) changed |= 1;
    if (old_function != function) changed |= 2;
    if (old_direction != direction) changed |= 4;
    if (changed == 0) return;

    pins[index].value.store(value);
    pins[index].function.store(function);
    pins[index].direction.store(direction);

    DebugEvent ev{};
    ev.time = now_;
    ev.type = DebugEventType::Pin;
    ev.u.pin = PinEvent{static_cast<uint32_t>(index), value, function,
                        direction, changed};
    if (!stream_.push(ev)) MCUS_WARN("event stream full");

    if (index < last_pin_value_.size()) {
        last_pin_value_[index] = value;
        last_pin_function_[index] = function;
        last_pin_direction_[index] = direction;
    }
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

    if (model_.usb.present && space == model_.usb.space) {
        for (size_t i = 0; i < model_.usb.endpoints.size(); ++i) {
            const auto& ep = model_.usb.endpoints[i];
            if (ep.len_reg == addr && ep.len_space == space && nv > 0) {
                if (i < usb_tx_pending_.size()) usb_tx_pending_[i] = 1;
            }
        }
    }

    // 模型描述的寄存器副作用（由 on_write 动作描述）
    apply_actions(model_, shm_, reg->on_write);

    PinState* pins = shm_.pins();
    for (size_t i = 0; i < model_.pins.size(); ++i) {
        const PinDesc& pd = model_.pins[i];
        if (pd.source_space == space && pd.source_reg == addr &&
            pd.source_bit < 32) {
            const uint32_t bv = (nv >> pd.source_bit) & 1u;
            const uint32_t dir = pins[i].direction.load();
            // 输入引脚由外部 PinState 决定电平；输出引脚才跟随锁存器。
            const uint32_t pv = dir ? bv : pins[i].value.load();
            update_pin(i, pv, pins[i].function.load(), dir);
        }
        if (pd.has_direction && pd.direction_space == space &&
            pd.direction_reg == addr && pd.direction_bit < 32) {
            const uint32_t dir = (nv >> pd.direction_bit) & 1u;
            uint32_t pv = pins[i].value.load();
            if (dir && pd.source_space == space) {
                // 切换为输出时，把端口锁存器当前值同步到引脚。
                const RegisterDesc* srd =
                    model_.find_register(pd.source_space, pd.source_reg);
                const uint8_t sw = srd ? srd->width : 8;
                const uint32_t latch =
                    shm_.read_reg(pd.source_space, pd.source_reg, sw);
                pv = (latch >> pd.source_bit) & 1u;
            }
            update_pin(i, pv, pins[i].function.load(), dir);
        }
    }

    if (reg->semantics == RegSemantics::WriteTrigger ||
        reg->semantics == RegSemantics::Fifo) {
        emit_mmio_event(space, addr, value, width);
    }

    // 架构相关副作用（保留给外部 ISA 插件）
    isa_->on_mmio_write(model_, *this, space, addr, nv, width);

    // 外设（UART/SPI/PWM/ADC/TKEY/USB）的副作用
    on_peripheral_mmio_write(space, addr, nv, width);
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

    // 输入引脚：把外部写入 PinState.value 的电平叠加回端口寄存器
    const PinState* pins = shm_.pins();
    bool has_input = false;
    uint32_t merged = v;
    for (size_t i = 0; i < model_.pins.size(); ++i) {
        const PinDesc& pd = model_.pins[i];
        if (pd.source_space != space || pd.source_reg != addr ||
            pd.source_bit >= 32) {
            continue;
        }
        // 端口读始终叠加 PinState.value：外部程序可以在任何方向下注入电平。
        // 没有外部写入时，update_pin 已把锁存器值同步到 PinState，读回值不变。
        {
            const uint32_t bit = 1u << pd.source_bit;
            merged = (merged & ~bit) |
                     ((pins[i].value.load() & 1u) << pd.source_bit);
            has_input = true;
        }
    }
    if (has_input) v = merged;

    v = on_peripheral_mmio_read(space, addr, v, width);
    apply_actions(model_, shm_, reg->on_read);
    isa_->on_mmio_read(model_, *this, space, addr, width);
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

bool Cpu::check_interrupt() {
    if (has_global_irq_enable_) {
        uint32_t v = read_reg_by_model(model_, shm_, global_irq_space_, global_irq_reg_);
        if (!(v & (1u << global_irq_bit_))) return false;
    }

    uint8_t* bitmap = shm_.interrupt_bitmap();
    const InterruptDesc* best = nullptr;

    for (const auto& irq : model_.interrupts) {
        uint32_t pending = read_reg_by_model(model_, shm_, irq.pending_space, irq.pending_reg);
        bool bp = false;
        size_t byte = irq.number / 8, bit = irq.number % 8;
        if (byte < shm_.interrupt_bitmap_size())
            bp = (bitmap[byte] & (1u << bit)) != 0;
        if (!bp && !(pending & (1u << irq.pending_bit))) continue;

        uint32_t enable = read_reg_by_model(model_, shm_, irq.enable_space, irq.enable_reg);
        if (!(enable & (1u << irq.enable_bit))) continue;

        // 中断服务中屏蔽：同号中断不能重入；同级或更低优先级也不能抢占。
        // 这是 UART/SPI 这类“进入中断后仍需软件清标志”的外设不出现中断风暴的关键。
        if (!irq_in_service_.empty()) {
            if (std::find(irq_in_service_.begin(), irq_in_service_.end(),
                          irq.number) != irq_in_service_.end()) {
                continue;
            }
            if (irq.priority >= irq_service_priority_.back()) continue;
        }

        if (!best || isa_->higher_priority(irq, *best)) best = &irq;
    }

    if (!best) return false;
    enter_interrupt(*best);
    return true;
}

void Cpu::enter_interrupt(const InterruptDesc& irq) {
    MCUS_DEBUG("enter interrupt " + irq.name);

    irq_in_service_.push_back(irq.number);
    irq_service_priority_.push_back(irq.priority);

    // 保存架构上下文（由 ISA 钩子实现，核心不假设具体架构）
    isa_->save_context(regs_, *this, model_);

    if (irq.clear_pending) {
        isa_->acknowledge_interrupt(irq, *this, model_);
    }

    uint8_t* bitmap = shm_.interrupt_bitmap();
    size_t byte = irq.number / 8, bit = irq.number % 8;
    if (byte < shm_.interrupt_bitmap_size())
        bitmap[byte] &= static_cast<uint8_t>(~(1u << bit));

    regs_.pc = isa_->interrupt_vector(irq, *this, model_);
    shm_.control()->pc.store(regs_.pc);

    DebugEvent ev{};
    ev.time = now_;
    ev.type = DebugEventType::Irq;
    ev.u.irq = IrqEvent{irq.number, 1};
    if (!stream_.push(ev)) MCUS_WARN("event stream full (irq)");
}

void Cpu::on_reti() {
    isa_->restore_context(regs_, *this, model_);
    if (!irq_in_service_.empty()) irq_in_service_.pop_back();
    if (!irq_service_priority_.empty()) irq_service_priority_.pop_back();
}

// ============================================================================
// 外设 / 总线事件
// ============================================================================

uint32_t Cpu::read_reg8(const SpaceId& space, Addr addr) const {
    const RegisterDesc* rd = model_.find_register(space, addr);
    const uint8_t width = rd ? rd->width : 8;
    return shm_.read_reg(space, addr, width);
}

void Cpu::write_reg8(const SpaceId& space, Addr addr, uint32_t value) {
    const RegisterDesc* rd = model_.find_register(space, addr);
    const uint8_t width = rd ? rd->width : 8;

    if (space == "sfr" && addr == 0xD5) {
        MCUS_INFO("write UEP2_T_LEN=" + std::to_string(value));
    }
    shm_.write_reg(space, addr, value, width);
}

void Cpu::raise_usb_int_fg(uint32_t bits) {
    if (!model_.usb.present) return;
    const RegisterDesc* rd = model_.find_register(model_.usb.space,
                                                   model_.usb.int_fg);
    const uint8_t width = rd ? rd->width : 8;
    const uint32_t current =
        shm_.read_reg(model_.usb.space, model_.usb.int_fg, width);
    shm_.write_reg(model_.usb.space, model_.usb.int_fg,
                   current | bits, width);
}

void Cpu::set_irq_pending(uint32_t number) {
    const InterruptDesc* irq = model_.find_interrupt(number);
    if (!irq) return;
    uint32_t v = read_reg8(irq->pending_space, irq->pending_reg);
    v |= (1u << irq->pending_bit);
    write_reg8(irq->pending_space, irq->pending_reg, v);
}

uint32_t Cpu::function_index(const PinDesc& pin,
                             const std::string& name) const {
    for (size_t i = 0; i < pin.functions.size(); ++i)
        if (pin.functions[i] == name) return static_cast<uint32_t>(i);
    return 0;
}

void Cpu::emit_bus_event(uint32_t bus_id, BusEventKind kind, BusDirection dir,
                         uint32_t xfer_id, const uint8_t* data, uint16_t len,
                         uint8_t flags) {
    DebugEvent ev{};
    ev.time = now_;
    ev.type = DebugEventType::Bus;
    BusEvent& b = ev.u.bus;
    b.bus_id = bus_id;
    b.xfer_id = xfer_id;
    b.len = len > sizeof(b.data) ? static_cast<uint16_t>(sizeof(b.data)) : len;
    b.kind = static_cast<uint8_t>(kind);
    b.direction = static_cast<uint8_t>(dir);
    b.flags = flags;
    if (data && b.len) std::memcpy(b.data, data, b.len);
    if (!stream_.push(ev)) MCUS_WARN("event stream full (bus)");
}

void Cpu::emit_usb_event(uint32_t xfer_id, UsbToken token, uint8_t ep,
                         UsbHandshake hs, const uint8_t* data, uint16_t len) {
    DebugEvent ev{};
    ev.time = now_;
    ev.type = DebugEventType::Usb;
    UsbEvent& u = ev.u.usb;
    u.xfer_id = xfer_id;
    u.len = len > sizeof(u.data) ? static_cast<uint16_t>(sizeof(u.data)) : len;
    u.ep = ep;
    u.token = static_cast<uint8_t>(token);
    u.handshake = static_cast<uint8_t>(hs);
    if (data && u.len) std::memcpy(u.data, data, u.len);
    if (!stream_.push(ev)) MCUS_WARN("event stream full (usb)");
}

void Cpu::drive_pwm_pin(const PwmPinDesc& pin, uint32_t value) {
    if (pin.pin >= model_.pins.size()) return;
    // 功能名由模型给出；没有配置时保持引脚当前功能。
    uint32_t fn = shm_.pins()[pin.pin].function.load();
    if (!pin.function.empty())
        fn = function_index(model_.pins[pin.pin], pin.function);
    update_pin(pin.pin, value & 1u, fn, 1);
}

void Cpu::complete_spi(size_t index) {
    if (index >= spi_rt_.size()) return;
    SpiRuntime& rt = spi_rt_[index];
    if (!rt.busy) return;
    // 有外部程序时，必须等到 SpiXfer 响应；没有外部程序时用默认 MISO。
    if (stream_.host_attached() && !rt.responded) return;
    if (now_ < rt.ready_at) return;
    const SpiDesc& s = model_.spis[index];
    write_reg8(s.space, s.data, rt.rx);
    uint32_t stat = read_reg8(s.space, s.stat);
    stat |= (1u << s.done_bit);
    stat &= static_cast<uint32_t>(~(1u << s.busy_bit));
    write_reg8(s.space, s.stat, stat);
    rt.busy = false;
    rt.responded = false;
}

bool Cpu::waiting_for_bus() const {
    if (!stream_.host_attached()) return false;
    for (const auto& rt : spi_rt_)
        if (rt.busy && !rt.responded) return true;
    return false;
}

void Cpu::force_complete_spi() {
    for (size_t i = 0; i < spi_rt_.size(); ++i) {
        SpiRuntime& rt = spi_rt_[i];
        if (!rt.busy) continue;
        rt.responded = true;
        rt.ready_at = now_;
        complete_spi(i);
    }
}

void Cpu::complete_adc(size_t index) {
    if (index >= adc_rt_.size()) return;
    AdcRuntime& rt = adc_rt_[index];
    if (!rt.busy || now_ < rt.ready_at) return;
    const AdcDesc& a = model_.adcs[index];

    uint32_t sample = 0;
    if (rt.channel < a.channel_pins.size()) {
        const uint32_t pin = a.channel_pins[rt.channel];
        if (pin != UINT32_MAX && pin < model_.pins.size())
            sample = shm_.pins()[pin].value.load();
    }
    const RegisterDesc* drd = model_.find_register(a.space, a.data);
    const uint8_t dw = drd ? drd->width : 8;
    if (dw < 32) sample &= ((1u << dw) - 1u);
    write_reg8(a.space, a.data, sample);

    uint32_t ctrl = read_reg8(a.space, a.ctrl);
    ctrl |= (1u << a.if_bit);
    ctrl &= static_cast<uint32_t>(~(1u << a.busy_bit));
    write_reg8(a.space, a.ctrl, ctrl);
    rt.busy = false;
}

void Cpu::complete_tkey(size_t index) {
    if (index >= tkey_rt_.size()) return;
    TkeyRuntime& rt = tkey_rt_[index];
    if (!rt.busy || now_ < rt.ready_at) return;
    const TkeyDesc& t = model_.tkeys[index];

    uint32_t data = 0;
    const size_t max_channels =
        std::min<size_t>(t.channel_pins.size(), t.data_bits);
    for (size_t ch = 0; ch < max_channels; ++ch) {
        const uint32_t pin = t.channel_pins[ch];
        if (pin < model_.pins.size() && (shm_.pins()[pin].value.load() & 1u))
            data |= (1u << ch);
    }
    write_reg8(t.space, t.datl, data & 0xFFu);
    write_reg8(t.space, t.dath, (data >> 8) & 0xFFu);

    uint32_t ctrl = read_reg8(t.space, t.ctrl);
    ctrl |= (1u << t.if_bit);
    write_reg8(t.space, t.ctrl, ctrl);
    rt.busy = false;
}

void Cpu::advance_peripherals(VirtTime delta) {
    now_ += delta;

    for (size_t i = 0; i < uart_rt_.size(); ++i) {
        UartRuntime& rt = uart_rt_[i];
        if (!rt.tx_pending || now_ < rt.tx_ready) continue;
        const UartDesc& u = model_.uarts[i];
        uint32_t ctrl = read_reg8(u.space, u.ctrl);
        ctrl |= (1u << u.tx_flag_bit);
        write_reg8(u.space, u.ctrl, ctrl);
        rt.tx_pending = false;
    }

    for (size_t i = 0; i < spi_rt_.size(); ++i) complete_spi(i);
    for (size_t i = 0; i < adc_rt_.size(); ++i) complete_adc(i);
    for (size_t i = 0; i < tkey_rt_.size(); ++i) complete_tkey(i);

    for (size_t i = 0; i < pwm_rt_.size(); ++i) {
        PwmRuntime& rt = pwm_rt_[i];
        const PwmDesc& p = model_.pwms[i];
        if (!rt.started) continue;

        const uint32_t ctrl = read_reg8(p.space, p.ctrl);
        const bool en1 = (ctrl & (1u << p.enable_bit1)) != 0;
        const bool en2 = (ctrl & (1u << p.enable_bit2)) != 0;
        if (!en1 && !en2) {
            rt.acc = 0;
            continue;
        }

        rt.acc += delta;
        const uint64_t ticks = rt.acc / p.prescaler;
        rt.acc %= p.prescaler;
        if (ticks > 0) {
            const uint64_t total = rt.counter + ticks;
            rt.counter = static_cast<uint32_t>(total % p.period);
            if (total >= p.period) {
                uint32_t c = read_reg8(p.space, p.ctrl);
                c |= (1u << p.if_bit);
                write_reg8(p.space, p.ctrl, c);
            }
        }

        const uint32_t duty1 = read_reg8(p.space, p.data1);
        const uint32_t duty2 = read_reg8(p.space, p.data2);
        for (const auto& pp : p.pins) {
            const bool en = pp.channel == 2 ? en2 : en1;
            const uint32_t duty = pp.channel == 2 ? duty2 : duty1;
            drive_pwm_pin(pp, en ? (rt.counter < duty ? 1u : 0u) : 0u);
        }
    }
}

void Cpu::poll_bus_in() {
    DebugEvent ev{};
    while (stream_.pop_in(ev)) {
        if (ev.type == DebugEventType::Bus) {
            const BusEvent& b = ev.u.bus;
            const BusEventKind kind = static_cast<BusEventKind>(b.kind);

            if (kind == BusEventKind::UartRx &&
                b.direction == static_cast<uint8_t>(BusDirection::ToMcu)) {
                size_t idx = b.bus_id;
                if (idx >= model_.uarts.size()) idx = 0;
                if (idx >= model_.uarts.size()) continue;
                UartRuntime& rt = uart_rt_[idx];
                const UartDesc& u = model_.uarts[idx];
                uint16_t n = b.len == 0 ? 1 : b.len;
                if (n > sizeof(b.data)) n = sizeof(b.data);
                for (uint16_t i = 0; i < n; ++i) {
                    if (rt.rx.size() >= u.rx_fifo) {
                        MCUS_WARN("uart rx fifo full, byte dropped");
                        break;
                    }
                    rt.rx.push_back(b.data[i]);
                }
                if (!rt.rx.empty()) {
                    uint32_t ctrl = read_reg8(u.space, u.ctrl);
                    ctrl |= (1u << u.rx_flag_bit);
                    write_reg8(u.space, u.ctrl, ctrl);
                }
            } else if (kind == BusEventKind::SpiXfer &&
                       b.direction ==
                           static_cast<uint8_t>(BusDirection::ToMcu)) {
                for (size_t i = 0; i < spi_rt_.size(); ++i) {
                    SpiRuntime& rt = spi_rt_[i];
                    if (!rt.busy || rt.xfer_id != b.xfer_id) continue;
                    rt.rx = b.len ? b.data[0] : 0xFF;
                    rt.responded = true;
                    // 外部响应到达即视为传输完成，虚拟时间不因等待而推进。
                    rt.ready_at = now_;
                    complete_spi(i);
                    break;
                }
            }
        } else if (ev.type == DebugEventType::Usb) {
            const UsbEvent& u = ev.u.usb;
            if (!model_.usb.present) continue;
            const UsbToken token = static_cast<UsbToken>(u.token);
            const size_t ep_count = model_.usb.endpoints.size();
            size_t ep_idx = ep_count;
            for (size_t i = 0; i < ep_count; ++i)
                if (model_.usb.endpoints[i].index == u.ep) ep_idx = i;

            if (token == UsbToken::Setup || token == UsbToken::Out) {
                if (ep_idx >= ep_count) continue;
                uint16_t n = u.len;
                if (n > sizeof(u.data)) n = sizeof(u.data);
                for (uint16_t i = 0; i < n; ++i)
                    write_usb_byte(u.ep, i, u.data[i]);
                write_reg8(model_.usb.space, model_.usb.rx_len, n);
                uint32_t st = read_reg8(model_.usb.space, model_.usb.int_st);
                const uint32_t token_bits =
                    (token == UsbToken::Setup) ? 0x30u : 0x00u;
                st = (st & ~0xFFu) | (u.ep & 0x0Fu) | token_bits;
                write_reg8(model_.usb.space, model_.usb.int_st, st);
                uint32_t fg = read_reg8(model_.usb.space, model_.usb.int_fg);
                fg |= (1u << model_.usb.int_fg_bit);
                /* CH552 的 CDC_EP2_OUT 会检查 U_TOG_OK；建模为收包时置位。 */
                if (model_.usb.tog_ok_bit > 0 && model_.usb.tog_ok_bit < 8) {
                    fg |= (1u << model_.usb.tog_ok_bit);
                }
                /* 处理一次数据包时清掉上电遗留的 BUS_RST，避免固件重复 reset。 */
                if (model_.usb.bus_reset_bit < 8) {
                    fg &= ~(1u << model_.usb.bus_reset_bit);
                }
                write_reg8(model_.usb.space, model_.usb.int_fg, fg);
            } else if (token == UsbToken::In) {
                if (ep_idx >= ep_count) {
                    emit_usb_event(u.xfer_id, UsbToken::In, u.ep,
                                   UsbHandshake::Nak, nullptr, 0);
                    continue;
                }
                const UsbEndpointDesc& ed = model_.usb.endpoints[ep_idx];
                uint16_t req = u.len ? u.len : ed.max_packet;
                if (ed.len_reg != 0) {
                    const uint32_t l =
                        read_reg8(ed.len_space, ed.len_reg) & 0xFFFFu;
                    if (l <= ed.max_packet) req = static_cast<uint16_t>(l);
                }
                if (req > ed.max_packet) req = ed.max_packet;
                uint8_t data[64]{};
                for (uint16_t i = 0; i < req; ++i)
                    data[i] = static_cast<uint8_t>(read_usb_byte(u.ep, i));
                if (u.ep == 2) MCUS_INFO("IN2 x84=" + std::to_string(read8("xram",84)) +
                    " d0=" + std::to_string(data[0]) + " req=" + std::to_string(req));
                emit_usb_event(u.xfer_id, UsbToken::In, u.ep,
                               UsbHandshake::Ack, data, req);
                if (ed.len_reg != 0) {
                    shm_.write_reg(ed.len_space, ed.len_reg, 0, 8);
                }
                uint32_t st = read_reg8(model_.usb.space, model_.usb.int_st);
                st = (st & ~0xFFu) | (u.ep & 0x0Fu) | 0x20u;
                write_reg8(model_.usb.space, model_.usb.int_st, st);

                uint32_t fg = read_reg8(model_.usb.space, model_.usb.int_fg);
                fg |= (1u << model_.usb.int_fg_bit);
                if (model_.usb.tog_ok_bit > 0 && model_.usb.tog_ok_bit < 8) {
                    fg |= (1u << model_.usb.tog_ok_bit);
                }
                if (model_.usb.bus_reset_bit < 8) {
                    fg &= ~(1u << model_.usb.bus_reset_bit);
                }
                write_reg8(model_.usb.space, model_.usb.int_fg, fg);
            } else if (token == UsbToken::Reset) {
                for (const auto& ed : model_.usb.endpoints)
                    for (uint32_t i = 0; i < ed.max_packet; ++i)
                        write_usb_byte(ed.index, i, 0);
                write_reg8(model_.usb.space, model_.usb.dev_ad, 0);
                write_reg8(model_.usb.space, model_.usb.rx_len, 0);
                write_reg8(model_.usb.space, model_.usb.int_st, 0);
                write_reg8(model_.usb.space, model_.usb.int_fg, 0);
            }
        }
    }
}

void Cpu::poll_external_pins() {
    PinState* pins = shm_.pins();
    uint32_t gpio_enable = 0;
    if (model_.gpio_irq.present) {
        gpio_enable = read_reg8(model_.gpio_irq.enable_space,
                                model_.gpio_irq.enable_reg);
    }

    for (size_t i = 0; i < model_.pins.size(); ++i) {
        const PinDesc& pd = model_.pins[i];
        const uint32_t value = pins[i].value.load();
        const uint32_t function = pins[i].function.load();
        const uint32_t direction = pins[i].direction.load();

        if (i < last_pin_value_.size() && value != last_pin_value_[i]) {
            DebugEvent ev{};
            ev.time = now_;
            ev.type = DebugEventType::Pin;
            ev.u.pin = PinEvent{static_cast<uint32_t>(i), value, function,
                                direction, 1};
            if (!stream_.push(ev)) MCUS_WARN("event stream full (pin)");
            last_pin_value_[i] = value;

            // 外部驱动的输入引脚变化 -> GPIO 边沿中断。
            if (model_.gpio_irq.present && direction == 0 && function == 0 &&
                pd.source_reg != 0 && model_.gpio_irq.enable_bits != 0) {
                const uint32_t bit =
                    (pd.number - model_.gpio_irq.enable_bit_base) %
                    model_.gpio_irq.enable_bits;
                if (gpio_enable & (1u << bit))
                    set_irq_pending(model_.gpio_irq.irq);
            }
        }
        if (i < last_pin_function_.size() &&
            function != last_pin_function_[i]) {
            DebugEvent ev{};
            ev.time = now_;
            ev.type = DebugEventType::Pin;
            ev.u.pin = PinEvent{static_cast<uint32_t>(i), value, function,
                                direction, 2};
            if (!stream_.push(ev)) MCUS_WARN("event stream full (pin)");
            last_pin_function_[i] = function;
        }
        if (i < last_pin_direction_.size() &&
            direction != last_pin_direction_[i]) {
            DebugEvent ev{};
            ev.time = now_;
            ev.type = DebugEventType::Pin;
            ev.u.pin = PinEvent{static_cast<uint32_t>(i), value, function,
                                direction, 4};
            if (!stream_.push(ev)) MCUS_WARN("event stream full (pin)");
            last_pin_direction_[i] = direction;
        }
    }
}

void Cpu::on_peripheral_mmio_write(const SpaceId& space, Addr addr,
                                   uint32_t value, uint8_t width) {
    (void)width;

    for (size_t i = 0; i < model_.uarts.size(); ++i) {
        const UartDesc& u = model_.uarts[i];
        if (space != u.space || addr != u.data) continue;
        UartRuntime& rt = uart_rt_[i];
        uint8_t byte = static_cast<uint8_t>(value);
        emit_bus_event(static_cast<uint32_t>(i), BusEventKind::UartTx,
                       BusDirection::ToHost, next_bus_xfer_id_++, &byte, 1);
        if (u.tx_cycles == 0) {
            uint32_t ctrl = read_reg8(u.space, u.ctrl);
            ctrl |= (1u << u.tx_flag_bit);
            write_reg8(u.space, u.ctrl, ctrl);
        } else {
            rt.tx_pending = true;
            rt.tx_ready = now_ + u.tx_cycles;
        }
    }

    for (size_t i = 0; i < model_.spis.size(); ++i) {
        const SpiDesc& s = model_.spis[i];
        if (space != s.space || addr != s.data) continue;
        SpiRuntime& rt = spi_rt_[i];
        if (rt.busy) {
            MCUS_WARN("spi write while busy, byte dropped");
            continue;
        }
        rt.busy = true;
        rt.tx = static_cast<uint8_t>(value);
        // 没有外部程序时用空闲高电平 0xFF；有外部程序时等待 SpiXfer 响应。
        rt.rx = 0xFF;
        rt.responded = false;
        rt.xfer_id = next_bus_xfer_id_++;
        rt.ready_at = now_ + s.xfer_cycles;

        uint32_t stat = read_reg8(s.space, s.stat);
        stat &= static_cast<uint32_t>(~(1u << s.done_bit));
        stat |= (1u << s.busy_bit);
        write_reg8(s.space, s.stat, stat);

        uint8_t d = rt.tx;
        emit_bus_event(static_cast<uint32_t>(i), BusEventKind::SpiXfer,
                       BusDirection::ToHost, rt.xfer_id, &d, 1);
    }

    for (size_t i = 0; i < model_.adcs.size(); ++i) {
        const AdcDesc& a = model_.adcs[i];
        if (space != a.space || addr != a.ctrl) continue;
        AdcRuntime& rt = adc_rt_[i];
        if (!(value & (1u << a.start_bit)) || rt.busy) continue;
        rt.busy = true;
        rt.channel = static_cast<uint8_t>((read_reg8(a.space, a.cfg) >> a.channel_shift) & a.channel_mask);
        rt.ready_at = now_ + a.conversion_cycles;

        uint32_t ctrl = value;
        ctrl &= static_cast<uint32_t>(~(1u << a.start_bit));
        ctrl |= (1u << a.busy_bit);
        write_reg8(a.space, a.ctrl, ctrl);
    }

    for (size_t i = 0; i < model_.tkeys.size(); ++i) {
        const TkeyDesc& t = model_.tkeys[i];
        if (space != t.space || addr != t.ctrl) continue;
        TkeyRuntime& rt = tkey_rt_[i];
        if (!(value & (1u << t.start_bit)) || rt.busy) continue;
        rt.busy = true;
        rt.ready_at = now_ + t.scan_cycles;
        uint32_t ctrl = value & static_cast<uint32_t>(~(1u << t.start_bit));
        write_reg8(t.space, t.ctrl, ctrl);
    }

    for (size_t i = 0; i < model_.pwms.size(); ++i) {
        const PwmDesc& p = model_.pwms[i];
        if (space != p.space || addr != p.ctrl) continue;
        pwm_rt_[i].started = true;
    }

    if (model_.usb.present && space == model_.usb.space) {
        for (const auto& ep : model_.usb.endpoints) {
            if (addr != ep.ctrl) continue;
            uint8_t d = static_cast<uint8_t>(value);
            emit_usb_event(next_bus_xfer_id_++, UsbToken::EpReady, ep.index,
                           UsbHandshake::None, &d, 1);
        }
    }
}

uint32_t Cpu::on_peripheral_mmio_read(const SpaceId& space, Addr addr,
                                      uint32_t value, uint8_t width) {
    (void)width;

    for (size_t i = 0; i < model_.uarts.size(); ++i) {
        const UartDesc& u = model_.uarts[i];
        if (space != u.space || addr != u.data) continue;
        UartRuntime& rt = uart_rt_[i];
        uint32_t out = value;
        if (!rt.rx.empty()) {
            out = rt.rx.front();
            rt.rx.pop_front();
        }
        uint32_t ctrl = read_reg8(u.space, u.ctrl);
        if (rt.rx.empty())
            ctrl &= static_cast<uint32_t>(~(1u << u.rx_flag_bit));
        else
            ctrl |= (1u << u.rx_flag_bit);
        write_reg8(u.space, u.ctrl, ctrl);
        return out;
    }
    return value;
}

uint32_t Cpu::read_usb_byte(uint8_t ep, uint32_t offset) {
    uint32_t base = model_.usb.buffer_base;
    for (const auto& e : model_.usb.endpoints) {
        if (e.index == ep) {
            base += e.buffer_offset + e.tx_buffer_offset;
            break;
        }
    }
    return read8(model_.usb.buffer_space, base + offset);
}

void Cpu::write_usb_byte(uint8_t ep, uint32_t offset, uint8_t value) {
    uint32_t base = model_.usb.buffer_base;
    for (const auto& e : model_.usb.endpoints) {
        if (e.index == ep) {
            base += e.buffer_offset;
            break;
        }
    }
    write8(model_.usb.buffer_space, base + offset, value);
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
    poll_bus_in();
    poll_external_pins();
    // 同步总线等待外部响应时冻结 CPU/虚拟时间，由 Simulator 让出线程。
    if (waiting_for_bus()) return 0;
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