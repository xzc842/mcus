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

#include "mcus/event_stream.h"
#include <sys/mman.h>
#include <cassert>
#include <iostream>

using namespace mcus;

static void cleanup(const char* name) { ::shm_unlink(name); }

static DebugEvent make_mmio(uint32_t value) {
    DebugEvent ev{};
    ev.time = 1;
    ev.type = DebugEventType::Mmio;
    ev.u.mmio.time = 1;
    ev.u.mmio.addr = 0x1000;
    ev.u.mmio.value = value;
    ev.u.mmio.width = 32;
    ev.u.mmio.access = MmioAccess::Write;
    ev.u.mmio.space_idx = 0;
    return ev;
}

int main() {
    const char* name = "/mcus_test_stream";
    cleanup(name);
    {
        EventStream s(name, 4);
        assert(s.empty());
        assert(!s.full());

        DebugEvent ev = make_mmio(42);
        assert(s.push(ev));
        assert(s.size() == 1);
        assert(!s.empty());

        DebugEvent out{};
        assert(s.pop(out));
        assert(out.type == DebugEventType::Mmio);
        assert(out.u.mmio.addr == 0x1000);
        assert(out.u.mmio.value == 42);
        assert(out.seq == 0);
        assert(s.empty());

        // 统一事件流：PIN 事件也能入队
        DebugEvent pin{};
        pin.time = 2;
        pin.type = DebugEventType::Pin;
        pin.u.pin = PinEvent{3, 1, 0, 0, 1};
        assert(s.push(pin));
        assert(s.pop(out));
        assert(out.type == DebugEventType::Pin);
        assert(out.u.pin.pin_id == 3);
        assert(out.seq == 1);

        for (int i = 0; i < 4; ++i) {
            assert(s.push(make_mmio(static_cast<uint32_t>(i))));
        }
        assert(s.full());
        assert(!s.push(ev));
        assert(s.backpressure());
        s.clear_backpressure();
        assert(!s.backpressure());
    }

    // attach 不重置已有队列
    {
        EventStream writer(name, 4);
        assert(writer.push(make_mmio(7)));
    }
    {
        EventStream reader(name, 0, false);
        assert(!reader.empty());
        DebugEvent out{};
        assert(reader.pop(out));
        assert(out.u.mmio.value == 7);
        assert(reader.empty());
    }

    cleanup(name);
    std::cout << "test_event_stream passed\n";
    return 0;
}
