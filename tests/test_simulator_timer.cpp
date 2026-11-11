// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0
//
// 端到端回归：真实 mcus 进程 + 共享内存控制，验证
// clock_step_interval 配置下 Timer0 仍按 12T 计数。

#include "mcus/model.h"
#include "mcus/shared_memory.h"
#include "mcus/event_stream.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <csignal>

using namespace mcus;

static bool wait_status(SharedMemory& shm, ControlStatus want, int ms = 3000) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (shm.control()->status.load() == static_cast<uint32_t>(want))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

struct TimerSpec {
    const char* name;
    uint8_t tmod;
    uint8_t th_reg;
    uint8_t tl_reg;
    uint8_t tr_bit;
};

int main(int argc, char** argv) {
    uint32_t clock_interval = 1;
    int timer = 0;
    int argi = 1;
    if (argi < argc) clock_interval = static_cast<uint32_t>(std::strtoul(argv[argi++], nullptr, 10));
    if (argi < argc) timer = std::atoi(argv[argi++]);
    const TimerSpec specs[3] = {
        {"Timer0", 0x01, 0x8C, 0x8A, 0x8C},
        {"Timer1", 0x10, 0x8D, 0x8B, 0x8E},
        {"Timer2", 0x00, 0xCD, 0xCC, 0xCA},
    };
    assert(timer >= 0 && timer < 3);
    const TimerSpec& spec = specs[timer];
    Model model = Model::load_from_file(MCUS_TEST_STC90C51_MODEL_PATH);
    const std::string suffix = std::to_string(::getpid()) + "_" +
                               std::to_string(timer);
    const std::string shm_name = "/mcus_e2e_timer_" + suffix;
    const std::string stream_name = "/mcus_e2e_timer_stream_" + suffix;
    const std::string bin = "/tmp/mcus_e2e_timer_" + suffix + ".bin";
    const std::string cfg = "/tmp/mcus_e2e_timer_" + suffix + ".toml";

    // 设置 TMOD、计数器清零、启动 TRx，然后 SJMP $。
    const unsigned char fw[] = {
        0x75, 0x89, spec.tmod,
        0x75, spec.th_reg, 0x00,
        0x75, spec.tl_reg, 0x00,
        0xD2, spec.tr_bit,
        0x80, 0xFE,
    };
    {
        std::ofstream out(bin, std::ios::binary);
        out.write(reinterpret_cast<const char*>(fw), sizeof(fw));
    }
    {
        std::ofstream out(cfg);
        out << "[simulator]\n"
            << "shm_name = \"" << shm_name << "\"\n"
            << "stream_name = \"" << stream_name << "\"\n"
            << "stream_capacity = 128\n"
            << "start_paused = true\n"
            << "control_poll_interval = 64\n"
            << "clock_step_interval = " << clock_interval << "\n"
            << "\n[crystal]\n"
            << "frequency_hz = 11059200\n"
            << "cycles_per_instruction = 1\n"
            << "\n[debug]\n"
            << "log_level = \"error\"\n"
            << "log_output = \"stderr\"\n";
    }

    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());

    pid_t pid = ::fork();
    if (pid == 0) {
        ::execl(MCUS_BIN_PATH, MCUS_BIN_PATH,
                "--isa", MCUS_ISA_PATH,
                "--model", MCUS_TEST_STC90C51_MODEL_PATH,
                "--firmware", bin.c_str(),
                "--config", cfg.c_str(), nullptr);
        ::_exit(127);
    }
    assert(pid > 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    SharedMemory shm(model, shm_name, false);
    assert(wait_status(shm, ControlStatus::Paused));
    shm.control()->command.store(static_cast<uint32_t>(ControlCommand::Resume));
    assert(wait_status(shm, ControlStatus::Running));

    constexpr uint64_t kTarget = 20000;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (shm.control()->virtual_time.load() < kTarget &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(shm.control()->virtual_time.load() >= kTarget);
    shm.control()->command.store(static_cast<uint32_t>(ControlCommand::Pause));
    assert(wait_status(shm, ControlStatus::Paused));

    const uint32_t tl = shm.read_reg("sfr", spec.tl_reg, 8);
    const uint32_t th = shm.read_reg("sfr", spec.th_reg, 8);
    const uint32_t ticks = (th << 8) | tl;
    // 启动固件只占少量周期；允许边界差异。
    const uint32_t expected = kTarget / 12;
    assert(ticks + 80 >= expected);
    assert(ticks <= expected + 80);

    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    ::unlink(bin.c_str());
    ::unlink(cfg.c_str());
    std::printf("test_simulator_timer passed (%s, clock_step_interval=%u)\n",
                spec.name, clock_interval);
    return 0;
}
