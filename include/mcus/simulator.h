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
#include "shared_memory.h"
#include "event_stream.h"
#include "cpu.h"
#include "config.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace mcus {

class Simulator {
public:
    Simulator(Model model, FirmwareImage firmware, Config cfg);
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    void run();
    void stop();

#ifdef MCUS_TEST
    // 仅供单元测试：在无 CPU 线程的情况下直接推进外设时钟。
    void advance_clock_for_test(VirtTime delta) { advance_clock(delta); }
    Cpu& cpu_for_test() { return cpu_; }
    SharedMemory& shm_for_test() { return shm_; }
#endif

private:
    void cpu_loop();
    void clock_loop();
    void handle_control_commands();
    void do_dump(Addr addr, uint32_t len);
    void do_reset();
    void advance_clock(VirtTime delta);

    Model model_;
    FirmwareImage firmware_;
    Config cfg_;
    SharedMemory shm_;
    EventStream stream_;
    Cpu cpu_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    VirtTime virtual_time_ = 0;
    std::thread cpu_thread_;
    std::thread clock_thread_;

    VirtTime acc_clock_ = 0;
    uint32_t clock_step_interval_ = 100;
    std::vector<uint64_t> timer_acc_;

    uint32_t bus_wait_timeout_ms_ = 1000;
    bool bus_waiting_ = false;
    std::chrono::steady_clock::time_point bus_wait_start_{};
};

} // namespace mcus