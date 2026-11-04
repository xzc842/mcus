# 如何编写 ISA

`mcus` 的 ISA 是插件化的。核心不写死具体架构，按 `models/*.json` 的 `arch` 字段加载对应 ISA。本文说明怎么新增一个 ISA。

## 1. 接口

ISA 实现 `include/mcus/instruction.h` 的 `InstructionSet`：

```cpp
class InstructionSet {
public:
    virtual ~InstructionSet() = default;
    virtual ExecResult execute(CpuRegisters& regs, MemoryBus& bus) = 0;
};
```

- **`CpuRegisters`**：通用寄存器、`pc`、`sp`、`flags`。8051 的 `sp` 不用，`sp` 在 SFR 0x81，由 ISA 自己读写。
- **`MemoryBus`**：按空间读写 8/16/32 位，`read8("code", addr)`、`write8("sfr", addr, v)`、`on_reti()`。
- **`ExecResult`**：`cycles`（消耗虚拟时间）、`halted`（是否停机）。

## 2. 注册

ISA 以动态库（`.so`）形式提供，不编进 `mcus` 主程序。库必须导出四个 C ABI 符号：

```cpp
// src/isa_arm.cpp
extern "C" uint32_t mcus_isa_abi_version() {
    return mcus::kIsaAbiVersion;
}

extern "C" const char* mcus_isa_arch() {
    return "arm";
}

extern "C" mcus::InstructionSet* mcus_isa_create(const mcus::Model* model) {
    return new mcus::IsaArm(*model);   // 必须用 new 分配
}

extern "C" void mcus_isa_destroy(mcus::InstructionSet* isa) {
    delete isa;
}
```

符号名定义在 `include/mcus/isa_plugin.h`：

| 符号 | 作用 |
|---|---|
| `mcus_isa_abi_version` | 返回 ABI 版本，必须等于 `kIsaAbiVersion` |
| `mcus_isa_arch` | 返回架构名，必须与模型 `arch` 一致 |
| `mcus_isa_create` | 按模型创建 `InstructionSet` 实例 |
| `mcus_isa_destroy` | 销毁实例 |

启动时由 `mcus --isa <lib.so>` 加载并注册到 `IsaRegistry`：

```bash
mcus --isa ./build/libmcus_isa_8051.so --model ... --firmware ... --config ...
```

CMake 侧参考 `CMakeLists.txt`：

```cmake
add_library(mcus_isa_arm SHARED src/isa_arm.cpp)
target_link_libraries(mcus_isa_arm PRIVATE mcus_core)
```

`mcus` 会校验 ABI 版本和 `arch`，不一致直接报错退出。

## 3. 模型文件

`models/<chip>.json` 的 `arch` 字段决定加载哪个 ISA：

```json
{
  "name": "my_arm_chip",
  "arch": "arm",
  ...
}
```

**`arch` 必须是 `IsaRegistry` 注册过的名字**，否则 `IsaRegistry::create` 返回 `nullptr`，`Cpu` 构造抛 `no ISA for arch`。

## 4. 架构级描述 `arch_desc`

ISA 不硬编码芯片差异，从 `model.arch_desc` 读：

```json
"arch_desc": {
  "global_irq_enable": { "space": "sfr", "reg": 168, "bit": 7, "present": true },
  "irq_push_bytes": 2,
  "fast_spaces": ["code", "iram", "xram"],
  "reset_space": "code",
  "reset_vector": 0,
  "sp_space": "iram",
  "initial_sp": 7,
  "sp_reg": 129,
  "sp_reg_space": "sfr",
  "has_pc_reg": false
}
```

- **`global_irq_enable`**：全局中断使能（8051 是 `IE` bit7，ARM 是 `PRIMASK`，RISC-V 是 `mstatus.MIE`）；
- **`irq_push_bytes`**：中断压栈宽度（8051 压 2 字节 PC，ARM 压 4/8 字节）；
- **`fast_spaces`**：快速空间（`code`/`iram`/`xram`，别的架构 `flash`/`ram`）；
- **`reset_space`/`reset_vector`**：复位向量；
- **`sp_space`/`initial_sp`/`sp_reg`/`sp_reg_space`**：栈空间和 SP 寄存器；
- **`has_pc_reg`/`pc_reg_*`**：PC 外部寄存器（8051 没有，别的架构有）。

