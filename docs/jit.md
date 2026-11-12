# 如何编写 JIT 库

本文说明如何为 `mcus` 编写一个架构 JIT Translator 插件。安装和单独编译步骤见
[docs/jit_build.md](jit_build.md)；本文重点讲接口、字节码和正确性要求。

## 1. 架构

```text
mcus / libmcus_core.so
  ├─ JitEngine             加载插件、翻译缓存、字节码执行
  ├─ x86-64 backend        把 BcProgram 编成宿主机器码
  └─ dlopen(--jit) ───────► libmcus_jit_<arch>.so
                              └─ JitTranslator
                                   guest 指令 → BcProgram/helper
```

职责边界：

| 组件 | 位置 | 职责 |
|---|---|---|
| `JitEngine` | core | 插件加载、ABI 校验、字节码缓存、native 编译/执行 |
| x86-64 backend | core | `BcOp` → 宿主机器码，内存/寄存器快速路径 |
| `JitTranslator` | 插件 | 解码 guest 指令，生成 `BcProgram` 和 helper |
| helper | 插件 | 复杂语义（栈、位操作、SFR、乘除等）的 C++ 实现 |

JIT 插件**不链接 `libmcus_core.so`**，只依赖 `mcus_isa_sdk` 和头文件。但插件和 core
必须使用同一份 `include/mcus/jit.h` / `bytecode.h`，因为 native 代码直接使用
`JitContext` 的字段偏移。

## 2. 必须导出的 ABI

每个 JIT 插件必须导出这些 C 符号：

```cpp
extern "C" uint32_t mcus_jit_abi_version() {
    return mcus::kJitAbiVersion;       // 当前 2
}

extern "C" const char* mcus_jit_arch() {
    return "8051";
}

extern "C" uint32_t mcus_jit_layout_hash() {
    return mcus::kJitContextLayoutHash;
}

extern "C" mcus::JitTranslator* mcus_jit_create(const mcus::Model* model) {
    return new MyTranslator(*model);
}

extern "C" void mcus_jit_destroy(mcus::JitTranslator* t) {
    delete t;
}
```

core 加载时依次校验：

1. `mcus_jit_abi_version` 与 `kJitAbiVersion`；
2. `mcus_jit_arch` 与模型 `arch`；
3. `mcus_jit_layout_hash` 与 `kJitContextLayoutHash`。

任何一项不一致都会拒绝加载，不会进入执行阶段。

## 3. 实现 JitTranslator

接口在 [include/mcus/jit.h](../include/mcus/jit.h)：

```cpp
class JitTranslator {
public:
    virtual ~JitTranslator() = default;

    virtual bool translate(const Model& model,
                           const JitCodeSource& code,
                           Addr pc,
                           BcProgram& out) = 0;

    virtual JitHelperFn helper() const { return nullptr; }
};
```

### 3.1 `JitCodeSource`

```cpp
struct JitCodeSource {
    const uint8_t* code;  // 代码空间基址
    size_t size;
    Addr base;            // code 对应的绝对地址
    bool fetch8(Addr addr, uint8_t& out) const;
};
```

`fetch8` 会检查地址边界。不要直接 `code[pc]`，否则代码空间边界或空洞会越界。

### 3.2 `translate` 契约

成功时：

- 返回 `true`；
- `out.valid = true`；
- `out.guest_pc = pc`；
- `out.guest_len = 指令/块长度`；
- `out.cycles = guest 周期数`；
- `out.code` 至少包含周期推进、检查和结束。

对一条 8051 指令，最小形态通常是：

```text
CallHelper / native op
AddCycles 1
CheckReg 或 SetPc+Check
End
```

如果生成一个基本块，可以连续翻译多条指令，但**每条 guest 指令后都必须有检查和周期推进**，
否则中断/断点/事件流的时间粒度会改变。

### 3.3 回退

无法翻译的指令不要返回 `false` 让整块失败，推荐生成：

```text
CallIsa       // 用 ISA 解释执行当前 guest PC
CheckReg
End
```

并设置 `out.fallback = true`。这样真实固件仍能继续运行。`Cpu::run_jit` 会跳过对
纯 fallback 块的 native 缓存。

### 3.4 helper

`helper()` 返回统一的：

```cpp
uint32_t helper_dispatch(JitContext* ctx, uint32_t id,
                         uint32_t arg0, uint32_t arg1);
```

字节码里的参数布局是：

