# 单独编译 JIT 模块

## 1. 结论

JIT Translator 是一个独立共享库：

```text
build/libmcus_jit_8051.so
```

它和 ISA 插件一样，**不依赖 `libmcus_core.so`**，运行时由 `mcus --jit <lib.so>`
或配置文件 `[simulator] jit = "..."` 加载。

但 JIT 插件和 ISA 插件有一个关键区别：

> JIT 插件和 `libmcus_core.so` 共享 `JitContext` 的 C++ 内存布局，必须使用**同一份
> `include/mcus/jit.h` / `bytecode.h`** 编译，并且 ABI 版本和布局 hash 必须一致。

因此：

- JIT 插件可以独立编译；
- 但**不能**把一个源码版本编译出的 JIT 插件，配到另一个版本的 core 上；
- core 加载时会校验 `mcus_jit_abi_version` 和 `mcus_jit_layout_hash`，不匹配直接报错。

## 2. 只编译 JIT 需要的文件

### 必须的源文件

| 文件 | 作用 |
|---|---|
| `src/isa_jit_8051.cpp` | 8051 Translator：guest 指令 → 字节码/helper，以及 JIT ABI 导出 |
| `src/model.cpp` | `Model` JSON 解析/校验；如果链接预编译 SDK 则不需要 |
| `src/log.cpp` | 日志实现；如果链接预编译 SDK 则不需要 |
| `src/isa_base.cpp` | `InstructionSet` 默认钩子；如果链接预编译 SDK 则不需要 |

### 必须的头文件

```text
include/mcus/jit.h
include/mcus/bytecode.h
include/mcus/instruction.h
include/mcus/model.h
include/mcus/types.h
```

`jit.h` 会包含 `bytecode.h`、`instruction.h`、`model.h`。

### 不需要的文件

JIT 插件编译**不需要**：

```text
src/cpu.cpp
src/jit.cpp / src/jit_x86.cpp
src/shared_memory.cpp
src/event_stream.cpp
src/simulator.cpp
src/main.cpp
libmcus_core.so
```

注意：

- `src/jit.cpp` 和 `src/jit_x86.cpp` 属于 **core 侧 JIT 引擎/宿主后端**，已经编进
  `libmcus_core.so`；
- JIT 插件里只有 Translator 和 helper，不包含 x86-64 代码生成器。

### 第三方依赖

`src/isa_jit_8051.cpp` 本身不直接使用 nlohmann/json；如果同时编译 `src/model.cpp`，
需要 nlohmann/json 头文件：

```text
build/_deps/nlohmann_json-src/include
```

使用预编译 `libmcus_isa_sdk.a` 时通常不需要再手动加这个 include。

