// Copyright 2026 XueZicheng <xuezheng842@outlook.com>
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

// 最小 x86-64 后端：先把 AddCycles / Check / Branch / Nop / End 编译成机器码。
// 其余 op 返回 false，由字节码执行器或解释器回退。
#include "mcus/jit.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mcus {
namespace {

constexpr size_t kPage = 4096;

struct Emitter {
    std::vector<uint8_t> code;
    std::vector<std::pair<size_t, uint32_t>> label_fixups; // patch, code index
    std::vector<std::pair<size_t, uint32_t>> je_fixups;
    std::vector<size_t> exit_fixups;
    std::vector<uint32_t> index_off;

    void u8(uint8_t v) { code.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) code.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void patch32(size_t at, uint32_t v) {
        for (int i = 0; i < 4; ++i) code[at + i] = static_cast<uint8_t>(v >> (8 * i));
    }

    // push rbx; mov rbx, rdi
    void prologue() {
        u8(0x53);                       // push rbx
        u8(0x41); u8(0x54);             // push r12
        u8(0x48); u8(0x83); u8(0xEC); u8(0x08); // sub rsp, 8
        u8(0x48); u8(0x89); u8(0xFB);   // mov rbx, rdi
    }
    void epilogue() {
        u8(0x48); u8(0x83); u8(0xC4); u8(0x08); // add rsp, 8
        u8(0x41); u8(0x5C);             // pop r12
        u8(0x5B);                       // pop rbx
        u8(0xC3);                       // ret
    }
    // add qword [rbx+disp32], imm32
    void add_now(int32_t off, int32_t imm) {
        u8(0x48); u8(0x81); u8(0x83); u32(static_cast<uint32_t>(off)); u32(static_cast<uint32_t>(imm));
    }
    // mov rdi, [rbx+disp32]
    void load_user(int32_t off) {
        u8(0x48); u8(0x8B); u8(0xBB); u32(static_cast<uint32_t>(off));
    }
    // mov esi, imm32
    void mov_esi(uint32_t imm) { u8(0xBE); u32(imm); }
    // mov esi, [rbx+disp32]
    void load_esi_ctx(int32_t off) {
        u8(0x8B); u8(0xB3); u32(static_cast<uint32_t>(off));
    }
    // mov dword [rbx+disp32], imm32
    void store_pc(int32_t off, uint32_t imm) {
        u8(0xC7); u8(0x83); u32(static_cast<uint32_t>(off)); u32(imm);
    }
    // r10 = mmio + sfr_offsets[addr]
    void load_sfr_addr(int32_t mmio_off, int32_t table_off, uint32_t addr) {
        u8(0x48); u8(0x63); u8(0x83);
        u32(static_cast<uint32_t>(table_off + static_cast<int32_t>(addr) * 4));
        u8(0x4C); u8(0x8B); u8(0x93); u32(static_cast<uint32_t>(mmio_off));
        u8(0x49); u8(0x01); u8(0xC2);
    }
    // r10 = [ctx+off]
    void load_ptr_only(int32_t off) {
        u8(0x4C); u8(0x8B); u8(0x93); u32(static_cast<uint32_t>(off));
    }
    // r10 = ptr + bias
    void load_ptr(int32_t data_off, int32_t bias_off) {
        u8(0x4C); u8(0x8B); u8(0x93); u32(static_cast<uint32_t>(data_off));
        u8(0x4C); u8(0x63); u8(0x9B); u32(static_cast<uint32_t>(bias_off));
        u8(0x4D); u8(0x01); u8(0xDA);
    }
    // mov byte [r10+disp32], imm8
    void store_imm8(int32_t off, uint8_t v) {
        u8(0x41); u8(0xC6); u8(0x82); u32(static_cast<uint32_t>(off)); u8(v);
    }
    // movzx eax, byte [r10+disp32]
    void load_eax8(int32_t off) {
        u8(0x41); u8(0x0F); u8(0xB6); u8(0x82); u32(static_cast<uint32_t>(off));
    }
    // mov byte [r10+disp32], al
    void store_al(int32_t off) {
        u8(0x41); u8(0x88); u8(0x82); u32(static_cast<uint32_t>(off));
    }
    // eax 低 8 位的奇偶性写入 PSW.P
    void parity_to_psw(int32_t psw_off) {
        u8(0x89); u8(0xC2);                 // mov edx, eax
        u8(0xC1); u8(0xEA); u8(0x04);       // shr edx, 4
        u8(0x31); u8(0xD0);                 // xor eax, edx
        u8(0x89); u8(0xC2);                 // mov edx, eax
        u8(0xC1); u8(0xEA); u8(0x02);       // shr edx, 2
        u8(0x31); u8(0xD0);                 // xor eax, edx
        u8(0x89); u8(0xC2);                 // mov edx, eax
        u8(0xC1); u8(0xEA); u8(0x01);       // shr edx, 1
        u8(0x31); u8(0xD0);                 // xor eax, edx
        u8(0x83); u8(0xE0); u8(0x01);       // and eax, 1
        u8(0x41); u8(0x0F); u8(0xB6); u8(0x92);
        u32(static_cast<uint32_t>(psw_off)); // movzx edx, byte [r10+psw]
        u8(0x83); u8(0xE2); u8(0xFE);        // and edx, ~1
        u8(0x09); u8(0xD0);                  // or eax, edx
        u8(0x41); u8(0x88); u8(0x82);
        u32(static_cast<uint32_t>(psw_off)); // mov [r10+psw], al
    }
    // rdi=ctx, esi=id, edx=a, ecx=b, call [rbx+helper_off]
    void call_helper(int32_t off, uint32_t id, uint32_t a, uint32_t b) {
        u8(0x48); u8(0x89); u8(0xDF);           // mov rdi, rbx
        u8(0xBE); u32(id);                       // mov esi, id
        u8(0xBA); u32(a);                        // mov edx, arg0
        u8(0xB9); u32(b);                        // mov ecx, arg1
        u8(0xFF); u8(0x93); u32(static_cast<uint32_t>(off)); // call [rbx+off]
    }
    // call qword [rbx+disp32]
    void call_check(int32_t off) {
        u8(0xFF); u8(0x93); u32(static_cast<uint32_t>(off));
    }
    void test_eax() { u8(0x85); u8(0xC0); }
    void jnz_placeholder() {
        u8(0x0F); u8(0x85);
        exit_fixups.push_back(code.size());
        u32(0);
    }
    void jmp_placeholder(uint32_t target_index) {
        u8(0xE9);
        label_fixups.emplace_back(code.size(), target_index);
        u32(0);
    }
    void cmp_pc_imm(int32_t pc_off, uint32_t pc) {
        u8(0x81); u8(0xBB); u32(static_cast<uint32_t>(pc_off)); u32(pc);
    }
    void je_placeholder(uint32_t target_index) {
        u8(0x0F); u8(0x84);
        je_fixups.emplace_back(code.size(), target_index);
        u32(0);
    }
};

struct CacheHeader {
    char magic[4] = {'M', 'J', 'X', '1'};
    uint32_t abi = 4;
    uint32_t size = 0;
    uint32_t guest_pc = 0;
    uint32_t sum = 0;
};

constexpr uint32_t kNativeCacheVersion = 4;

uint64_t hash_program(const BcProgram& p) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t n) {
        const auto* b = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) {
            h ^= b[i];
            h *= 1099511628211ull;
        }
    };
    const uint32_t version = kBytecodeVersion;
    mix(&version, sizeof(version));
    mix(&kNativeCacheVersion, sizeof(kNativeCacheVersion));
    mix(&p.guest_pc, sizeof(p.guest_pc));
    mix(p.code.data(), p.code.size() * sizeof(BcInsn));
    return h;
}

