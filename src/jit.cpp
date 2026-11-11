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

#include "mcus/jit.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace mcus {
namespace {

template <typename T>
T load_symbol(void* handle, const char* name) {
    void* sym = ::dlsym(handle, name);
    if (!sym) throw std::runtime_error(std::string("JIT missing symbol: ") + name);
    return reinterpret_cast<T>(sym);
}

uint32_t read_mem(const JitContext& ctx, const BcProgram& p, uint8_t space,
                  Addr addr, uint8_t width) {
    if (!ctx.read) return 0;
    if (space >= p.spaces.size()) return 0;
    return ctx.read(ctx.user, space, addr, width);
}

void write_mem(const JitContext& ctx, const BcProgram& p, uint8_t space,
               Addr addr, uint32_t value, uint8_t width) {
    if (!ctx.write || space >= p.spaces.size()) return;
    ctx.write(ctx.user, space, addr, value, width);
}

uint32_t alu_rhs(const JitContext& ctx, const BcInsn& in) {
    return in.b ? ctx.regs.r[in.b & 31] : in.imm;
}

} // namespace


namespace {

uint64_t hash_source(const Model& model, const JitCodeSource& src, Addr pc) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t n) {
        const auto* b = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const uint32_t version = kBytecodeVersion;
    const uint32_t abi = kJitAbiVersion;
    mix(&version, sizeof(version));
    mix(&abi, sizeof(abi));
    mix(&pc, sizeof(pc));
    mix(model.arch.data(), model.arch.size());
    mix(model.name.data(), model.name.size());
    const size_t off = (pc >= src.base) ? static_cast<size_t>(pc - src.base) : src.size;
    const size_t n = off < src.size ? std::min<size_t>(32, src.size - off) : 0;
    if (n) mix(src.code + off, n);
    return h;
}

std::string bc_dir() {
    const char* no = std::getenv("MCUS_JIT_NO_CACHE");
    if (no && *no) return {};
    const char* ov = std::getenv("MCUS_JIT_BC_CACHE_DIR");
    if (ov && *ov) return ov;
    const char* base = std::getenv("MCUS_JIT_CACHE_DIR");
    if (base && *base) return std::string(base) + "/bc";
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return std::string(xdg) + "/mcus/jit/bc";
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    return std::string(home) + "/.cache/mcus/jit/bc";
}

std::string bc_path(uint64_t hash) {
    const std::string dir = bc_dir();
    if (dir.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return {};
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llx.mbc",
                  static_cast<unsigned long long>(hash));
    return dir + name;
}

bool load_bc(const std::string& path, BcProgram& out) {
    if (path.empty()) return false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    BcFileHeader h{};
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || std::memcmp(h.magic, "MBC1", 4) != 0 ||
        h.version != kBytecodeVersion || h.code_count > 4096 ||
        h.const_count > 4096 || h.label_count > 4096 ||
        h.spaces_count > 32)
        return false;
    out = BcProgram{};
    out.version = h.version;
    out.guest_pc = h.guest_pc;
    out.guest_len = h.guest_len;
    out.cycles = h.cycles;
    out.code.resize(h.code_count);
    out.consts.resize(h.const_count);
    out.labels.resize(h.label_count);
    out.spaces.resize(h.spaces_count);
    if (h.code_count)
        f.read(reinterpret_cast<char*>(out.code.data()),
               h.code_count * sizeof(BcInsn));
    if (h.const_count)
        f.read(reinterpret_cast<char*>(out.consts.data()),
               h.const_count * sizeof(uint32_t));
    if (h.label_count)
        f.read(reinterpret_cast<char*>(out.labels.data()),
               h.label_count * sizeof(BcLabel));
    for (auto& sp : out.spaces) {
        uint32_t n = 0;
        f.read(reinterpret_cast<char*>(&n), sizeof(n));
        if (!f || n > 64) return false;
        sp.resize(n);
        if (n) f.read(sp.data(), n);
    }
    uint32_t arch_len = 0;
    f.read(reinterpret_cast<char*>(&arch_len), sizeof(arch_len));
    if (!f || arch_len > 64) return false;
    out.arch.resize(arch_len);
    if (arch_len) f.read(out.arch.data(), arch_len);
    if (!f) return false;
    out.valid = true;
    return true;
}

