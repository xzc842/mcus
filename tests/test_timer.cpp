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
    const char* shm_name = "/mcus_test_timer_shm";
    cleanup(shm_name);
    {
        SharedMemory shm(m, shm_name);
        assert(!m.timers.empty());
        for (const auto& t : m.timers) {
            assert(m.find_register(t.counter.reg_space, t.counter.lo_reg));
            if (t.counter.bits > 8)
                assert(m.find_register(t.counter.reg_space, t.counter.hi_reg));
            assert(m.find_register(t.counter.reg_space, t.counter.reload_lo_reg));
            assert(m.find_interrupt(t.irq));
        }

        // 手动推进一次：置使能，设重载 10，计数 9，推进 1
        const auto& t = m.timers[0];
        const auto& sp = t.counter.reg_space;
        shm.write_reg(sp, t.enable.reg, 1u << t.enable.bit, 32);
        shm.write_reg(sp, t.counter.reload_lo_reg, 10, 32);
        shm.write_reg(sp, t.counter.lo_reg, 9, 32);

        uint32_t count = shm.read_reg(sp, t.counter.lo_reg, 32) + 1;
        if (count >= 10) {
            uint32_t st = shm.read_reg(sp, t.overflow.reg, 32);
            st |= (1u << t.overflow.bit);
            shm.write_reg(sp, t.overflow.reg, st, 32);
            const InterruptDesc* irq = m.find_interrupt(t.irq);
            uint32_t pend = shm.read_reg(irq->pending_space, irq->pending_reg, 32);
            pend |= (1u << irq->pending_bit);
            shm.write_reg(irq->pending_space, irq->pending_reg, pend, 32);
        }
        uint32_t st = shm.read_reg(sp, t.overflow.reg, 32);
        assert(st & (1u << t.overflow.bit));
        const InterruptDesc* irq = m.find_interrupt(t.irq);
        uint32_t pend = shm.read_reg(irq->pending_space, irq->pending_reg, 32);
        assert(pend & (1u << irq->pending_bit));
    }
    cleanup(shm_name);
    std::cout << "test_timer passed\n";
    return 0;
}