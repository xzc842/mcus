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

// 编码：0x07 space_id addr32 value32（写 32 位）
// dummy.json 里空间首次出现顺序：code=0, ram=1, mmio=2
static std::vector<uint8_t> make_firmware() {
    std::vector<uint8_t> fw;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) fw.push_back((v >> (8 * i)) & 0xFF);
    };
    fw.push_back(0x07); fw.push_back(2); u32(1073741824); u32(1); // 写 GPIO_OUT
    fw.push_back(0x04);                                          // 停机
    return fw;
}

int main() {
    Model m = Model::load_from_file(MCUS_TEST_MODEL_PATH);
    const char* shm_name = "/mcus_test_cpu_shm";
    const char* stream_name = "/mcus_test_cpu_stream";
    cleanup(shm_name); cleanup(stream_name);
    {
        SharedMemory shm(m, shm_name);
        EventStream stream(stream_name, 16);
        Cpu cpu(m, shm, stream);
        cpu.load_firmware(make_firmware());

        VirtTime dt = cpu.step();
        assert(dt > 0);
        assert(!stream.empty());
        DebugEvent ev{};
        assert(stream.pop(ev));
        assert(ev.type == DebugEventType::Mmio);
        assert(ev.u.mmio.addr == 1073741824);
        assert(ev.u.mmio.value == 1);
        assert(shm.read_reg("mmio", 1073741824, 32) == 1);

        dt = cpu.step();
        assert(cpu.halted());

        cpu.reset();
        assert(!cpu.halted());
        assert(shm.read_reg("mmio", 1073741824, 32) == 0);

        // 中断注入
        shm.write_reg("sfr", 0xA8, 0x80, 8);  // EA=1
        shm.write_reg("mmio", 1073741844, 1, 32); // IRQ_ENABLE
        shm.write_reg("mmio", 1073741848, 1, 32); // IRQ_PENDING
        cpu.load_firmware(make_firmware());
        dt = cpu.step();
        assert(dt > 0);
        assert(cpu.regs().pc == 16);
        assert(shm.read_reg("mmio", 1073741848, 32) == 0);
    }
    cleanup(shm_name); cleanup(stream_name);
    std::cout << "test_cpu_minimal passed\n";
    return 0;
}