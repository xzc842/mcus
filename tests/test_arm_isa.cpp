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
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"

#include <sys/mman.h>
#include <cassert>
#include <iostream>
#include <vector>

using namespace mcus;

extern "C" InstructionSet* mcus_isa_create(const Model*);
extern "C" void mcus_isa_destroy(InstructionSet*);

static void install_arm() {
    IsaRegistry::instance().register_isa(
        "arm", [](const Model& m) -> IsaPtr {
            return IsaPtr(mcus_isa_create(&m), IsaDeleter{mcus_isa_destroy});
        });
}

static void cleanup(const char* n) { ::shm_unlink(n); }

// 链接在 0x08000000：向量表 + 复位处理 + 函数调用/PUSH/POP。
// 代码把 GPIOC_ODR bit13 置 1，然后 BL 到子函数，子函数返回后死循环。
static const unsigned char kFirmware[] = {
    0xf0, 0x4f, 0x00, 0x20, 0x41, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x41, 0xf2, 0x0c, 0x01, 0xc4, 0xf2, 0x01, 0x01,
    0x4f, 0xf4, 0x00, 0x52, 0x0a, 0x60, 0x00, 0xf0, 0x01, 0xf8, 0xfe, 0xe7,
    0x00, 0xb5, 0x5a, 0x20, 0x4f, 0xea, 0x40, 0x00, 0x00, 0xbd};

int main() {
    install_arm();
    Model m = Model::load_from_file(MCUS_TEST_ARM_MODEL_PATH);
    assert(m.arch == "arm");

    const char* shm_name = "/mcus_test_arm_shm";
    const char* stream_name = "/mcus_test_arm_stream";
    cleanup(shm_name);
    cleanup(stream_name);
    {
        SharedMemory shm(m, shm_name);
        EventStream stream(stream_name, 16);
        Cpu cpu(m, shm, stream);
        cpu.load_firmware(std::vector<uint8_t>(kFirmware,
                                               kFirmware + sizeof(kFirmware)));

        // 复位向量：SP=0x20004FF0，Reset_Handler=0x08000041
        assert(cpu.regs().sp == 0x20004FF0u);
        assert(cpu.regs().pc == 0x08000040u);

        for (int i = 0; i < 32 && cpu.pc() != 0x08000052u; ++i)
            cpu.step();
        assert(cpu.pc() == 0x08000052u);

        assert(shm.read_reg("periph", 0x4001100Cu, 32) == 0x2000u);
        assert(shm.pins()[45].value.load() == 1u); // PC13
        assert(cpu.regs().r[0] == 0xB4u);          // MOVS 0x5A + LSL #1

        // ---- NVIC 中断进入/返回 ----
        {
            std::vector<uint8_t> fw(0x110, 0);
            auto put32 = [&](size_t off, uint32_t v) {
                for (int i = 0; i < 4; ++i) fw[off + i] = (v >> (8 * i)) & 0xFF;
            };
            put32(0x00, 0x20004FF0u);          // initial SP
            put32(0x04, 0x08000041u);          // Reset_Handler
            put32(4 * (16 + 28), 0x08000101u); // TIM2 vector -> handler
            put32(0x40, 0xE7FEu);              // b .
            put32(0x100, 0x2001u);             // movs r0, #1
            put32(0x102, 0x4770u);             // bx lr

            cpu.load_firmware(fw);
            shm.write_reg("system", 0xE000E100u, 1u << 28, 32); // ISER0
            shm.write_reg("system", 0xE000E200u, 1u << 28, 32); // ISPR0

            assert(cpu.step() > 0);                 // exception entry
            assert(cpu.pc() == 0x08000100u);
            assert(cpu.step() > 0);                 // movs r0,#1
            assert(cpu.regs().r[0] == 1u);
            assert(cpu.step() > 0);                 // bx lr -> exception return
            assert(cpu.pc() == 0x08000040u);
            assert((shm.read_reg("system", 0xE000E200u, 32) & (1u << 28)) == 0);
        }

        std::cout << "test_arm_isa passed\n";
    }
    cleanup(shm_name);
    cleanup(stream_name);
    return 0;
}
