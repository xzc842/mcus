#include "mcus/model.h"
#include "mcus/shared_memory.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

static void usage(const char* prog) {
    std::cerr << "usage: " << prog
              << " <model.json> <shm_name>\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    try {
        mcus::Model m = mcus::Model::load_from_file(argv[1]);
        mcus::SharedMemory shm(m, argv[2]);

        auto* ctl = shm.control();
        std::printf("=== control ===\n");
        std::printf("command      = %u\n", ctl->command.load());
        std::printf("status       = %u\n", ctl->status.load());
        std::printf("virtual_time = %llu\n",
                    static_cast<unsigned long long>(ctl->virtual_time.load()));
        std::printf("pc           = 0x%X\n", ctl->pc.load());
        std::printf("halted       = %u\n", ctl->halted.load());
        std::printf("reset_pin    = %u\n", ctl->reset_pin.load());
        std::printf("dump_ready   = %u\n", ctl->dump_ready.load());

        std::printf("\n=== registers ===\n");
        for (const auto& rd : m.registers) {
            uint32_t v = shm.read_reg(rd.space, rd.addr, rd.width);
            std::printf("%-16s [%s] @0x%08X width=%u value=0x%X\n",
                rd.name.c_str(), rd.space.c_str(), rd.addr, rd.width, v);
        }

        std::printf("\n=== pins ===\n");
        auto* pins = shm.pins();
        for (size_t i = 0; i < m.pins.size(); ++i) {
            std::printf("%-8s func=%u value=%u dir=%u\n",
                        m.pins[i].name.c_str(),
                        pins[i].function.load(),
                        pins[i].value.load(),
                        pins[i].direction.load());
        }

        std::printf("\n=== interrupt bitmap ===\n");
        uint8_t* bm = shm.interrupt_bitmap();
        size_t n = shm.interrupt_bitmap_size();
        for (size_t i = 0; i < n; ++i) {
            std::printf("%02X ", bm[i]);
        }
        std::printf("\n");
    } catch (const std::exception& e) {
        std::cerr << "dump_shm error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}