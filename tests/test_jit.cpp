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
#include "mcus/model.h"

#include <chrono>
#include <cstdio>

namespace {

uint32_t check_ok(void*, uint32_t) { return 0; }

struct StopCounter {
    uint64_t count = 0;
    uint64_t limit = 0;
};

uint32_t check_stop(void* user, uint32_t) {
    auto* c = static_cast<StopCounter*>(user);
    return ++c->count >= c->limit ? 1u : 0u;
}

} // namespace

int main() {
    using namespace mcus;

    Model model = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);

    JitEngine engine;
    if (!engine.load_plugin(MCUS_TEST_JIT8051_PATH, model.arch)) {
        std::puts("jit plugin unavailable");
        return 1;
    }

    // 0x80 0xFE = SJMP $
    const uint8_t code[] = {0x80, 0xFE};
    JitCodeSource src{code, sizeof(code), 0};

    BcProgram program;
    if (!engine.translate(model, src, 0, program)) {
        std::puts("translate failed");
        return 1;
    }
    if (!program.valid || program.guest_len != 2) {
        std::puts("invalid program");
        return 1;
    }
    if (program.code.empty()) {
        std::puts("empty program");
        return 1;
    }

    JitContext ctx;
    ctx.user = nullptr;
    ctx.check = check_ok;
    ctx.helper = engine.helper();
    ctx.max_steps = 1000000;
    ctx.regs.pc = 0;

    const auto t0 = std::chrono::steady_clock::now();
    const bool finished = engine.execute(program, ctx);
    const auto t1 = std::chrono::steady_clock::now();

    if (!finished || ctx.steps != ctx.max_steps || ctx.now != ctx.max_steps) {
        std::puts("unexpected execution result");
        return 1;
    }

    const double ns =
        std::chrono::duration<double, std::nano>(t1 - t0).count() /
        static_cast<double>(ctx.steps);
    std::printf("8051 SJMP bytecode: %.2f ns/step, steps=%llu\n", ns,
                static_cast<unsigned long long>(ctx.steps));

    // 同一段字节码编译成 x86-64，再跑一遍。
    NativeProgram native;
    if (!engine.compile(program, native)) {
        std::puts("native compile failed");
        return 1;
    }
    StopCounter stop;
    stop.limit = 1000000;
    JitContext nctx;
    nctx.user = &stop;
    nctx.check = check_stop;
    nctx.helper = engine.helper();
    const auto nt0 = std::chrono::steady_clock::now();
    engine.execute_native(native, nctx);
    const auto nt1 = std::chrono::steady_clock::now();
    const double nns =
        std::chrono::duration<double, std::nano>(nt1 - nt0).count() /
        static_cast<double>(stop.count);
    std::printf("8051 SJMP native : %.2f ns/step, steps=%llu\n", nns,
                static_cast<unsigned long long>(stop.count));
    if (stop.count != stop.limit || nctx.now != stop.limit) {
        std::puts("native execution mismatch");
        return 1;
    }
    engine.release(native);

    // NOP 单步
    const uint8_t nop[] = {0x00};
    JitCodeSource nop_src{nop, sizeof(nop), 0};
    BcProgram nop_program;
    if (!engine.translate(model, nop_src, 0, nop_program)) {
        std::puts("nop translate failed");
        return 1;
    }
    JitContext nop_ctx;
    nop_ctx.check = check_ok;
    nop_ctx.helper = engine.helper();
    nop_ctx.max_steps = 1;
    if (!engine.execute(nop_program, nop_ctx) || nop_ctx.steps != 1 ||
        nop_ctx.now != 1) {
        std::puts("nop execute failed");
        return 1;
    }

    std::puts("jit ok");
    return 0;
}