**ISA 实现从 `model.arch_desc` 读，不硬编码。**

## 5. 指令集实现

### 5.1 取指

`execute` 开头：

```cpp
ExecResult execute(CpuRegisters& regs, MemoryBus& bus) override {
    ExecResult res;
    res.cycles = model_.cycles_per_instruction;

    Addr pc = regs.pc;
    uint8_t op = fetch(bus, pc);

    switch (op) {
        case 0x00: pc += 1; break;
        ...
    }

    regs.pc = pc;
    return res;
}
```

**`pc` 始终指向 opcode，`fetch(bus, addr)` 是 `bus.read8("code", addr)`。** **`execute` 末尾 `regs.pc = pc`。**

### 5.2 内存访问

- **`fetch(bus, addr)`**：取指，`bus.read8("code", addr)`；
- **`direct_read(bus, addr)`**：8051 的 `direct`，`addr < 0x80` 读 `iram`，`>= 0x80` 读 `sfr`；
- **`direct_write(bus, addr, v)`**：同上；
- **`reg_read`/`reg_write`**：8051 的 `Rn`，`iram` 的 `reg_bank_base + n`；
- **`bit_read`/`bit_write`**：8051 的位地址，`model_.resolve_bit_addr` 解析。

**别的架构按自己的内存模型实现，`MemoryBus` 提供 `read8`/`read16`/`read32`/`write8`/`write16`/`write32`。**

### 5.3 标志位

8051 的 `CY`/`AC`/`OV`/`P` 在 `PSW`（`sfr` 0xD0），`write_acc` 更新 `P`，`add_a`/`subb_a` 更新 `CY`/`AC`/`OV`。

**别的架构按自己的 flags 寄存器实现，`CpuRegisters::flags` 是通用 flags，ISA 自己维护。**

### 5.4 中断

`Cpu` 负责选中优先级最高的中断、清挂起位和设 PC；**保存/恢复上下文由 ISA 钩子负责**：

| 钩子 | 何时调用 | 默认行为 |
|---|---|---|
| `save_context(regs, bus, model)` | 进入中断前 | 按 `arch_desc.irq_push_bytes` 压 PC |
| `restore_context(regs, bus, model)` | `bus.on_reti()` 时 | 空 |
| `higher_priority(a, b)` | 选中断时 | `priority` 小者优先 |

8051 在 `save_context` 里保存 `IP`（`sfr` 0xB8）并按 `SP` 压 PC，`restore_context` 恢复 `IP`：

```cpp
void save_context(CpuRegisters& regs, MemoryBus& bus, const Model& model) override {
    ip_stack_.push_back(bus.read8("sfr", 0xB8));
    uint8_t sp = bus.read8("sfr", 0x81);
    for (uint8_t i = 0; i < model.arch_desc.irq_push_bytes; ++i) {
        bus.write8("iram", ++sp, (regs.pc >> (8 * i)) & 0xFF);
    }
    bus.write8("sfr", 0x81, sp);
}
```

**8051 的 `RETI`（`0x32`）：** 弹 PC 后调用 `bus.on_reti()`，触发 `restore_context`。

### 5.5 停机

`res.halted = true` 时，`Cpu::step` 置 `halted_`，`cpu_loop` 退出。

**8051 的未实现 opcode：**

```cpp
default:
    MCUS_WARN("8051 opcode unimplemented: 0x" + std::to_string(op));
    res.halted = true;
    break;
```

### 5.6 架构钩子一览

`InstructionSet` 里除 `execute` 外的虚函数都是可覆盖的钩子：

```cpp
virtual void reset(CpuRegisters&, MemoryBus&, const Model&);
virtual void save_context(...);
virtual void restore_context(...);
virtual bool resolve_bit_addr(const Model&, Addr, SpaceId&, Addr&, uint8_t&) const;
virtual void on_mmio_write(...);
virtual void on_mmio_read(...);
virtual bool higher_priority(const InterruptDesc&, const InterruptDesc&) const;
```

- `reset`：设置 PC/SP/架构寄存器。ARM 可在里面读向量表拿初始 SP/PC。
- `resolve_bit_addr`：8051 的位地址规则放在这里，核心不再写死。
- `on_mmio_write`/`on_mmio_read`：架构相关副作用；简单的“写 A 置 B 位”优先用模型的 `on_write` 动作。
- `CpuRegisters::r` 已扩到 32 个，覆盖 ARM（16）和 RISC-V（32）。