## 3. 用 CMake 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mcus_jit_8051 -j
```

产物：

```text
build/libmcus_jit_8051.so
```

对应的 CMake 定义：

```cmake
add_library(mcus_jit_8051 SHARED src/isa_jit_8051.cpp)
target_link_libraries(mcus_jit_8051 PRIVATE mcus_isa_sdk)
target_compile_options(mcus_jit_8051 PRIVATE -Wall -Wextra -Wpedantic)
```

`mcus_isa_sdk` 是静态库，提供 `Model`、日志和 `InstructionSet` 基类默认实现；
JIT 插件不链接 `mcus_core`。

## 3.1 一条命令安装

配置好安装规则后，用一条命令安装主程序、core、插件、SDK 和外部编译所需头文件：

```bash
cmake --install build --prefix "$HOME/.local"
```

安装布局：

```text
$PREFIX/bin/mcus
$PREFIX/bin/mcus_bench
$PREFIX/bin/dump_shm
$PREFIX/lib/libmcus_core.so
$PREFIX/lib/libmcus_isa_8051.so
$PREFIX/lib/libmcus_isa_arm.so
$PREFIX/lib/libmcus_jit_8051.so
$PREFIX/lib/libmcus_isa_sdk.a
$PREFIX/include/mcus/*.h
```

`mcus` 的安装 RPATH 是 `$ORIGIN/../lib`，不需要设置 `LD_LIBRARY_PATH`。
外部编译 ISA/JIT 插件时直接使用：

```text
$PREFIX/include
$PREFIX/lib/libmcus_isa_sdk.a
```

## 4. 手工编译（不经过 CMake）

如果已经构建过项目，可以直接复用 `libmcus_isa_sdk.a`：

```bash
g++ -std=c++17 -O2 -fPIC -shared -Iinclude \
    src/isa_jit_8051.cpp build/libmcus_isa_sdk.a \
    -o libmcus_jit_8051.so
```

如果完全手工编译 SDK：

```bash
g++ -std=c++17 -O2 -fPIC -shared -Iinclude \
    -Ibuild/_deps/nlohmann_json-src/include \
    src/isa_jit_8051.cpp src/model.cpp src/log.cpp src/isa_base.cpp \
    -o libmcus_jit_8051.so
```

`build/_deps/nlohmann_json-src/include` 是 FetchContent 的默认路径，实际以你的构建目录为准。

## 5. 验证是否独立

```bash
ldd libmcus_jit_8051.so
```

输出中不应出现 `libmcus_core.so`，通常只有：

```text
libstdc++.so.6
libgcc_s.so.1
libc.so.6
libm.so.6
```

检查导出符号：

```bash
nm -D libmcus_jit_8051.so | grep mcus_jit_
```

必须包含：

```text
mcus_jit_abi_version
mcus_jit_arch
mcus_jit_create
mcus_jit_destroy
mcus_jit_layout_hash
```

其中：

- `mcus_jit_abi_version()` 当前应返回 `2`；
- `mcus_jit_layout_hash()` 必须和 core 的 `kJitContextLayoutHash` 相同；
- 缺少 `mcus_jit_layout_hash` 说明插件是旧版本，需要重新编译。

## 6. 芯片驱动 / HPM 包中的编译

芯片包目录通常有：

```text
~/.hpm/packages/stc90c51-0.1.0/debug/
  isa_8051.cpp
  stc90c51.json
  debug.toml
  libmcus_isa_8051.so
  libmcus_jit_8051.so
```

JIT 源码 `isa_jit_8051.cpp` 不在芯片包生成流程里时，需要从 **和 core 相同版本的
mcus 源码树**复制进来，然后单独编译：

```bash
cd "$HOME/.hpm/packages/stc90c51-0.1.0/debug"

g++ -std=c++17 -O2 -fPIC -shared \
    -I/path/to/mcus/include \
    isa_jit_8051.cpp \
    /path/to/mcus/build/libmcus_isa_sdk.a \
    -o libmcus_jit_8051.so
```

更推荐的做法是让 HPM 的 mcus 子模块直接构建：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mcus_jit_8051 -j
install -Dm755 build/libmcus_jit_8051.so \
    "$HOME/.hpm/packages/stc90c51-0.1.0/debug/libmcus_jit_8051.so"
```

### 关键：HPM 子模块版本必须一致

HPM 项目通常把 mcus 作为子模块：

```text
hpm/debug/mcus
```

如果这个子模块还是旧的 JIT 提交（例如 `kJitAbiVersion = 1`、没有布局 hash），
而芯片包里的 JIT 插件是新版本，就会加载失败或行为异常。

必须保证：

```text
hpm/debug/mcus/include/mcus/jit.h
hpm/debug/mcus/src/isa_jit_8051.cpp
hpm/debug/mcus/src/jit_x86.cpp
hpm/debug/mcus/src/cpu.cpp
```

来自**同一个 mcus 提交**，并且 HPM 的 core/host/mcus 都用这个子模块重新构建。

不要把另一个 mcus 源码树编译出的 JIT 插件直接复制到旧 core 旁边；ABI 检查会拒绝，
或者在没有检查的旧版本上静默卡死。

## 7. 加载与运行

命令行：

```bash
./build/mcus \
  --isa ./build/libmcus_isa_8051.so \
  --jit ./build/libmcus_jit_8051.so \
  --model models/ch552g.json \
  --firmware firmware.bin \
  --config config/example.toml
```

配置文件：

```toml
[simulator]
jit = "./build/libmcus_jit_8051.so"
```

命令行 `--jit` 优先于配置。

## 8. 常见问题

### `JIT ABI mismatch`

插件和 core 的 `kJitAbiVersion` 不一致。重新编译两者：

```bash
cmake --build build --target mcus_core mcus_jit_8051 -j
```

### `JIT layout mismatch`

插件和 core 使用了不同版本的 `jit.h`/`JitContext`。必须用同一份 mcus 源码重新编译
插件和 core。

### `JIT missing symbol: mcus_jit_layout_hash`

JIT 插件是旧版本（ABI 1 或更早）。更新 `src/isa_jit_8051.cpp` 后重新编译。

### `JIT arch mismatch`

插件 `mcus_jit_arch()` 返回的架构与模型 `arch` 不一致。8051 模型需要
`libmcus_jit_8051.so`。

### 插件加载后 CPU 不推进

先确认：

```bash
ldd /path/to/mcus | grep mcus_core
readelf -d /path/to/mcus | grep RUNPATH
```

确保加载的是和插件同版本的 `libmcus_core.so`，而不是源码目录里的旧 core。

## 9. 相关文档

- [docs/isa_build.md](isa_build.md)：ISA 插件单独编译
- [docs/isa.md](isa.md)：ISA 接口
- [include/mcus/jit.h](../include/mcus/jit.h)：JIT ABI、`JitContext`、布局 hash
- [include/mcus/bytecode.h](../include/mcus/bytecode.h)：字节码格式
- [docs/benchmark.md](benchmark.md)：JIT benchmark 和时序测试
