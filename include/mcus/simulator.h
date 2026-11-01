#pragma once
#include "types.h"
#include "model.h"
#include "shared_memory.h"
#include "event_stream.h"
#include "cpu.h"
#include "config.h"
#include <atomic>
#include <thread>
#include <vector>

namespace mcus {

class Simulator {
public:
    Simulator(Model model, std::vector<uint8_t> firmware, Config cfg);
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;

    void run();
    void stop();

private:
    void cpu_loop();
    void clock_loop();
    void handle_control_commands();
    void do_dump(Addr addr, uint32_t len);
    void do_reset();
    void advance_clock(VirtTime delta);

    Model model_;
    std::vector<uint8_t> firmware_;
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
};

} // namespace mcus