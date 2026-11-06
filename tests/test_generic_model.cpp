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

int main() {
    Model m = Model::load_from_file(MCUS_TEST_GENERIC_MODEL_PATH);
    assert(m.arch == "mcus-minimal");
    assert(m.reset_space == "flash");
    assert(m.sp_space == "ram");
    assert(m.arch_desc.reset_space == "flash");
    assert(m.arch_desc.sp_space == "ram");

    const char* shm_name = "/mcus_test_generic_shm";
    const char* stream_name = "/mcus_test_generic_stream";
    cleanup(shm_name);
    cleanup(stream_name);

    {
        SharedMemory shm(m, shm_name);
        EventStream stream(stream_name, 16);

        // 0x00: JMP 0；0x10: HALT
        std::vector<uint8_t> fw(0x20, 0);
        fw[0] = 0x03;
        fw[1] = 0x00; fw[2] = 0x00; fw[3] = 0x00; fw[4] = 0x00;
        fw[0x10] = 0x04;

        Cpu cpu(m, shm, stream);
        cpu.load_firmware(fw);

        // 非 8051 空间名：flash / ram / periph，读写都必须走模型地址。
        cpu.write8("flash", 0x100, 0xAB);
        assert(cpu.read8("flash", 0x100) == 0xAB);

        // ram 的 base 是 0x10000；旧实现会按 addr & (size-1) 算错。
        cpu.write8("ram", 0x10010, 0xCD);
        assert(cpu.read8("ram", 0x10010) == 0xCD);

        cpu.write32("periph", 536870912, 0x12345678);
        assert(cpu.read32("periph", 536870912) == 0x12345678);
        assert(shm.read_reg("periph", 536870912, 32) == 0x12345678);

        // 中断向量/使能/挂起寄存器也完全来自模型。
        shm.write_reg("periph", 536870928, 1, 32);
        shm.write_reg("periph", 536870932, 1, 32);
        const uint32_t sp_before = cpu.regs().sp;
        VirtTime dt = cpu.step();
        assert(dt > 0);
        assert(cpu.regs().pc == 0x10);
        assert(cpu.regs().sp == sp_before - 2);

        dt = cpu.step();
        assert(dt > 0);
        assert(cpu.halted());

        std::cout << "test_generic_model passed\n";
    }

    cleanup(shm_name);
    cleanup(stream_name);
    return 0;
}