## 6. ISA 扩展

芯片特有指令用 `model.isa_extensions` 开关：

```json
"isa_extensions": ["wch_dptr1_movx"]
```

```cpp
bool has_ext(const std::string& name) const {
    return extensions_.count(name) > 0;
}

case 0xA5: {
    if (!has_ext("wch_dptr1_movx")) {
        res.halted = true;
        break;
    }
    // WCH 0xA5: MOVX @DPTR1,A
    ...
}
```

**别的芯片扩展（STC、Nuvoton、Silabs）用 `has_ext("stc_xxx")`/`has_ext("nuvoton_xxx")`。**

## 7. 测试

### 7.1 单元测试

`tests/test_isa_<arch>.cpp`：

```cpp
TEST(IsaArm, Add) {
    Model m = Model::load_from_file("models/my_arm_chip.json");
    SharedMemory shm(m, "/mcus_test_arm");
    EventStream stream("/mcus_test_arm_stream", 16);
    Cpu cpu(m, shm, stream);
    cpu.load_firmware(firmware);
    cpu.step();
    EXPECT_EQ(cpu.regs().r[0], 3);
}
```

### 7.2 固件测试

写 `.c`，`sdcc`/`arm-none-eabi-gcc` 编译，`mcus` 跑，`dump_shm` 看寄存器。

**8051 的 `blink.bin`、`xram_test`、`movc_test`、`arith_test`、`bit_test`、`timer_test` 是参考。**

## 8. 示例：最小 ISA（`mcus-minimal`）

`src/isa_registry.cpp` 的 `MinimalIsa` 是参考：

- **编码**：`0x00 NOP`、`0x01 space_id addr32 value32`（写）、`0x02 space_id addr32`（读）、`0x03 target32`（跳转）、`0x04`（停机）、`0x07 space_id addr32 value32`（写 32 位）、`0x08 reg imm32`（加载立即数）；
- **`spaces_`**：按 `model.regions` 顺序去重，`space_of(id)` 返回空间名；
- **`execute`**：`switch (op)`，`fetch8`/`fetch32`，`regs.r[0]`/`regs.flags`。

**`dummy.json` 的 `arch: "mcus-minimal"`，`test_cpu_minimal` 验证。**

## 9. 检查清单

新增 ISA 前：

- [ ] `src/isa_<arch>.cpp` 实现 `InstructionSet`；
- [ ] 导出 `mcus_isa_arch` / `mcus_isa_create` / `mcus_isa_destroy` 三个 C ABI 符号；
- [ ] CMake 中把 ISA 编译成 `SHARED` 库并链接 `mcus_core`；
- [ ] `models/<chip>.json` 的 `arch` 是 `<arch>`；
- [ ] `models/<chip>.json` 的 `arch_desc` 填 `global_irq_enable`/`irq_push_bytes`/`fast_spaces`/`reset_space`/`sp_space` 等；
- [ ] `Cpu`/`Simulator` 不改，所有芯片差异在 `arch_desc`；
- [ ] `tests/test_isa_<arch>.cpp` 加单元测试；
- [ ] 写固件，`mcus` 跑，`dump_shm` 验证。

## 10. 常见错误

- **`arch` 没注册**：`IsaRegistry::create` 返回 `nullptr`，`Cpu` 构造抛 `no ISA for arch`；
- **`pc` 没更新**：`execute` 末尾 `regs.pc = pc` 漏了，`Cpu::step` 的 `pc` 不变；
- **`cycles` 为 0**：`step` 返回 `model_.cycles_per_instruction`，`virtual_time` 推进；
- **`halted` 没置**：未实现 opcode 不 `res.halted = true`，`cpu_loop` 死循环；
- **`sp` 硬编码**：8051 的 `sp` 在 `sfr` 0x81，别的架构 `sp_reg`/`sp_reg_space` 从 `arch_desc` 读；
- **`code`/`iram`/`xram` 硬编码**：`fast_spaces` 从 `arch_desc` 读，别的架构 `flash`/`ram`。

## 11. 参考

- `src/isa_8051.cpp`：8051 完整指令集；
- `src/isa_registry.cpp` 的 `MinimalIsa`：最小 ISA；
- `docs/external_contract.md`：共享内存和事件流契约；
- `models/schema.md`：模型字段说明。
