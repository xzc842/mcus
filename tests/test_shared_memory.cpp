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

        auto* ih = shm.inject_header();
        assert(ih->capacity.load() == shm.inject_capacity());
    }
    cleanup(shm_name);
    std::cout << "test_shared_memory passed\n";
    return 0;
}