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

    assert(!m.timers.empty());
    assert(m.find_interrupt(1));

    assert(m.uarts.size() == 2);
    assert(m.spis.size() == 1);
    assert(m.pwms.size() == 1);
    assert(m.adcs.size() == 1);
    assert(m.tkeys.size() == 1);
    assert(m.gpio_irq.present);
    assert(m.usb.present && m.usb.endpoints.size() == 5);
    assert(m.pins[0].has_direction);

    std::cout << "test_ch552g passed\n";
    return 0;
}