uint32_t checksum(const uint8_t* data, size_t n) {
    uint32_t s = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        s ^= data[i];
        s *= 16777619u;
    }
    return s;
}

std::string cache_path(uint64_t hash) {
    const char* no = std::getenv("MCUS_JIT_NO_CACHE");
    if (no && *no) return {};
    const char* ov = std::getenv("MCUS_JIT_CACHE_DIR");
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    std::string dir;
    if (ov && *ov) dir = ov;
    else if (xdg && *xdg) dir = std::string(xdg) + "/mcus/jit";
    else {
        const char* home = std::getenv("HOME");
        if (!home || !*home) return {};
        dir = std::string(home) + "/.cache/mcus/jit";
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llx.mcode",
                  static_cast<unsigned long long>(hash));
    return dir + name;
}

bool load_cache(const std::string& path, NativeProgram& out) {
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    CacheHeader h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || std::memcmp(h.magic, "MJX1", 4) != 0 ||
        h.abi != 4 || h.size == 0 || h.size > (1u << 20))
        return false;
    std::vector<uint8_t> code(h.size);
    f.read(reinterpret_cast<char*>(code.data()), h.size);
    if (!f || checksum(code.data(), code.size()) != h.sum) return false;

    const size_t alloc = ((h.size + kPage - 1) / kPage) * kPage;
    void* mem = ::mmap(nullptr, alloc, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return false;
    std::memcpy(mem, code.data(), code.size());
    if (::mprotect(mem, alloc, PROT_READ | PROT_EXEC) != 0) {
        ::munmap(mem, alloc);
        return false;
    }
    out.code = mem;
    out.size = alloc;
    out.guest_pc = h.guest_pc;
    out.valid = true;
    return true;
}

