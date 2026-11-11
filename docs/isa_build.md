# 单独编译 ISA

## 1. 结论

可以。每个 ISA 都是一个独立的共享库：

```text
build/libmcus_isa_8051.so
build/libmcus_isa_arm.so
```

它们只依赖 C++ 运行库和静态库 `build/libmcus_isa_sdk.a`，**不依赖 `libmcus_core.so`**，
也不依赖 `mcus`/`Simulator` 的可执行文件。运行时由 `mcus --isa <lib.so>` 动态加载。

ISA 不是独立可执行程序：它需要由 `mcus` 加载，并配合一个 `arch` 匹配的模型文件使用。

## 2. 只编译 ISA 需要的文件

### 必须的源文件

| 文件 | 作用 |
|---|---|
| `src/isa_8051.cpp` 或 `src/isa_arm.cpp` | 具体架构的 `InstructionSet` 实现和 ABI 导出 |
| `src/model.cpp` | `Model` 的 JSON 解析、校验和查询（ISA 读取 `arch_desc`/寄存器） |
| `src/log.cpp` | `MCUS_WARN` 等日志实现 |
| `src/isa_base.cpp` | `InstructionSet::reset/save_context/...` 默认钩子 |

### 必须的头文件

```text
include/mcus/instruction.h
include/mcus/isa_plugin.h
include/mcus/log.h
include/mcus/model.h
include/mcus/types.h
```

`instruction.h` 会包含 `types.h` 和 `model.h`，ISA 源文件通常只需：

```cpp
#include "mcus/instruction.h"
#include "mcus/isa_plugin.h"
#include "mcus/log.h"
```

### 第三方依赖

只有 `src/model.cpp` 需要 **nlohmann/json 头文件**。CMake 通过 FetchContent 提供；
手工编译时需要 `-I<...>/nlohmann_json/include`。日志使用标准库文件流和 mutex，没有额外依赖。

### 不需要的文件

ISA 编译不需要：

```text
src/cpu.cpp
src/shared_memory.cpp
src/event_stream.cpp
src/simulator.cpp
src/config.cpp
src/firmware.cpp
src/isa_registry.cpp
src/main.cpp
tests/*
```

也不需要链接 `libmcus_core.so`。

### 使用预编译 SDK 时

如果已经构建过项目，可以只保留：

```text
src/isa_8051.cpp        # 或 src/isa_arm.cpp
include/mcus/*.h
build/libmcus_isa_sdk.a
```

然后按第 6 节的手工命令链接 SDK。

## 3. 只构建 ISA

不需要构建主程序：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mcus_isa_8051 -j
cmake --build build --target mcus_isa_arm -j
```

产物：

```text
build/libmcus_isa_8051.so
build/libmcus_isa_arm.so
build/libmcus_isa_sdk.a
```

`mcus_isa_sdk` 是 ISA SDK 静态库，包含：

- `src/model.cpp`：模型 JSON 解析/校验；
- `src/log.cpp`：`MCUS_WARN` 等日志；
- `src/isa_base.cpp`：`InstructionSet` 默认钩子。

ISA 源码只包含：

```cpp
#include "mcus/instruction.h"   // InstructionSet/MemoryBus/CpuRegisters/Model
#include "mcus/isa_plugin.h"    // ABI 导出符号
#include "mcus/log.h"           // MCUS_WARN
```

不再需要包含 `mcus/cpu.h`、`shared_memory.h` 或 `event_stream.h`。

## 4. 验证是否真的独立

```bash
ldd build/libmcus_isa_8051.so
```

输出中不应出现 `libmcus_core.so`，只会有 `libstdc++`、`libgcc_s`、`libc` 等系统库。
可以进一步检查导出符号：

```bash
nm -D build/libmcus_isa_8051.so | grep mcus_isa_
```

应能看到：

```text
mcus_isa_abi_version
mcus_isa_arch
mcus_isa_create
mcus_isa_destroy
```

## 5. 在 CMake 中新增 ISA

```cmake
add_library(my_isa SHARED src/isa_myarch.cpp)
target_link_libraries(my_isa PRIVATE mcus_isa_sdk)
target_compile_options(my_isa PRIVATE -Wall -Wextra -Wpedantic)
```

ISA 必须导出四个 C ABI 符号：

```cpp
extern "C" uint32_t mcus_isa_abi_version() {
    return mcus::kIsaAbiVersion;
}

extern "C" const char* mcus_isa_arch() {
    return "myarch";
}

extern "C" mcus::InstructionSet* mcus_isa_create(const mcus::Model* model) {
    return new mcus::MyIsa(*model);
}

extern "C" void mcus_isa_destroy(mcus::InstructionSet* isa) {
    delete isa;
}
```

ABI 版本和 `arch` 由 `mcus` 在加载时校验；不匹配会直接报错，不会进入执行阶段。

## 6. 手工编译（不经过 CMake）

通常不推荐，因为 `model.cpp` 需要 nlohmann/json 头文件。CMake 构建后可以直接复用
已编译的 SDK：

```bash
g++ -std=c++17 -O2 -fPIC -shared -Iinclude \
    src/isa_8051.cpp build/libmcus_isa_sdk.a \
    -o libmcus_isa_8051.so
```

如果需要完全手工编译 SDK：

```bash
g++ -std=c++17 -O2 -fPIC -shared -Iinclude \
    -Ibuild/_deps/nlohmann_json-src/include \
    src/isa_8051.cpp src/model.cpp src/log.cpp src/isa_base.cpp \
    -o libmcus_isa_8051.so
```

`build/_deps/nlohmann_json-src/include` 是 FetchContent 的默认路径，实际路径以你的
构建目录为准。

## 7. 加载运行

```bash
./build/mcus \
  --isa ./build/libmcus_isa_8051.so \
  --model models/ch552g.json \
  --firmware firmware.hex \
  --config config/example.toml
```

ISA 的 `arch` 必须与模型一致，例如：

- `models/ch552g.json` 的 `arch: "8051"` → `libmcus_isa_8051.so`
- `models/stm32f103c8t6.json` 的 `arch: "arm"` → `libmcus_isa_arm.so`
- `models/dummy.json` 的 `arch: "mcus-minimal"` → 内置 ISA，不需要 `--isa`

## 8. 常见问题

### `no ISA for arch: xxx`

没有通过 `--isa` 加载对应插件，或插件的 `mcus_isa_arch()` 返回值与模型 `arch` 不一致。

### `undefined symbol`

说明 ISA 没有链接 `mcus_isa_sdk`。检查：

```cmake
target_link_libraries(my_isa PRIVATE mcus_isa_sdk)
```

### `cannot open shared object file: libmcus_core.so`

这是旧版 ISA 的构建方式：插件直接链接了 `mcus_core`。当前应改为链接 `mcus_isa_sdk`，
然后重新构建插件。

### `ISA ABI mismatch`

`kIsaAbiVersion` 与插件不一致。重新编译插件：

```bash
cmake --build build --target mcus_isa_8051 mcus_isa_arm -j
```

## 9. 相关文档

- [docs/isa.md](isa.md)：`InstructionSet` 接口、模型钩子、指令实现和测试；
- [include/mcus/isa_plugin.h](../include/mcus/isa_plugin.h)：ABI 符号和版本；
- [include/mcus/instruction.h](../include/mcus/instruction.h)：`MemoryBus`、`InstructionSet`、`CpuRegisters`。
