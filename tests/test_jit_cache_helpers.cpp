// Copyright 2026 XueZicheng <xuezicheng842@outlook.com>
// SPDX-License-Identifier: Apache-2.0
//
// 回归：字节码缓存命中时也必须创建 translator，否则 CallHelper 的
// ctx.helper 为空，native 代码会跳到 nullptr。

#include "mcus/jit.h"
#include "mcus/model.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>
#include <vector>

using namespace mcus;

static bool translate_once(const Model& model,
                           const std::vector<uint8_t>& fw) {
    JitEngine engine;
    assert(engine.load_plugin(MCUS_TEST_JIT8051_PATH, "8051"));
    JitCodeSource src{fw.data(), fw.size(), 0};
    BcProgram p;
    const bool ok = engine.translate(model, src, 0, p);
    assert(ok);
    assert(engine.helper() != nullptr);
    return ok;
}

int main() {
    const std::string dir =
        "/tmp/mcus_jit_cache_helper_" + std::to_string(::getpid());
    std::filesystem::remove_all(dir);
    ::setenv("MCUS_JIT_BC_CACHE_DIR", dir.c_str(), 1);
    ::unsetenv("MCUS_JIT_NO_CACHE");

    const Model model = Model::load_from_file(MCUS_TEST_CH552G_MODEL_PATH);
    // MOV P1,#1; SJMP $
    const std::vector<uint8_t> fw = {0x75, 0x90, 0x01, 0x80, 0xFE};

    // 第一次生成 .mbc，第二次命中磁盘缓存。
    assert(translate_once(model, fw));
    assert(translate_once(model, fw));
    assert(!std::filesystem::is_empty(dir));

    std::filesystem::remove_all(dir);
    return 0;
}
