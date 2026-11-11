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
#include <sys/mman.h>
#include <cassert>
#include <iostream>

using namespace mcus;

static void cleanup(const char* n) { ::shm_unlink(n); }

int main() {
    Model m = Model::load_from_file(MCUS_TEST_MODEL_PATH);
    const char* shm_name = "/mcus_test_shm";
    cleanup(shm_name);
    {
        SharedMemory shm(m, shm_name);
        auto* ctl = shm.control();
        assert(ctl->virtual_time.load() == 0);
        assert(ctl->status.load() ==
               static_cast<uint32_t>(ControlStatus::Running));

        shm.write_reg("mmio", 1073741824, 0xDEADBEEF, 32);
        assert(shm.read_reg("mmio", 1073741824, 32) == 0xDEADBEEF);

        PinState* pins = shm.pins();
        assert(shm.pin_count() == m.pins.size());
        pins[0].value.store(1);
        assert(pins[0].value.load() == 1);
        assert(pins[0].function.load() == m.pins[0].reset_function);

        shm.reset_all();
        assert(shm.read_reg("mmio", 1073741824, 32) == 0);
        assert(pins[0].value.load() == 0);

        uint8_t* bm = shm.interrupt_bitmap();
        assert(shm.interrupt_bitmap_size() >= 4);
        bm[0] = 0xFF;
        assert(bm[0] == 0xFF);
        shm.reset_all();
        assert(bm[0] == 0);

        static_assert(sizeof(PinState) == 12, "PinState ABI layout");
        assert(shm.pin_hold_size() == shm.pin_count());
        shm.set_pin_value(0, 1);
        assert(shm.pins()[0].value.load() == 1);
        assert(shm.pin_hold()[0] == 1);
        shm.release_pin(0);
        assert(shm.pin_hold()[0] == 0);

        auto* ih = shm.inject_header();
        assert(ih->capacity.load() == shm.inject_capacity());
    }
    cleanup(shm_name);
    std::cout << "test_shared_memory passed\n";
    return 0;
}