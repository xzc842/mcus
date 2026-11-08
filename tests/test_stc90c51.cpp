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

#include "mcus/cpu.h"
#include "mcus/event_stream.h"
#include "mcus/firmware.h"
#include "mcus/model.h"
#include "mcus/shared_memory.h"
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

using namespace mcus;

extern "C" InstructionSet* mcus_isa_create(const Model*);
extern "C" void mcus_isa_destroy(InstructionSet*);

static void install_8051() {
    IsaRegistry::instance().register_isa(
        "8051", [](const Model& m) -> IsaPtr {
            return IsaPtr(mcus_isa_create(&m), IsaDeleter{mcus_isa_destroy});
        });
}

int main() {
    install_8051();
    Model model = Model::load_from_file(MCUS_TEST_STC90C51_MODEL_PATH);
    assert(model.arch == "8051");
    assert(model.regions.size() == 4);
    assert(model.regions[0].space == "code");
    assert(model.regions[0].size == 4096);
    assert(model.regions[3].space == "xram");
    assert(model.regions[3].size == 256);
    assert(model.regions[3].addr_mask == 255);

    assert(model.pins.size() == 42);
    assert(model.pins[8].name == "P1.0");
    assert(model.pins[8].source_reg == 0x90);
    assert(model.find_register("sfr", 0xE8) != nullptr); // P4
    assert(model.find_register("sfr", 0xC0) != nullptr); // XICON
    assert(model.find_interrupt(7) != nullptr);           // INT3

    const std::string shm_name = "/mcus_stc90_" + std::to_string(::getpid());
    const std::string stream_name = shm_name + "_s";
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());

    {
        SharedMemory shm(model, shm_name);
        EventStream stream(stream_name, 64);
        Cpu cpu(model, shm, stream);

        // MOV P1, #0xAA; SJMP $
        FirmwareImage fw;
        fw.load_addr = 0;
        fw.data = {0x75, 0x90, 0xAA, 0x80, 0xFE};
        cpu.load_firmware(fw);

        assert(shm.read_reg("sfr", 0x90, 8) == 0xFF);
        VirtTime dt = cpu.step();
        assert(dt > 0);
        assert(shm.read_reg("sfr", 0x90, 8) == 0xAA);

        // P1.0=0, P1.1=1, ... 引脚值随端口输出更新。
        for (int i = 0; i < 8; ++i) {
            const uint32_t expected = (0xAAu >> i) & 1u;
            assert(shm.pins()[8 + i].value.load() == expected);
        }
        std::puts("stc90c51 ok");
    }

    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    return 0;
}
