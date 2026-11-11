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
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"
#include "mcus/model.h"
#include "mcus/shared_memory.h"

#include <sys/mman.h>
#include <cstdio>
#include <vector>

using namespace mcus;

extern "C" InstructionSet* mcus_isa_create(const Model*);
extern "C" void mcus_isa_destroy(InstructionSet*);

static void install_8051() {
    IsaRegistry::instance().register_isa(
        "8051", [](const Model& m) -> IsaPtr {
            return IsaPtr(mcus_isa_create(&m), IsaDeleter{mcus_isa_destroy});
        });
}

struct Result {
    uint32_t pc = 0;
    VirtTime now = 0;
    uint32_t acc = 0;
    uint32_t psw = 0;
    uint32_t mem[4]{};
};

static Result run(const Model& model, bool jit, const char* suffix,
                  const std::vector<uint8_t>& fw, uint64_t steps) {
    const std::string shm_name = std::string("/mcus_jit_diff_") + suffix;
    const std::string stream_name = shm_name + "_s";
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());

    Result r{};
    {
        SharedMemory shm(model, shm_name);
        EventStream stream(stream_name, 256);
        Cpu cpu(model, shm, stream);
        cpu.load_firmware(fw);

        uint64_t done = 0;
        if (jit) {
            if (!cpu.enable_jit(MCUS_TEST_JIT8051_PATH)) {
                std::puts("jit enable failed");
                return r;
            }
            done = cpu.run_jit(steps);
        }
        while (done < steps) {
            if (cpu.step() == 0) break;
            ++done;
            cpu.advance_peripherals(1);
        }

        r.pc = cpu.regs().pc;
        r.now = cpu.now();
        r.acc = cpu.read8("sfr", 0xE0);
        r.psw = cpu.read8("sfr", 0xD0);
        for (int i = 0; i < 4; ++i)
            r.mem[i] = cpu.read8("iram", 0x30 + i);
    }
    ::shm_unlink(shm_name.c_str());
    ::shm_unlink(stream_name.c_str());
    return r;
}

static bool same(const Result& a, const Result& b) {
    return a.pc == b.pc && a.now == b.now && a.acc == b.acc &&
           a.psw == b.psw && a.mem[0] == b.mem[0] &&
           a.mem[1] == b.mem[1] && a.mem[2] == b.mem[2] &&
           a.mem[3] == b.mem[3];
}

static int check(const Model& model, const char* name,
                 const std::vector<uint8_t>& fw, uint64_t steps) {
    const Result a = run(model, false, (std::string(name) + "i").c_str(), fw, steps);
    const Result b = run(model, true, (std::string(name) + "j").c_str(), fw, steps);
    std::printf("%s interp pc=%u now=%llu acc=0x%x psw=0x%x mem=%x,%x,%x,%x\n",
                name, a.pc, (unsigned long long)a.now, a.acc, a.psw,
                a.mem[0], a.mem[1], a.mem[2], a.mem[3]);
    std::printf("%s jit    pc=%u now=%llu acc=0x%x psw=0x%x mem=%x,%x,%x,%x\n",
                name, b.pc, (unsigned long long)b.now, b.acc, b.psw,
                b.mem[0], b.mem[1], b.mem[2], b.mem[3]);
    if (!same(a, b)) {
        std::printf("%s diff mismatch\n", name);
        return 1;
    }
    return 0;
}

int main() {
    install_8051();
    const Model model = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);

    // MOV A,#0x55; INC A; MOV 0x30,A; SJMP $
    if (check(model, "basic",
              {0x74, 0x55, 0x04, 0xF5, 0x30, 0x80, 0xFE}, 4) != 0)
        return 1;

    // ALU/flags: A=0x10+0x25 -> 0x35; 0x80+0x80 -> 0x00, CY/OV;
    // 0x05-0x06 -> 0xFF, CY.
    if (check(model, "alu",
              {0x74, 0x10, 0x24, 0x25, 0xF5, 0x30,
               0x74, 0x80, 0x24, 0x80, 0xF5, 0x31,
               0x74, 0x05, 0x94, 0x06, 0xF5, 0x32,
               0x80, 0xFE}, 17) != 0)
        return 1;

    // MOV A,#0; JZ +2; MOV A,#0xAA (skipped); MOV 0x33,A; SJMP $
    if (check(model, "cond",
              {0x74, 0x00, 0x60, 0x02, 0x74, 0xAA, 0xF5, 0x33,
               0x80, 0xFE}, 4) != 0)
        return 1;

    // stack + bit: MOV SP,#0x30; MOV A,#0x12; PUSH ACC; MOV A,#0;
    // POP ACC; MOV 0x30,A; SETB 0; MOV C,0; MOV A,#0; RLC A; MOV 0x31,A
    if (check(model, "stackbit",
              {0x75, 0x81, 0x30, 0x74, 0x12, 0xC0, 0xE0, 0x74, 0x00,
               0xD0, 0xE0, 0xF5, 0x30, 0xD2, 0x00, 0xA2, 0x00,
               0x74, 0x00, 0x33, 0xF5, 0x31, 0x80, 0xFE}, 12) != 0)
        return 1;

    // MOV A,#3; loop: DEC A; JNZ loop; MOV 0x30,A; SJMP $
    if (check(model, "djnz",
              {0x74, 0x03, 0x14, 0x70, 0xFD, 0xF5, 0x30, 0x80, 0xFE},
              9) != 0)
        return 1;

    // MOVX + XCH: DPTR=0x10; A=0x5A; MOVX @DPTR,A; A=0; MOVX A,@DPTR;
    // MOV 0x30,A; A=0x12; MOV 0x31,#0x34; XCH A,0x31; MOV 0x32,A
    if (check(model, "movx",
              {0x90, 0x00, 0x10, 0x74, 0x5A, 0xF0, 0x74, 0x00, 0xE0,
               0xF5, 0x30, 0x74, 0x12, 0x75, 0x31, 0x34, 0xC5, 0x31,
               0xF5, 0x32, 0x80, 0xFE}, 10) != 0)
        return 1;

    // SFR direct: MOV 0xB0,#0x55; MOV A,0xB0; MOV 0x30,A;
    // MOV A,#0xAA; MOV 0xB0,A; MOV A,0xB0; MOV 0x31,A
    if (check(model, "sfr",
              {0x75, 0xB0, 0x55, 0xE5, 0xB0, 0xF5, 0x30,
               0x74, 0xAA, 0xF5, 0xB0, 0xE5, 0xB0, 0xF5, 0x31,
               0x80, 0xFE}, 7) != 0)
        return 1;

    // RETI: SP=0x0A; stack[0x0A]=0x00, stack[0x09]=0x00; RETI -> PC=0
    if (check(model, "reti",
              {0x75, 0x81, 0x0A, 0x75, 0x0A, 0x00, 0x75, 0x09, 0x00,
               0x32}, 4) != 0)
        return 1;

    // ANL/ORL/XRL A,#imm
    if (check(model, "logic",
              {0x74, 0xF0, 0x54, 0x0F, 0x44, 0xAA, 0x64, 0xFF,
               0xF5, 0x30, 0x80, 0xFE}, 5) != 0)
        return 1;

    std::puts("jit diff ok");
    return 0;
}
