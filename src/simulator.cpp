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

Simulator::Simulator(Model model, std::vector<uint8_t> firmware, Config cfg)
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
            bool was_paused = cpu_.paused();
            cpu_.set_paused(false);
            for (uint32_t i = 0; i < n && !cpu_.halted(); ++i) {
                VirtTime dt = cpu_.step();
                if (dt == 0) break;
                virtual_time_ += dt;
                shm_.control()->virtual_time.store(virtual_time_, std::memory_order_relaxed);
                advance_clock(dt);
            }
            if (was_paused) {
                cpu_.set_paused(true);
                ctl->status.store(static_cast<uint32_t>(ControlStatus::Paused));
            }
            break;
        }
        case ControlCommand::Dump: {
            do_dump(ctl->dump_addr.load(), ctl->dump_len.load());
            ctl->dump_ready.store(1);
            ctl->status.store(static_cast<uint32_t>(ControlStatus::DumpDone));
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
    // 默认 dump code 空间；外部可扩展 dump_space 字段
    const SpaceId space = model_.reset_space;
    if (model_.find_region(space, addr)) {
        copied = cpu_.export_memory(space, addr, std::min<size_t>(len, cap), dst);
    } else {
        // 尝试所有空间
        for (const auto& r : model_.regions) {
            if (r.is_mmio) {
                if (model_.find_region(r.space, addr)) {
                    copied = shm_.export_mmio(r.space, addr,
                                              std::min<size_t>(len, cap), dst);
                    break;
                }
            } else if (model_.find_region(r.space, addr)) {
                copied = cpu_.export_memory(r.space, addr,
                                            std::min<size_t>(len, cap), dst);
                break;
            }
        }
    }
    if (!cfg_.debug.dump_prefix.empty()) {
        std::string path = cfg_.debug.dump_prefix + "_" +
                           std::to_string(addr) + ".bin";
        std::ofstream f(path, std::ios::binary);
        if (f.is_open())
            f.write(reinterpret_cast<const char*>(dst),
                    static_cast<std::streamsize>(copied));
    }
    MCUS_INFO("dump copied " + std::to_string(copied) + " bytes");
}

void Simulator::do_reset() {
    cpu_.reset();
    virtual_time_ = 0;
    shm_.control()->virtual_time.store(0);
    shm_.control()->status.store(static_cast<uint32_t>(ControlStatus::Running));
}

void Simulator::advance_clock(VirtTime delta) {
    for (const auto& t : model_.timers) {
        const auto& sp = t.counter.reg_space;
        uint32_t ctrl = shm_.read_reg(t.enable.space, t.enable.reg, 32);
        if (!(ctrl & (1u << t.enable.bit))) continue;

        uint32_t lo = shm_.read_reg(sp, t.counter.lo_reg, 32);
        uint32_t hi = (t.counter.bits > 8) ? shm_.read_reg(sp, t.counter.hi_reg, 32) : 0;
        uint64_t cur = (static_cast<uint64_t>(hi) << 8) | lo;
        uint64_t mask = (t.counter.bits >= 64) ? ~0ull : ((1ull << t.counter.bits) - 1);

        uint64_t next;
        bool overflow;
        if (t.counter.up) {
            next = (cur + delta) & mask;
            overflow = ((cur + delta) > mask);
        } else {
            if (delta > cur) {
                next = mask - ((delta - cur - 1) & mask);
                overflow = true;
            } else {
                next = cur - delta;
                overflow = false;
            }
        }

        if (overflow) {
            if (t.overflow_action == "reload") {
                uint32_t rlo = shm_.read_reg(sp, t.counter.reload_lo_reg, 32);
                uint32_t rhi = (t.counter.bits > 8)
                                   ? shm_.read_reg(sp, t.counter.reload_hi_reg, 32)
                                   : 0;
                next = (static_cast<uint64_t>(rhi) << 8) | rlo;
            } else if (t.overflow_action == "reset") {
                shm_.control()->reset_pin.store(1);
                next = 0;
            }
            uint32_t st = shm_.read_reg(t.overflow.space, t.overflow.reg, 32);
            st |= (1u << t.overflow.bit);
            shm_.write_reg(t.overflow.space, t.overflow.reg, st, 32);
        }

        shm_.write_reg(sp, t.counter.lo_reg,
                       static_cast<uint32_t>(next & 0xFF), 32);
        if (t.counter.bits > 8)
            shm_.write_reg(sp, t.counter.hi_reg,
                           static_cast<uint32_t>((next >> 8) & 0xFF), 32);
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
                break;
            }
            std::this_thread::yield();
            continue;
        }
        virtual_time_ += dt;
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
        if (cpu_.halted()) break;
    }
    MCUS_INFO("clock loop exited");
}

} // namespace mcus