```text
a   = helper id（8 位）
b   = arg1（8 位）
imm = arg0（32 位）
```

**只有 8 位可以放在 `b` 里。** 16 位返回地址、第二个 16 位操作数等必须打包进 `imm`。
例如 LCALL 的 helper 参数：

```cpp
arg0 = target | (return_addr << 16);
```

不要假设 `b` 能保存 `pc + 3`；这是之前 LCALL 返回地址被截断的原因。

## 4. 字节码

`BcInsn` 固定 8 字节：

```cpp
struct BcInsn {
    uint16_t op;
    uint8_t a;
    uint8_t b;
    uint32_t imm;
};
```

常用 op：

| op | 用途 |
|---|---|
| `AddCycles` | `ctx.now += imm` |
| `Check` / `CheckReg` | 指令边界检查；返回非零退出 native |
| `SetPc` | 设置 guest PC |
| `CallHelper` | 调用插件 helper |
| `CallIsa` | 回退解释执行一条 guest 指令 |
| `Branch` / `BranchIfPc` | 块内静态/动态跳转 |
| `End` | 结束当前块 |
| `MovAImm` / `IncA` / `DecA` | ACC 快速路径 |
| `MovIram*` | iRAM 直接快速路径（无 MMIO 副作用） |
| `MovSfr*` | 仅限 ACC/PSW 等无副作用 SFR；GPIO/UART 等必须走 helper/MMIO |

完整列表和编码见 [include/mcus/bytecode.h](../include/mcus/bytecode.h)。

### 4.1 缓存版本

修改以下内容时必须同时升级版本，否则旧缓存可能被错误复用：

| 位置 | 常量 |
|---|---|
| `include/mcus/bytecode.h` | `kBytecodeVersion` |
| `src/jit_x86.cpp` | `kNativeCacheVersion` |
| `include/mcus/jit.h` | `kJitAbiVersion` / `kJitContextLayoutHash` |

修改 `JitContext` 布局时，布局 hash 会自动变化；仍然建议显式提高 ABI 版本，
让错误信息更明确。

## 5. JitContext

`JitContext` 是插件 helper 和 native 代码共享的 guest 状态：

```cpp
struct JitContext {
    CpuRegisters regs;
    VirtTime now;
    uint64_t steps;
    uint64_t max_steps;
    bool halted;
    uint8_t code_space, iram_space, sfr_space, xram_space;
    uint8_t* iram_data;
    int32_t iram_bias;
    uint8_t* sfr_data;
    int32_t sfr_bias;
    uint8_t* acc_ptr;
    uint8_t* psw_ptr;
    uint8_t* sfr_mmio;
    int32_t sfr_offsets[256];
    uint8_t* xram_data;
    int32_t xram_bias;
    void* user;
    JitReadFn read;
    JitWriteFn write;
    JitCheckFn check;
    JitCallIsaFn call_isa;
    JitRetiFn reti;
    JitHelperFn helper;
};
```

约束：

- **SFR/MMIO 必须走 `read`/`write` 回调或 helper**，不要直接写 `sfr_mmio`。
  直接写会绕过 MMIO 语义、引脚同步和外设事件。
- `iram`/`xram` 的普通内存快速路径可以使用 `iram_data` / `xram_data` 和 bias，
  但必须确认地址落在对应空间内。
- `acc_ptr` / `psw_ptr` 只用于 ACC/PSW 的快速路径；写 ACC 后要维护 P 标志。
- `now` 是虚拟时间，`AddCycles` 后由 core 统一推进外设。
- `check`、`call_isa`、`reti` 由 core 设置，插件不要覆盖。
- `helper` 由 `helper()` 返回，core 在每次 native 块执行前填入。

## 6. 正确性检查清单

写 translator 时逐项核对：

### 控制流

- 相对跳转：`pc + 指令长度 + int8(rel)`；
- `AJMP/ACALL` 的 11 位页内地址；
- `LJMP/LCALL` 的 16 位地址；
- `JMP @A+DPTR`；
- `CJNE` 比较后同时设置 CY 和分支；
- `DJNZ` 减一后按结果分支；
- `RET` / `RETI` 的 pop 顺序。

### 栈

8051 的 `LCALL/ACALL` 必须先压 PC 低字节，再压高字节；`RET` 先弹高字节，再弹低字节。
helper 参数只有 8 位 `b`，返回地址必须打包进 `imm`。