void save_bc(const std::string& path, const BcProgram& p) {
    if (path.empty()) return;
    BcFileHeader h{};
    std::memcpy(h.magic, "MBC1", 4);
    h.code_count = static_cast<uint32_t>(p.code.size());
    h.const_count = static_cast<uint32_t>(p.consts.size());
    h.label_count = static_cast<uint32_t>(p.labels.size());
    h.guest_pc = p.guest_pc;
    h.guest_len = p.guest_len;
    h.cycles = p.cycles;
    h.spaces_count = static_cast<uint32_t>(p.spaces.size());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    if (!p.code.empty())
        f.write(reinterpret_cast<const char*>(p.code.data()),
                p.code.size() * sizeof(BcInsn));
    if (!p.consts.empty())
        f.write(reinterpret_cast<const char*>(p.consts.data()),
                p.consts.size() * sizeof(uint32_t));
    if (!p.labels.empty())
        f.write(reinterpret_cast<const char*>(p.labels.data()),
                p.labels.size() * sizeof(BcLabel));
    for (const auto& sp : p.spaces) {
        const uint32_t n = static_cast<uint32_t>(sp.size());
        f.write(reinterpret_cast<const char*>(&n), sizeof(n));
        f.write(sp.data(), n);
    }
    const uint32_t arch_len = static_cast<uint32_t>(p.arch.size());
    f.write(reinterpret_cast<const char*>(&arch_len), sizeof(arch_len));
    f.write(p.arch.data(), arch_len);
}

} // namespace

JitEngine::JitEngine() = default;

JitEngine::~JitEngine() { unload(); }

bool JitEngine::load_plugin(const std::string& path, const std::string& arch) {
    unload();
    if (path.empty()) return false;

    void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) throw std::runtime_error("dlopen " + path + ": " + ::dlerror());

    auto abi = load_symbol<uint32_t (*)()>(handle, kJitAbiVersionSymbol);
    auto plugin_arch = load_symbol<const char* (*)()>(handle, kJitArchSymbol);
    auto create = load_symbol<JitCreateFn>(handle, kJitCreateSymbol);
    auto destroy = load_symbol<JitDestroyFn>(handle, kJitDestroySymbol);

    if (abi() != kJitAbiVersion) {
        ::dlclose(handle);
        throw std::runtime_error("JIT ABI mismatch: " + path);
    }
    if (arch != plugin_arch()) {
        ::dlclose(handle);
        throw std::runtime_error("JIT arch mismatch: library=" +
                                 std::string(plugin_arch()) + " model=" + arch);
    }

    handle_ = handle;
    create_ = create;
    destroy_ = destroy;
    arch_ = arch;
    translator_ = nullptr;
    return true;
}

void JitEngine::unload() {
    if (translator_ && destroy_) destroy_(translator_);
    translator_ = nullptr;
    create_ = nullptr;
    destroy_ = nullptr;
    if (handle_) ::dlclose(handle_);
    handle_ = nullptr;
    arch_.clear();
}

bool JitEngine::translate(const Model& model, const JitCodeSource& code, Addr pc,
                          BcProgram& out) const {
    if (!handle_ || !create_) return false;
    const uint64_t hash = hash_source(model, code, pc);
    const std::string cached = bc_path(hash);
    if (load_bc(cached, out)) return true;
    auto* self = const_cast<JitEngine*>(this);
    if (!self->translator_) self->translator_ = create_(&model);
    if (!self->translator_) return false;
    out = BcProgram{};
    out.arch = model.arch;
    if (!self->translator_->translate(model, code, pc, out) || !out.valid)
        return false;
    save_bc(cached, out);
    return true;
}

