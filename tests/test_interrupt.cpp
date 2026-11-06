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

// 0x00: JMP 0（死循环）
// 0x10: HALT（中断向量）
static std::vector<uint8_t> make_firmware() {
    std::vector<uint8_t> fw(0x20, 0x00);
    auto u32 = [&](size_t off, uint32_t v) {
        for (int i = 0; i < 4; ++i) fw[off + i] = (v >> (8 * i)) & 0xFF;
    };
    fw[0] = 0x03;       // JMP
    u32(1, 0x0000);     // -> 0
    fw[0x10] = 0x04;    // HALT
    return fw;
}

int main() {
    Model m = Model::load_from_file(MCUS_TEST_MODEL_PATH);

    // 模拟 UART/SPI 这类“进入中断后仍需软件清标志”的中断源
    assert(!m.interrupts.empty());
    m.interrupts[0].clear_pending = false;

    const char* shm_name = "/mcus_test_irq_shm";
    const char* stream_name = "/mcus_test_irq_stream";
    cleanup(shm_name);
    cleanup(stream_name);
    {
        SharedMemory shm(m, shm_name);
        EventStream stream(stream_name, 16);
        Cpu cpu(m, shm, stream);
        cpu.load_firmware(make_firmware());

        // 使能全局中断和 IRQ1，并手工置挂起位
        shm.write_reg("mmio", 1073741844, 1, 32); // IRQ_ENABLE / EA
        shm.write_reg("mmio", 1073741848, 1, 32); // IRQ_PENDING

        const uint32_t sp_before = cpu.regs().sp;

        // 第一步：进入中断，PC=向量 0x10，压入 2 字节 PC
        VirtTime dt = cpu.step();
        assert(dt > 0);
        assert(cpu.regs().pc == 0x10);
        assert(cpu.regs().sp == sp_before - 2);
        assert(!cpu.halted());

        // 第二步：必须执行向量处指令（HALT），不能再次重入同一个中断。
        // 修复前这里会再次进入中断，PC 仍是 0x10，SP 再减 2。
        dt = cpu.step();
        assert(dt > 0);
        assert(cpu.halted());
        assert(cpu.regs().sp == sp_before - 2);

        // 复位清空中断服务栈
        cpu.reset();
        assert(!cpu.halted());

        std::cout << "test_interrupt passed\n";
    }
    cleanup(shm_name);
    cleanup(stream_name);
    return 0;
}