### 标志

- ACC 写后更新 P（奇偶）；
- `ADD/ADDC/SUBB` 更新 CY/AC/OV；
- `MUL/DIV` 更新 CY/OV；
- `DA A`；
- `RLC/RRC` 的 CY；
- `CJNE` 的 CY。

### SFR / GPIO

- `MOV direct,*`、`ORL/ANL/XRL direct,*`、`INC/DEC direct`、`SETB/CLR/CPL bit`
  只要目标是 SFR，就必须通过 MMIO 回调；
- GPIO 写入要触发引脚同步和 `PinEvent`；
- UART/SPI/PWM/ADC/TKEY/USB 的 SFR 写入要触发对应外设状态机。

### 时间

- 每条 guest 指令都要 `AddCycles`；
- 每条 guest 指令后都要 `Check`/`CheckReg`；
- `CallIsa` 回退的周期由 ISA 返回，core 会加到 `ctx.now`；
- native 块内不要跳过检查。

## 7. 测试

新增 translator 至少补以下测试：

1. **`test_jit_diff`**：同一固件解释器 vs JIT，比较 PC、`now`、ACC、PSW、内存；
2. **`test_jit_timing`**：注入中断，比较 Irq 时间、入口 PC、最终 `now`；
3. **`test_jit_event_timing`**：比较 UART 等 `BusEvent` 时间戳；
4. **`test_jit_peripherals_timing`**：比较 GPIO/PWM/SPI/ADC/TKEY/USB 事件和寄存器；
5. **`test_jit_cache_helpers`**：命中 `.mbc` 缓存后 translator/helper 仍可用；
6. **`test_simulator_timer_jit`**：真实 `mcus --jit` 路径的 Timer0 计数。

当前 `test_jit_diff` 覆盖 `basic/alu/cond/stackbit/djnz/movx/sfr/reti/logic/lcall/lcall_loop/cjne_rn_imm`。
新增指令时优先加最小固件场景，不要只测最终内存值；要比较 PSW 和 `now`。

运行：

```bash
ctest --test-dir build --output-on-failure -R jit
```

## 8. 构建

在 `CMakeLists.txt` 中添加：

```cmake
add_library(mcus_jit_myarch SHARED src/isa_jit_myarch.cpp)
target_link_libraries(mcus_jit_myarch PRIVATE mcus_isa_sdk)
target_compile_options(mcus_jit_myarch PRIVATE -Wall -Wextra -Wpedantic)
```

手工编译：

```bash
g++ -std=c++17 -O2 -fPIC -shared -Iinclude \
    src/isa_jit_myarch.cpp build/libmcus_isa_sdk.a \
    -o libmcus_jit_myarch.so
```

安装后的编译方式见 [docs/jit_build.md](jit_build.md)。

## 9. 调试

| 现象 | 检查 |
|---|---|
| `JIT ABI mismatch` | core 和插件来自不同构建 |
| `JIT layout mismatch` | `jit.h`/`JitContext` 不一致 |
| `JIT missing symbol: mcus_jit_layout_hash` | 插件太旧，重编译 |
| native `CallHelper` 跳到 `nullptr` | 命中缓存但 translator 未创建，或 helper 为空 |
| `pc` 不动、`virtual_time=0` | 检查/推进顺序、缓存、入口检查 |
| CPU 在跑但设备不动 | SFR 是否绕过 MMIO、GPIO/位操作、CJNE/LCALL 语义 |
| 设备动一点后停住 | 栈返回地址、LCALL/ACALL 参数截断、栈顺序 |

临时关闭缓存：

```bash
MCUS_JIT_NO_CACHE=1 mcus --jit ...
```

删除缓存：

```bash
rm -rf "${XDG_CACHE_HOME:-$HOME/.cache}/mcus/jit"
```

## 10. 相关文档

- [docs/jit_build.md](jit_build.md)：单独编译、安装、HPM 芯片包
- [docs/benchmark.md](benchmark.md)：JIT benchmark、缓存变量、时序回归
- [include/mcus/jit.h](../include/mcus/jit.h)：`JitTranslator`、`JitContext`、ABI
- [include/mcus/bytecode.h](../include/mcus/bytecode.h)：字节码编码
- [src/isa_jit_8051.cpp](../src/isa_jit_8051.cpp)：8051 参考实现
- [src/jit_x86.cpp](../src/jit_x86.cpp)：x86-64 后端
