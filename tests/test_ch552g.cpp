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
#include "mcus/cpu.h"
#include "mcus/event_stream.h"
#include <sys/mman.h>
#include <cassert>
#include <iostream>

using namespace mcus;

int main() {
    Model m = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);
    assert(m.arch == "8051");
    assert(m.find_register("sfr", 0xA1));
    const RegisterDesc* chip_id = m.find_register_by_name("CHIP_ID");
    assert(chip_id && chip_id->reset_value == 0x52);
    const RegisterDesc* safe_mod = m.find_register_by_name("SAFE_MOD");
    assert(safe_mod && safe_mod->read_alias == "CHIP_ID");

    SpaceId sp; Addr ba; uint8_t bit;
    assert(m.resolve_bit_addr(0x90, sp, ba, bit));
    assert(sp == "sfr" && ba == 0x90 && bit == 0);
    assert(m.resolve_bit_addr(0x00, sp, ba, bit));
    assert(sp == "iram" && ba == 0x20 && bit == 0);

    assert(!m.timers.empty());
    assert(m.find_interrupt(1));
    std::cout << "test_ch552g passed\n";
    return 0;
}