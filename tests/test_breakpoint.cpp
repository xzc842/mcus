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

#include "mcus/model.h"
#include "mcus/shared_memory.h"
#include "mcus/event_stream.h"
#include "mcus/cpu.h"
#include <sys/mman.h>
#include <cassert>
#include <iostream>
#include <vector>

using namespace mcus;

static void cleanup(const char* n) { ::shm_unlink(n); }

// 编码：0x08 reg imm32（加载立即数）、0x04 停机
// 固件：r0=1, r0=2, r0=3, 停机
static std::vector<uint8_t> make_firmware() {
    std::vector<uint8_t> fw;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) fw.push_back((v >> (8 * i)) & 0xFF);
    };
    fw.push_back(0x08); fw.push_back(0); u32(1);   // r0 = 1
    fw.push_back(0x08); fw.push_back(0); u32(2);   // r0 = 2
    fw.push_back(0x08); fw.push_back(0); u32(3);   // r0 = 3
    fw.push_back(0x04);                             // 停机
    return fw;
}

int main() {
    Model m = Model::load_from_file(MCUS_TEST_MODEL_PATH);
    const char* shm_name = "/mcus_test_bp_shm";
    const char* stream_name = "/mcus_test_bp_stream";
    cleanup(shm_name); cleanup(stream_name);
    {
        SharedMemory shm(m, shm_name);
        EventStream stream(stream_name, 16);
        Cpu cpu(m, shm, stream);
        cpu.load_firmware(make_firmware());

        // 固件地址：0x00 0x08 0x00 imm32(1)，0x06 0x08 0x00 imm32(2)，0x0C 0x08 0x00 imm32(3)，0x12 0x04
        // 断点在 0x06（第二条指令开始）
        auto* bh = shm.breakpoint_header();
        auto* slots = shm.breakpoints();
        slots[0].addr.store(0x06);
        slots[0].enabled.store(1);
        bh->count.store(1);
        bh->sorted.store(1);
        bh->min_addr.store(0x06);
        bh->max_addr.store(0x06);
        bh->hit.store(0);
        bh->hit_addr.store(0);

        // 第一步：pc=0，check_breakpoint 不命中，execute r0=1，pc=0x06
        VirtTime dt = cpu.step();
        assert(dt > 0);
        assert(cpu.regs().pc == 0x06);
        assert(cpu.regs().r[0] == 1);
        assert(bh->hit.load() == 0);          // 还没到断点

        // 第二步：pc=0x06，check_breakpoint 命中，paused，step 返回 0
        dt = cpu.step();
        assert(dt == 0);
        assert(cpu.regs().pc == 0x06);
        assert(cpu.regs().r[0] == 1);
        assert(bh->hit.load() == 1);          // 断点命中
        assert(bh->hit_addr.load() == 0x06);
        assert(cpu.paused());

        // 断点命中后 step 还是返回 0
        dt = cpu.step();
        assert(dt == 0);
        assert(cpu.regs().pc == 0x06);
        assert(cpu.regs().r[0] == 1);

        // 清 hit，继续（last_bp_pc_ 让 check_breakpoint 跳过当前断点）
        bh->hit.store(0);
        bh->hit_addr.store(0);
        cpu.set_paused(false);

        // 执行 r0=2，pc=0x0C
        dt = cpu.step();
        assert(dt > 0);
        assert(cpu.regs().pc == 0x0C);
        assert(cpu.regs().r[0] == 2);
        assert(bh->hit.load() == 0);

        // 执行 r0=3，pc=0x12
        dt = cpu.step();
        assert(dt > 0);
        assert(cpu.regs().pc == 0x12);
        assert(cpu.regs().r[0] == 3);

        // 停机
        dt = cpu.step();
        assert(dt > 0);
        assert(cpu.halted());

        // 复位，断点清空
        cpu.reset();
        assert(bh->count.load() == 0);
        assert(bh->hit.load() == 0);
        assert(bh->sorted.load() == 0);
    }
    cleanup(shm_name); cleanup(stream_name);
    std::cout << "test_breakpoint passed\n";
    return 0;
}