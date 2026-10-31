#include "mcus/event_stream.h"
#include <sys/mman.h>
#include <cassert>
#include <iostream>

using namespace mcus;

static void cleanup(const char* name) { ::shm_unlink(name); }

int main() {
    const char* name = "/mcus_test_stream";
    cleanup(name);
    {
        EventStream s(name, 4);
        assert(s.empty());
        assert(!s.full());

        MmioEvent ev{};
        ev.time = 1;
        ev.addr = 0x1000;
        ev.value = 42;
        ev.width = 32;
        ev.access = MmioAccess::Write;
        ev.space_idx = 0;

        assert(s.push(ev));
        assert(s.size() == 1);
        assert(!s.empty());

        MmioEvent out{};
        assert(s.pop(out));
        assert(out.addr == 0x1000);
        assert(out.value == 42);
        assert(out.space_idx == 0);
        assert(s.empty());

        for (int i = 0; i < 4; ++i) {
            ev.value = static_cast<uint32_t>(i);
            assert(s.push(ev));
        }
        assert(s.full());
        assert(!s.push(ev));
        assert(s.backpressure());
        s.clear_backpressure();
        assert(!s.backpressure());
    }
    cleanup(name);
    std::cout << "test_event_stream passed\n";
    return 0;
}