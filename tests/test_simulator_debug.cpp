// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0
//
// 回归：断点命中后 cpu_loop 处于 paused，仍必须处理 Dump/Resume 命令。
// 旧逻辑在 poll % interval != 0 时停止轮询，宿主会一直等待 dump。

#include "mcus/model.h"
#include "mcus/shared_memory.h"

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

using namespace mcus;

static bool wait_for(const std::function<bool()>& pred, int ms = 1000) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

int main() {
    const Model model = Model::load_from_file(MCUS_TEST_STC90C51_MODEL_PATH);
    const std::string suffix = std::to_string(::getpid());
    const std::string shm_name = "/mcus_debug_ctrl_" + suffix;
    const std::string stream_name = shm_name + "_s";
    const std::string bin = "/tmp/mcus_debug_ctrl_" + suffix + ".bin";
    const std::string cfg = "/tmp/mcus_debug_ctrl_" + suffix + ".toml";

    // 0x00: MOV A,#1 (2 bytes); 0x02: SJMP $
    const unsigned char fw[] = {0x74, 0x01, 0x80, 0xFE};
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
            << "clock_step_interval = 1\n"
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

    {
        SharedMemory shm(model, shm_name, false);
        auto* ctl = shm.control();
        assert(wait_for([&] {
            return ctl->status.load() ==
                   static_cast<uint32_t>(ControlStatus::Paused);
        }));

        // 断点 0x02；让命中时 poll 不在轮询点。
        auto* bh = shm.breakpoint_header();
        auto* slots = shm.breakpoints();
        slots[0].addr.store(0x02);
        slots[0].enabled.store(1);
        bh->count.store(1);
        bh->sorted.store(1);
        bh->min_addr.store(0x02);
        bh->max_addr.store(0x02);
        bh->hit.store(0);
        bh->hit_addr.store(0);

        ctl->command.store(static_cast<uint32_t>(ControlCommand::Resume));
        assert(wait_for([&] { return bh->hit.load() != 0; }));
        assert(wait_for([&] {
            return ctl->status.load() ==
                   static_cast<uint32_t>(ControlStatus::Paused);
        }));
        assert(ctl->pc.load() == 0x02);

        // 断点暂停状态下 Dump 必须被处理。
        ctl->dump_space_idx.store(
            static_cast<uint32_t>(model.space_index("iram")));
        ctl->dump_addr.store(0x20);
        ctl->dump_len.store(16);
        ctl->dump_ready.store(0);
        ctl->command.store(static_cast<uint32_t>(ControlCommand::Dump));
        assert(wait_for([&] { return ctl->dump_ready.load() != 0; }));
        assert(ctl->dump_len.load() == 16);

        // 断点暂停状态下 Resume 必须被处理。
        const uint64_t before = ctl->virtual_time.load();
        ctl->command.store(static_cast<uint32_t>(ControlCommand::Resume));
        assert(wait_for([&] {
            return ctl->virtual_time.load() > before + 100;
        }));
    }

    ::kill(pid, SIGTERM);
    int status = 0;
    ::waitpid(pid, &status, 0);
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    ::unlink(bin.c_str());
    ::unlink(cfg.c_str());
    std::puts("simulator debug control passed");
    return 0;
}
