#include "mcus/model.h"
#include "mcus/shared_memory.h"
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: dump_shm <model.json> <shm_name>\n";
        return 1;
    }

    try {
        mcus::Model model = mcus::Model::load_from_file(argv[1]);
        std::string shm_name = argv[2];

        // create=false：只读打开，不 memset、不 reset_all
        mcus::SharedMemory shm(model, shm_name, false);

        auto* ctl = shm.control();
        std::printf("=== control ===\n");
        std::printf("command     = %u\n", ctl->command.load());
        std::printf("status      = %u\n", ctl->status.load());
        std::printf("step_count  = %u\n", ctl->step_count.load());
        std::printf("virtual_time= %llu\n",
                    static_cast<unsigned long long>(ctl->virtual_time.load()));
        std::printf("pc          = 0x%04X\n", ctl->pc.load());
        std::printf("halted      = %u\n", ctl->halted.load());
        std::printf("reset_pin   = %u\n", ctl->reset_pin.load());

        std::printf("\n=== registers ===\n");
        for (const auto& rd : model.registers) {
            uint32_t v = shm.read_reg(rd.space, rd.addr, rd.width);
            std::printf("%-16s [%s] @0x%08X width=%u value=0x%X\n",
                        rd.name.c_str(), rd.space.c_str(),
                        static_cast<unsigned>(rd.addr),
                        static_cast<unsigned>(rd.width),
                        v);
        }

        std::printf("\n=== pins ===\n");
        const mcus::PinState* pins = shm.pins();
        for (size_t i = 0; i < model.pins.size(); ++i) {
            std::printf("%-8s func=%u value=%u dir=%u\n",
                        model.pins[i].name.c_str(),
                        pins[i].function.load(),
                        pins[i].value.load(),
                        pins[i].direction.load());
        }

        std::printf("\n=== interrupt bitmap ===\n");
        const uint8_t* bm = shm.interrupt_bitmap();
        for (size_t i = 0; i < shm.interrupt_bitmap_size(); ++i)
            std::printf("%02X ", bm[i]);
        std::printf("\n");
    } catch (const std::exception& e) {
        std::cerr << "dump_shm error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}