bool JitEngine::execute(const BcProgram& program, JitContext& ctx) const {
    if (program.code.empty()) return false;
    size_t ip = 0;
    while (ip < program.code.size()) {
        const BcInsn in = program.code[ip++];
        switch (static_cast<BcOp>(in.op)) {
            case BcOp::Nop:
                break;
            case BcOp::LoadImm:
                if ((in.a & 31) < 32) ctx.regs.r[in.a & 31] = in.imm;
                break;
            case BcOp::LoadReg:
            case BcOp::StoreReg:
                if ((in.a & 31) < 32 && (in.b & 31) < 32)
                    ctx.regs.r[in.a & 31] = ctx.regs.r[in.b & 31];
                break;
            case BcOp::LoadMem8:
            case BcOp::LoadMem16:
            case BcOp::LoadMem32: {
                const uint8_t width = static_cast<BcOp>(in.op) == BcOp::LoadMem8
                                          ? 8
                                          : (static_cast<BcOp>(in.op) == BcOp::LoadMem16
                                                 ? 16
                                                 : 32);
                if ((in.a & 31) < 32)
                    ctx.regs.r[in.a & 31] =
                        read_mem(ctx, program, in.b, in.imm, width);
                break;
            }
            case BcOp::StoreMem8:
            case BcOp::StoreMem16:
            case BcOp::StoreMem32: {
                const uint8_t width = static_cast<BcOp>(in.op) == BcOp::StoreMem8
                                          ? 8
                                          : (static_cast<BcOp>(in.op) == BcOp::StoreMem16
                                                 ? 16
                                                 : 32);
                if ((in.b & 31) < 32)
                    write_mem(ctx, program, in.a, in.imm, ctx.regs.r[in.b & 31],
                              width);
                break;
            }
            case BcOp::Add:
                ctx.regs.r[in.a & 31] += alu_rhs(ctx, in);
                break;
            case BcOp::Sub:
                ctx.regs.r[in.a & 31] -= alu_rhs(ctx, in);
                break;
            case BcOp::And:
                ctx.regs.r[in.a & 31] &= alu_rhs(ctx, in);
                break;
            case BcOp::Or:
                ctx.regs.r[in.a & 31] |= alu_rhs(ctx, in);
                break;
            case BcOp::Xor:
                ctx.regs.r[in.a & 31] ^= alu_rhs(ctx, in);
                break;
            case BcOp::Shl:
                ctx.regs.r[in.a & 31] <<= (in.imm & 31);
                break;
            case BcOp::Shr:
                ctx.regs.r[in.a & 31] >>= (in.imm & 31);
                break;
            case BcOp::Cmp:
                ctx.regs.flags = (ctx.regs.r[in.a & 31] == alu_rhs(ctx, in))
                                     ? 1u
                                     : 0u;
                break;
            case BcOp::SetFlags:
                ctx.regs.flags = in.imm;
                break;
            case BcOp::Branch:
                if (in.imm >= program.code.size()) return false;
                ip = in.imm;
                break;
            case BcOp::BranchIfPc:
                if (ctx.regs.pc == in.imm) {
                    if (in.a >= program.code.size()) return false;
                    ip = in.a;
                }
                break;
            case BcOp::CondBranch: {
                bool take = false;
                switch (static_cast<BcCond>(in.a)) {
                    case BcCond::Always: take = true; break;
                    case BcCond::Zero: take = (ctx.regs.flags & 1u) != 0; break;
                    case BcCond::NotZero: take = (ctx.regs.flags & 1u) == 0; break;
                    case BcCond::Carry: take = (ctx.regs.flags & 2u) != 0; break;
                    case BcCond::NotCarry: take = (ctx.regs.flags & 2u) == 0; break;
                    case BcCond::Negative: take = (ctx.regs.flags & 4u) != 0; break;
                    case BcCond::Positive: take = (ctx.regs.flags & 4u) == 0; break;
                }
                if (take) {
                    if (in.imm >= program.code.size()) return false;
                    ip = in.imm;
                }
                break;
            }
            case BcOp::CallHelper:
                if (!ctx.helper) return false;
                ctx.helper(&ctx, in.a, in.imm, in.b);
                break;
            case BcOp::CallIsa:
                if (!ctx.call_isa || !ctx.call_isa(ctx.user, program.guest_pc))
                    return false;
                break;
            case BcOp::AddCycles:
                ctx.now += in.imm;
                break;
            case BcOp::Check:
                if (ctx.check && ctx.check(ctx.user, program.guest_pc) != 0)
                    return false;
                ++ctx.steps;
                if (ctx.max_steps && ctx.steps >= ctx.max_steps) return true;
                break;
            case BcOp::SetPc:
                ctx.regs.pc = in.imm;
                break;
            case BcOp::CheckReg:
                if (ctx.check && ctx.check(ctx.user, ctx.regs.pc) != 0)
                    return false;
                ++ctx.steps;
                if (ctx.max_steps && ctx.steps >= ctx.max_steps) return true;
                break;
            case BcOp::Trap:
                return false;
            case BcOp::End:
                return true;
            default:
                return false;
        }
    }
    return true;
}

} // namespace mcus
