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

#include "mcus/simulator.h"
#include "mcus/log.h"
#include <chrono>
#include <fstream>
#include <cstring>
#include <algorithm>

namespace mcus {

Simulator::Simulator(Model model, FirmwareImage firmware, Config cfg)
    : model_(std::move(model)), firmware_(std::move(firmware)),
      cfg_(std::move(cfg)),
      shm_(model_, cfg_.simulator.shm_name),
      stream_(cfg_.simulator.stream_name, cfg_.simulator.stream_capacity),
      cpu_(model_, shm_, stream_) {
    cpu_.load_firmware(firmware_);
    if (cfg_.simulator.start_paused) {
        shm_.control()->status.store(static_cast<uint32_t>(ControlStatus::Paused));
        cpu_.set_paused(true);
    }
    clock_step_interval_ = cfg_.simulator.clock_step_interval;
    if (clock_step_interval_ == 0) clock_step_interval_ = 1;
    bus_wait_timeout_ms_ = cfg_.simulator.bus_wait_timeout_ms;
    timer_acc_.assign(model_.timers.size(), 0);
}

Simulator::~Simulator() { stop(); }

void Simulator::run() {
    MCUS_INFO("simulator run enter");
    running_.store(true);
    stop_requested_.store(false);
    cpu_thread_ = std::thread([this] { cpu_loop(); });
    clock_thread_ = std::thread([this] { clock_loop(); });
    cpu_thread_.join();
    MCUS_INFO("cpu thread joined");
    stop_requested_.store(true);
    clock_thread_.join();
    MCUS_INFO("clock thread joined");
    running_.store(false);
}

void Simulator::stop() {
    stop_requested_.store(true);
    if (cpu_thread_.joinable()) cpu_thread_.join();
    if (clock_thread_.joinable()) clock_thread_.join();
    running_.store(false);
}

void Simulator::handle_control_commands() {
    auto* ctl = shm_.control();
    uint32_t cmd = ctl->command.exchange(0);
    switch (static_cast<ControlCommand>(cmd)) {
        case ControlCommand::Pause:
            cpu_.set_paused(true);
            ctl->status.store(static_cast<uint32_t>(ControlStatus::Paused));
            break;
        case ControlCommand::Resume:
            cpu_.set_paused(false);
            shm_.breakpoint_header()->hit.store(0);
            shm_.breakpoint_header()->hit_addr.store(0);
            ctl->status.store(static_cast<uint32_t>(ControlStatus::Running));
            break;
        case ControlCommand::Step: {
            shm_.breakpoint_header()->hit.store(0);
            shm_.breakpoint_header()->hit_addr.store(0);
            uint32_t n = ctl->step_count.load();
            if (n == 0) n = 1;
            cpu_.set_paused(false);
            for (uint32_t i = 0; i < n && !cpu_.halted(); ++i) {
                VirtTime dt = cpu_.step();
                if (dt == 0) break;
                cpu_.advance_peripherals(dt);
                virtual_time_ = cpu_.now();
                shm_.control()->virtual_time.store(virtual_time_, std::memory_order_relaxed);
                advance_clock(dt);
            }
            // 调试语义：单步结束后一定暂停，让宿主能读状态
            cpu_.set_paused(true);
            ctl->status.store(static_cast<uint32_t>(
                cpu_.halted() ? ControlStatus::Halted : ControlStatus::Paused));
            break;
        }
        case ControlCommand::Dump: {
            do_dump(ctl->dump_addr.load(), ctl->dump_len.load());
            ctl->dump_ready.store(1);
            ctl->status.store(static_cast<uint32_t>(
                cpu_.halted()
                    ? ControlStatus::Halted
                    : (cpu_.paused() ? ControlStatus::Paused
                                     : ControlStatus::Running)));
            break;
        }
        case ControlCommand::Reset:
            do_reset();
            break;
        default:
            break;
    }
}

void Simulator::do_dump(Addr addr, uint32_t len) {
    uint8_t* dst = shm_.dump_buffer();
    size_t cap = shm_.dump_buffer_size();
    size_t copied = 0;
    SpaceId space;
    bool space_found = false;

    const uint32_t idx = shm_.control()->dump_space_idx.load();
    if (idx != UINT32_MAX && idx < model_.space_order.size()) {
        space = model_.space_order[idx];
        space_found = true;
    }

    if (space_found) {
        const MemoryRegion* region = model_.find_region(space, addr);
        if (region) {
            if (region->is_mmio) {
                copied = shm_.export_mmio(space, addr,
                                          std::min<size_t>(len, cap), dst);
            } else {
                copied = cpu_.export_memory(space, addr,
                                            std::min<size_t>(len, cap), dst);
            }
        }
    } else {
        // 兼容旧行为：默认 reset_space，再尝试所有 region
        space = model_.reset_space;
        if (model_.find_region(space, addr)) {
            copied = cpu_.export_memory(space, addr,
                                        std::min<size_t>(len, cap), dst);
        } else {
            for (const auto& r : model_.regions) {
                if (r.is_mmio) {
                    if (model_.find_region(r.space, addr)) {
                        copied = shm_.export_mmio(r.space, addr,
                                                  std::min<size_t>(len, cap), dst);
                        space = r.space;
                        break;
                    }
                } else if (model_.find_region(r.space, addr)) {
                    copied = cpu_.export_memory(r.space, addr,
                                                std::min<size_t>(len, cap), dst);
                    space = r.space;
                    break;
                }
            }
        }
    }

    // dump_len 回写实际拷贝长度，供宿主判断
    shm_.control()->dump_len.store(static_cast<uint32_t>(copied));

    if (!cfg_.debug.dump_prefix.empty()) {
        std::string path = cfg_.debug.dump_prefix + "_" + space + "_" +
                           std::to_string(addr) + ".bin";
        std::ofstream f(path, std::ios::binary);
        if (f.is_open())
            f.write(reinterpret_cast<const char*>(dst),
                    static_cast<std::streamsize>(copied));
    }
    MCUS_INFO("dump copied " + std::to_string(copied) + " bytes from " +
              (space.empty() ? "?" : space));
}

void Simulator::do_reset() {
    // reset_all 会清断点，先保存再恢复
    auto* bh = shm_.breakpoint_header();
    auto* slots = shm_.breakpoints();
    const uint32_t bp_count = std::min<uint32_t>(
        bh->count.load(), static_cast<uint32_t>(shm_.breakpoint_capacity()));
    std::vector<std::pair<uint32_t, uint32_t>> saved;
    saved.reserve(bp_count);
    for (uint32_t i = 0; i < bp_count; ++i) {
        saved.emplace_back(slots[i].addr.load(), slots[i].enabled.load());
    }
    const bool was_paused = cpu_.paused();

    cpu_.reset();
    virtual_time_ = 0;
    shm_.control()->virtual_time.store(0);
    shm_.control()->status.store(static_cast<uint32_t>(
        was_paused ? ControlStatus::Paused : ControlStatus::Running));
    if (was_paused) cpu_.set_paused(true);

    for (uint32_t i = 0; i < bp_count; ++i) {
        slots[i].addr.store(saved[i].first);
        slots[i].enabled.store(saved[i].second);
    }
    bh->count.store(bp_count);
    bh->hit.store(0);
    bh->hit_addr.store(0);
    if (!saved.empty()) {
        bh->sorted.store(1);
        bh->min_addr.store(saved.front().first);
        bh->max_addr.store(saved.back().first);
    }
}

void Simulator::advance_clock(VirtTime delta) {
    auto reg_width = [&](const SpaceId& space, Addr addr) -> uint8_t {
        const RegisterDesc* r = model_.find_register(space, addr);
        return r ? r->width : 8;
    };

    for (size_t ti = 0; ti < model_.timers.size(); ++ti) {
        const auto& t = model_.timers[ti];
        const auto& sp = t.counter.reg_space;

        const uint8_t enable_w = reg_width(t.enable.space, t.enable.reg);
        uint32_t ctrl = shm_.read_reg(t.enable.space, t.enable.reg, enable_w);
        if (!(ctrl & (1u << t.enable.bit))) continue;

        uint64_t prescaler = t.prescaler == 0 ? 1 : t.prescaler;
        if (t.prescaler_reg != 0) {
            const uint8_t pw = reg_width(t.prescaler_space, t.prescaler_reg);
            const uint32_t psc = shm_.read_reg(t.prescaler_space,
                                               t.prescaler_reg, pw);
            prescaler = (static_cast<uint64_t>(psc) + 1) * t.prescaler_scale;
            if (prescaler == 0) prescaler = 1;
        }
        const uint64_t total = timer_acc_[ti] + delta;
        const uint64_t ticks = total / prescaler;
        timer_acc_[ti] = total % prescaler;
        if (ticks == 0) continue;

        // bits > 8 且 hi_reg 非 0：两个 8 位寄存器拼成计数器；
        // 否则 lo_reg 本身就是一个 bits 位的寄存器（8/16/32）。
        const bool dual = t.counter.bits > 8 && t.counter.hi_reg != 0;
        auto read_counter_reg = [&](Addr addr) -> uint64_t {
            return shm_.read_reg(sp, addr, reg_width(sp, addr));
        };

        uint64_t cur = dual
                           ? ((read_counter_reg(t.counter.hi_reg) << 8) |
                              read_counter_reg(t.counter.lo_reg))
                           : read_counter_reg(t.counter.lo_reg);
        const uint64_t mask =
            (t.counter.bits >= 64) ? ~0ull : ((1ull << t.counter.bits) - 1);
        const uint64_t reload = dual
            ? ((read_counter_reg(t.counter.reload_hi_reg) << 8) |
               read_counter_reg(t.counter.reload_lo_reg))
            : read_counter_reg(t.counter.reload_lo_reg);

        uint64_t next;
        bool overflow;
        if (t.reload_compare) {
            const uint64_t period = (reload < mask ? reload : mask) + 1;
            if (t.counter.up) {
                const uint64_t total_count = cur + ticks;
                overflow = total_count >= period;
                next = total_count % period;
            } else {
                if (ticks > cur) {
                    const uint64_t remaining = ticks - cur;
                    next = period - 1 -
                           ((remaining - 1) % period);
                    overflow = true;
                } else {
                    next = cur - ticks;
                    overflow = false;
                }
            }
        } else if (t.counter.up) {
            next = (cur + ticks) & mask;
            overflow = ((cur + ticks) > mask);
        } else {
            if (ticks > cur) {
                next = mask - ((ticks - cur - 1) & mask);
                overflow = true;
            } else {
                next = cur - ticks;
                overflow = false;
            }
        }

        if (overflow) {
            if (t.overflow_action == "reload") {
                next = dual
                           ? ((read_counter_reg(t.counter.reload_hi_reg) << 8) |
                              read_counter_reg(t.counter.reload_lo_reg))
                           : read_counter_reg(t.counter.reload_lo_reg);
            } else if (t.overflow_action == "reset") {
                shm_.control()->reset_pin.store(1);
                next = 0;
            }

            const uint8_t flag_w = reg_width(t.overflow.space, t.overflow.reg);
            uint32_t st = shm_.read_reg(t.overflow.space, t.overflow.reg, flag_w);
            st |= (1u << t.overflow.bit);
            shm_.write_reg(t.overflow.space, t.overflow.reg, st, flag_w);

            // 按模型里的 irq 号置挂起位
            if (const InterruptDesc* irq = model_.find_interrupt(t.irq)) {
                const uint8_t pw =
                    reg_width(irq->pending_space, irq->pending_reg);
                uint32_t pending = shm_.read_reg(irq->pending_space,
                                                 irq->pending_reg, pw);
                pending |= (1u << irq->pending_bit);
                shm_.write_reg(irq->pending_space, irq->pending_reg,
                               pending, pw);
            }
        }

        if (dual) {
            shm_.write_reg(sp, t.counter.lo_reg,
                           static_cast<uint32_t>(next & 0xFF),
                           reg_width(sp, t.counter.lo_reg));
            shm_.write_reg(sp, t.counter.hi_reg,
                           static_cast<uint32_t>((next >> 8) & 0xFF),
                           reg_width(sp, t.counter.hi_reg));
        } else {
            const uint8_t w = reg_width(sp, t.counter.lo_reg);
            uint64_t v = next;
            if (w < 32) v &= ((1ull << w) - 1);
            shm_.write_reg(sp, t.counter.lo_reg,
                           static_cast<uint32_t>(v), w);
        }
    }
}

void Simulator::cpu_loop() {
    MCUS_INFO("cpu loop enter");
    uint32_t poll = 0;
    while (!stop_requested_.load()) {
        //MCUS_INFO("cpu loop tick");
        auto* ctl = shm_.control();
        if (poll++ % cfg_.simulator.control_poll_interval == 0){
            handle_control_commands();
        }
        if (cpu_.paused()) {
            std::this_thread::yield();
            continue;
        }
        if (stream_.full()) {
            std::this_thread::yield();
            continue;
        }
        VirtTime dt = cpu_.step();
        if (dt == 0) {
            if (cpu_.halted()) {
                ctl->status.store(static_cast<uint32_t>(ControlStatus::Halted));
                if (!cfg_.simulator.stay_alive) break;
            }
            if (cpu_.waiting_for_bus()) {
                const auto now = std::chrono::steady_clock::now();
                if (!bus_waiting_) {
                    bus_waiting_ = true;
                    bus_wait_start_ = now;
                } else if (bus_wait_timeout_ms_ > 0 &&
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               now - bus_wait_start_).count() >=
                               bus_wait_timeout_ms_) {
                    MCUS_WARN("bus wait timeout, using default MISO");
                    cpu_.force_complete_spi();
                    bus_waiting_ = false;
                }
            } else {
                bus_waiting_ = false;
            }
            std::this_thread::yield();
            continue;
        }
        bus_waiting_ = false;
        cpu_.advance_peripherals(dt);
        virtual_time_ = cpu_.now();
        ctl->virtual_time.store(virtual_time_, std::memory_order_relaxed);
        acc_clock_ += dt;
        if (acc_clock_ >= clock_step_interval_) {
            advance_clock(acc_clock_);
            acc_clock_ = 0;
        }
    }
    MCUS_INFO("cpu loop exited");
}

void Simulator::clock_loop() {
    MCUS_INFO("clock loop enter");
    while (!stop_requested_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (cpu_.halted() && !cfg_.simulator.stay_alive) break;
    }
    MCUS_INFO("clock loop exited");
}

} // namespace mcus