void save_cache(const std::string& path, const std::vector<uint8_t>& code,
                uint32_t guest_pc) {
    if (path.empty()) return;
    CacheHeader h{};
    h.size = static_cast<uint32_t>(code.size());
    h.guest_pc = guest_pc;
    h.sum = checksum(code.data(), code.size());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.write(reinterpret_cast<const char*>(code.data()), code.size());
}

} // namespace

bool JitEngine::compile(const BcProgram& program, NativeProgram& out) const {
    out = NativeProgram{};
    if (program.code.empty()) return false;

    const std::string cached = cache_path(hash_program(program));
    if (load_cache(cached, out)) return true;

    Emitter e;
    e.index_off.resize(program.code.size(), 0);
    e.prologue();

    const int32_t now_off = static_cast<int32_t>(offsetof(JitContext, now));
    const int32_t user_off = static_cast<int32_t>(offsetof(JitContext, user));
    const int32_t check_off = static_cast<int32_t>(offsetof(JitContext, check));
    const int32_t pc_off = static_cast<int32_t>(offsetof(JitContext, regs) +
                                                offsetof(CpuRegisters, pc));
    const int32_t helper_off = static_cast<int32_t>(offsetof(JitContext, helper));
    const int32_t call_isa_off = static_cast<int32_t>(offsetof(JitContext, call_isa));
    const int32_t iram_data_off = static_cast<int32_t>(offsetof(JitContext, iram_data));
    const int32_t iram_bias_off = static_cast<int32_t>(offsetof(JitContext, iram_bias));
    const int32_t acc_off = static_cast<int32_t>(offsetof(JitContext, acc_ptr));
    const int32_t psw_off = static_cast<int32_t>(offsetof(JitContext, psw_ptr));
    const int32_t sfr_mmio_off = static_cast<int32_t>(offsetof(JitContext, sfr_mmio));
    const int32_t sfr_offsets_off = static_cast<int32_t>(offsetof(JitContext, sfr_offsets));

    for (size_t ip = 0; ip < program.code.size(); ++ip) {
        e.index_off[ip] = static_cast<uint32_t>(e.code.size());
        const BcInsn in = program.code[ip];
        switch (static_cast<BcOp>(in.op)) {
            case BcOp::Nop:
                break;
            case BcOp::AddCycles:
                e.add_now(now_off, static_cast<int32_t>(in.imm));
                break;
            case BcOp::Check:
                e.load_user(user_off);
                e.mov_esi(in.imm);
                e.call_check(check_off);
                e.test_eax();
                e.jnz_placeholder();
                break;
            case BcOp::CheckReg:
                e.load_user(user_off);
                e.load_esi_ctx(pc_off);
                e.call_check(check_off);
                e.test_eax();
                e.jnz_placeholder();
                break;
            case BcOp::SetPc:
                e.store_pc(pc_off, in.imm);
                break;
            case BcOp::CallHelper:
                e.call_helper(helper_off, in.a, in.imm, in.b);
                break;
            case BcOp::MovAImm: {
                e.load_ptr_only(acc_off);
                e.store_imm8(0, static_cast<uint8_t>(in.a));
                e.load_ptr_only(psw_off);
                uint8_t p = 0;
                for (int i = 0; i < 8; ++i) p ^= (in.a >> i) & 1u;
                e.u8(0x41); e.u8(0x80); e.u8(0xA2);
                e.u32(0); e.u8(0xFE);
                if (p) {
                    e.u8(0x41); e.u8(0x80); e.u8(0x8A);
                    e.u32(0); e.u8(0x01);
                }
                break;
            }
            case BcOp::IncA:
                e.load_ptr_only(acc_off);
                e.load_eax8(0);
                e.u8(0x83); e.u8(0xC0); e.u8(0x01);
                e.store_al(0);
                e.load_ptr_only(psw_off);
                e.parity_to_psw(0);
                break;
            case BcOp::DecA:
                e.load_ptr_only(acc_off);
                e.load_eax8(0);
                e.u8(0x83); e.u8(0xE8); e.u8(0x01);
                e.store_al(0);
                e.load_ptr_only(psw_off);
                e.parity_to_psw(0);
                break;
            case BcOp::MovSfrA:
                e.load_ptr_only(acc_off);
                e.load_eax8(0);
                e.u8(0x41); e.u8(0x89); e.u8(0xC4); // mov r12d,eax
                e.load_sfr_addr(sfr_mmio_off, sfr_offsets_off, in.imm);
                e.u8(0x44); e.u8(0x89); e.u8(0xE0); // mov eax,r12d
                e.store_al(0);
                break;
            case BcOp::MovIramA:
                e.load_ptr_only(acc_off);
                e.load_eax8(0);
                e.u8(0x41); e.u8(0x89); e.u8(0xC4); // mov r12d,eax
                e.load_ptr(iram_data_off, iram_bias_off);
                e.u8(0x45); e.u8(0x88); e.u8(0xA2);
                e.u32(static_cast<uint32_t>(in.imm));
                break;
            case BcOp::MovASfr:
                e.load_sfr_addr(sfr_mmio_off, sfr_offsets_off, in.imm);
                e.load_eax8(0);
                e.u8(0x41); e.u8(0x89); e.u8(0xC4); // mov r12d,eax
                e.load_ptr_only(acc_off);
                e.u8(0x44); e.u8(0x89); e.u8(0xE0); // mov eax,r12d
                e.store_al(0);
                e.load_ptr_only(psw_off);
                e.parity_to_psw(0);
                break;
            case BcOp::MovAIram:
                e.load_ptr(iram_data_off, iram_bias_off);
                e.load_eax8(static_cast<int32_t>(in.imm));
                e.u8(0x41); e.u8(0x89); e.u8(0xC4); // mov r12d,eax
                e.load_ptr_only(acc_off);
                e.u8(0x45); e.u8(0x88); e.u8(0xA2);
                e.u32(0);
                e.load_ptr_only(psw_off);
                e.parity_to_psw(0);
                break;
            case BcOp::MovSfrImm:
                e.load_sfr_addr(sfr_mmio_off, sfr_offsets_off, in.imm);
                e.store_imm8(0, static_cast<uint8_t>(in.a));
                break;
            case BcOp::MovIramImm:
                e.load_ptr(iram_data_off, iram_bias_off);
                e.store_imm8(static_cast<int32_t>(in.imm),
                             static_cast<uint8_t>(in.a));
                break;
            case BcOp::AnlAImm:
            case BcOp::OrlAImm:
            case BcOp::XrlAImm: {
                e.load_ptr_only(acc_off);
                e.load_eax8(0);
                if (in.op == static_cast<uint16_t>(BcOp::AnlAImm)) {
                    e.u8(0x25); e.u32(in.a); // and eax, imm32
                } else if (in.op == static_cast<uint16_t>(BcOp::OrlAImm)) {
                    e.u8(0x0D); e.u32(in.a); // or eax, imm32
                } else {
                    e.u8(0x35); e.u32(in.a); // xor eax, imm32
                }
                e.store_al(0);
                e.load_ptr_only(psw_off);
                e.parity_to_psw(0);
                break;
            }
            case BcOp::IncIram:
                e.load_ptr(iram_data_off, iram_bias_off);
                e.u8(0x41); e.u8(0x80); e.u8(0x82);
                e.u32(static_cast<uint32_t>(in.imm)); e.u8(0x01);
                break;
            case BcOp::DecIram:
                e.load_ptr(iram_data_off, iram_bias_off);
                e.u8(0x41); e.u8(0x80); e.u8(0xAA);
                e.u32(static_cast<uint32_t>(in.imm)); e.u8(0x01);
                break;
            case BcOp::CallIsa:
                e.load_user(user_off);
                e.load_esi_ctx(pc_off);
                e.u8(0xFF); e.u8(0x93); e.u32(static_cast<uint32_t>(call_isa_off));
                break;
            case BcOp::Branch:
                if (in.imm >= program.code.size()) return false;
                e.jmp_placeholder(in.imm);
                break;
            case BcOp::BranchIfPc:
                if (in.a >= program.code.size()) return false;
                e.cmp_pc_imm(pc_off, in.imm);
                e.je_placeholder(in.a);
                break;
            case BcOp::End:
                e.jmp_placeholder(UINT32_MAX); // 统一指向出口
                break;
            default:
                return false;
        }
    }

    const uint32_t exit_off = static_cast<uint32_t>(e.code.size());
    e.epilogue();

    for (auto& fx : e.exit_fixups) {
        const int32_t rel = static_cast<int32_t>(exit_off) -
                            static_cast<int32_t>(fx + 4);
        e.patch32(fx, static_cast<uint32_t>(rel));
    }
    for (auto& fx : e.je_fixups) {
        const uint32_t target = e.index_off.at(fx.second);
        const int32_t rel = static_cast<int32_t>(target) -
                            static_cast<int32_t>(fx.first + 4);
        e.patch32(fx.first, static_cast<uint32_t>(rel));
    }
    for (auto& fx : e.label_fixups) {
        const uint32_t target = fx.second == UINT32_MAX
                                    ? exit_off
                                    : e.index_off.at(fx.second);
        const int32_t rel = static_cast<int32_t>(target) -
                            static_cast<int32_t>(fx.first + 4);
        e.patch32(fx.first, static_cast<uint32_t>(rel));
    }

    const size_t size = e.code.size();
    const size_t alloc = ((size + kPage - 1) / kPage) * kPage;
    void* mem = ::mmap(nullptr, alloc, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return false;
    std::memcpy(mem, e.code.data(), size);
    if (::mprotect(mem, alloc, PROT_READ | PROT_EXEC) != 0) {
        ::munmap(mem, alloc);
        return false;
    }

    out.code = mem;
    out.size = alloc;
    out.guest_pc = program.guest_pc;
    out.valid = true;
    save_cache(cached, e.code, program.guest_pc);
    return true;
}

void JitEngine::execute_native(const NativeProgram& program, JitContext& ctx) const {
    if (!program.valid || !program.code) return;
    using Fn = void (*)(JitContext*);
    reinterpret_cast<Fn>(program.code)(&ctx);
}

void JitEngine::release(NativeProgram& program) const {
    if (program.code) ::munmap(program.code, program.size);
    program = NativeProgram{};
}

} // namespace mcus
