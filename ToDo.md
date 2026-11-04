# mcus 项目文档

## 1. 项目定位

`mcus` 是一个**通用单片机模拟器**。它不模拟芯片内部的门级或电气行为，而是提供一个“时间对齐的指令执行环境 + 寄存器语义层”，让为单片机编译的固件以为自己运行在真实芯片上。

核心设计原则：

- **虚拟时间单一来源**：所有组件按虚拟晶振周期推进，不跟真实时间对齐。
- **芯片差异在模型文件**：地址空间、寄存器语义、定时器结构、引脚复用、中断向量全部由 `models/*.json` 描述，核心不写死具体芯片。
- **指令集可插拔**：按模型文件的 `arch` 字段加载 ISA 模块，当前有 `mcus-minimal` 和 `8051` 两个 ISA。
- **外部程序不在本项目内**：外设模型通过共享内存和事件流与模拟器交互，只碰状态，不调用模拟器内部函数。
- **流满背压暂停**：事件流满时 CPU 线程暂停推进，避免丢事件导致行为不一致。
- **复位由 RST 引脚触发**：外部程序写 `reset_pin`，模拟器在指令边界检测并执行复位序列。

## 2. 目录结构

```
mcus/
├── CMakeLists.txt
├── README.md
├── config/
│   ├── example.toml
│   └── debug.toml
├── models/
│   ├── dummy.json
│   ├── ch552g.json
│   └── schema.md
├── include/mcus/
│   ├── types.h
│   ├── model.h
│   ├── shared_memory.h
│   ├── event_stream.h
│   ├── cpu.h
│   ├── simulator.h
│   ├── config.h
│   ├── instruction.h
│   └── log.h
├── src/
│   ├── model.cpp
│   ├── shared_memory.cpp
│   ├── event_stream.cpp
│   ├── cpu.cpp
│   ├── simulator.cpp
│   ├── config.cpp
│   ├── isa_registry.cpp
│   ├── isa_8051.cpp
│   └── log.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── test_event_stream.cpp
│   ├── test_shared_memory.cpp
│   ├── test_cpu_minimal.cpp
│   ├── test_timer.cpp
│   └── test_ch552g.cpp
└── tools/
    └── dump_shm.cpp
```

## 3. 构建与运行

### 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

### 运行

```bash
./build/mcus --isa ./build/libmcus_isa_8051.so \
  --model <model.json> --firmware <firmware.hex|firmware.bin> \
  --config <config.toml>
```

四个启动参数：

- **ISA 库**：架构指令集动态库，如 `libmcus_isa_8051.so`。
- **模型文件**：描述芯片静态结构。
- **固件**：`.bin` 或 `.hex`/`.ihx`。
- **配置文件**：共享内存名、事件流容量、晶振频率、日志级别等。

### 调试工具

```bash
./build/dump_shm <model.json> <shm_name>
```

打印控制区、MMIO 寄存器、引脚表、中断位图。

## 4. 配置与模型文件

### `config/example.toml`

常规运行配置。关键字段：

- `simulator.shm_name`：共享内存名，默认 `/mcus_shm`。
- `simulator.stream_name`：事件流名，默认 `/mcus_stream`。
- `simulator.stream_capacity`：事件流容量，默认 4096。
- `simulator.start_paused`：启动后是否立即暂停。
- `crystal.frequency_hz`：虚拟晶振频率。
- `crystal.cycles_per_instruction`：每条指令粗略虚拟耗时。
- `debug.log_level`：`trace`/`debug`/`info`/`warn`/`error`。
- `debug.log_output`：`stderr` 或文件路径。

### `config/debug.toml`

调试用配置，启动即暂停、缩小事件流容量、开详细日志。

### `models/schema.md`

模型文件字段说明：`regions`、`registers`、`pins`、`interrupts`、`timers`、`isa_extensions`、`reset_vector`、`initial_sp` 等。

### `models/dummy.json`

最小可运行模型：一段 Flash、一段 RAM、一段 MMIO、一个可写触发事件的寄存器、一个引脚、一个中断、一个定时器。用于验证框架闭环和测试。

### `models/ch552g.json`

WCH CH552G 模型：

- 地址空间：`code`（16KB）、`iram`（256B）、`sfr`（128B）、`xram`（1KB，`addr_mask=0x3FF`）。
- 寄存器：P1、P3、PSW、ACC、B、SP、DPL、DPH、TCON、TMOD、TL0/TH0、TL1/TH1、SCON、SBUF、T2CON、T2MOD、RCAP2L/H、TL2/TH2、PWM、SPI、UART1、ADC、TKEY、USB 等。
- 同地址多语义：`SAFE_MOD`/`CHIP_ID` 通过 `read_alias`/`write_alias` 互指；`ROM_CTRL`/`ROM_STATUS` 同理。
- 中断：14 个中断源，向量地址与手册一致。
- 定时器：Timer0/1 为 16 位，`overflow_action: "isa"`；Timer2 为 16 位自动重载，`overflow_action: "reload"`。
- 引脚：P1.0–P1.7、P3.0–P3.7、RST，`source_reg` 指向对应端口 SFR，用于寄存器位到引脚表的同步。
- ISA 扩展：`wch_dptr1_movx`，启用 0xA5 扩展指令。

## 5. 头文件

### `include/mcus/types.h`

基础类型：`VirtTime`（虚拟时间，晶振周期数）、`Addr`（32 位地址）、`SpaceId`（地址空间名）、`MmioEvent`（事件流记录，纯 POD，不含 `std::string`）、`RegSemantics`、`ControlCommand`、`ControlStatus`。

### `include/mcus/model.h`

模型结构：

- `MemoryRegion`：空间名、基址、大小、是否 MMIO、`addr_mask`。
- `RegisterDesc`：寄存器名、空间、地址、宽度、语义、复位值、`read_alias`/`write_alias`、位别名。
- `PinDesc`：引脚名、编号、复用功能、复位功能、`source_space`/`source_reg`/`source_bit`。
- `InterruptDesc`：中断号、向量地址、使能/挂起寄存器与位。
- `CounterDesc`/`TimerDesc`：计数器结构、使能/溢出位、中断号、溢出行为。
- `Model`：聚合以上，提供 `load_from_file`、`validate`、`find_region`、`find_register`、`find_register_by_name`、`find_interrupt`、`resolve_bit_addr`、`space_index`。

### `include/mcus/shared_memory.h`

共享内存布局与控制结构：

- `ControlBlock`：命令、状态、单步计数、dump 地址/长度/就绪、复位引脚、虚拟时间、PC、停机标志。
- `PinState`：功能、值、方向。
- `InjectSlot`/`InjectHeader`：注入区。
- `SharedMemory`：POSIX 共享内存映射，提供 `control`、`pins`、`interrupt_bitmap`、`inject_header`、`inject_slots`、`read_reg`、`write_reg`、`reset_all`、`dump_buffer`、`export_mmio`。

### `include/mcus/event_stream.h`

环形队列 + 背压标志。`push` 满时返回 false 并置背压，`pop` 空时返回 false。外部程序通过同一个共享内存对象消费事件。

### `include/mcus/instruction.h`

指令集接口与注册表：

- `ExecResult`：消耗虚拟时间、是否停机。
- `MemoryBus`：按空间读写 8/16/32 位，`on_reti` 默认空实现。
- `InstructionSet`：`execute(CpuRegisters&, MemoryBus&)`。
- `IsaRegistry`：按 `arch` 注册和创建 ISA 实例，提供 `register_all_isas`。

### `include/mcus/cpu.h`

CPU 状态与执行：

- `CpuRegisters`：通用寄存器、PC、SP、flags。
- `Cpu`：继承 `MemoryBus`，持有 ISA 实例、内存块、暂停/停机标志、中断嵌套栈。
- 方法：`step`、`load_firmware`、`reset`、`export_memory`、`read8/16/32`、`write8/16/32`、`handle_mmio_write/read`、`check_interrupt`、`enter_interrupt`、`on_reti`、`check_reset_pin`、`check_inject_slots`。

### `include/mcus/simulator.h`

`SimulatorConfig` 和 `Simulator`：持有模型、固件、配置、共享内存、事件流、CPU；提供 `run`、`stop`；内部 `cpu_loop` 和 `clock_loop`，处理控制区命令、背压暂停、虚拟时间推进、定时器推进、dump、复位。

### `include/mcus/config.h`

TOML 配置结构：`SimulatorSection`、`CrystalSection`、`DebugSection`、`Config`，提供 `load_from_file` 和 `apply_to_model`。

### `include/mcus/log.h`

分级日志：`LogLevel`、`log_set_level`、`log_set_output`、`log_write`，宏 `MCUS_TRACE/DEBUG/INFO/WARN/ERROR`。

## 6. 实现文件

### `src/model.cpp`

解析 JSON 模型文件，填充 `Model`，建立寄存器地址索引和空间顺序，执行校验：区域不重叠、复位向量和初始 SP 在合法空间、寄存器宽度合法、同地址多语义必须通过别名互指、引脚编号唯一、中断向量和使能/挂起寄存器已定义、定时器寄存器已定义。

### `src/shared_memory.cpp`

创建/映射 POSIX 共享内存，按模型计算控制区、MMIO 区、引脚表、中断位图、注入区、dump 区偏移；提供按空间和地址读写 MMIO 寄存器；`reset_all` 恢复复位值、引脚复位功能、清中断位图和注入区；`export_mmio` 按寄存器顺序导出 MMIO 字节。

### `src/event_stream.cpp`

创建/映射事件流共享内存，环形队列头尾原子指针、容量、背压标志；`push` 满时置背压返回 false，`pop` 空时返回 false。

### `src/cpu.cpp`

CPU 核心：

- 构造时按模型非 MMIO 区域分配内存块，初始化 ISA。
- `mem_offset` 按空间和 `addr_mask` 计算偏移。
- `load_firmware` 将固件拷贝到复位向量所在区域。
- `reset` 恢复寄存器、清中断栈、复位共享内存。
- `read8/16/32`、`write8/16/32` 按空间分发，MMIO 走 `handle_mmio_*`，普通内存直接读写。
- `handle_mmio_write` 按别名选语义，处理写 1 清零，更新引脚表，按 `WriteTrigger`/`Fifo` 推事件。
- `handle_mmio_read` 按别名选语义，只写寄存器返回复位值，读清零后清零。
- `check_inject_slots` 消费注入区。
- `check_interrupt` 检查挂起位和中断位图，进入中断。
- `enter_interrupt` 保存 IP 到中断栈，压 PC，清挂起位和位图，跳向量。
- `on_reti` 恢复 IP。
- `check_reset_pin` 检测复位引脚并执行复位。
- `step` 依次检查停机、暂停、复位、注入、中断，然后执行一条指令，更新控制区 PC 和停机状态。

### `src/simulator.cpp`

模拟器主循环：

- `cpu_loop`：处理控制区命令、检查暂停和背压、执行 `cpu_.step`、累加虚拟时间、推进定时器。
- `clock_loop`：周期性诊断，停机后退出。
- `handle_control_commands`：暂停、继续、单步、dump、复位。
- `do_dump`：导出普通内存或 MMIO 到 dump 缓冲区，可选落文件。
- `advance_clock`：按 `overflow_action` 推进定时器，溢出时置标志和中断位图。

### `src/config.cpp`

解析 TOML 配置，填默认值，`apply_to_model` 用配置覆盖模型晶振频率和每指令周期数。

### `src/isa_registry.cpp`

`IsaRegistry` 单例，`register_isa` 注册工厂，`create` 按 `arch` 创建实例；`register_all_isas` 调用各 ISA 模块的注册函数。

### `src/isa_8051.cpp`

8051 指令集实现：

- `execute` 开头 `Addr pc = regs.pc;`，`pc` 始终指向 opcode。
- 覆盖标准 8051 指令：数据传送、算术、逻辑、位操作、跳转、调用返回、栈、乘除、移位、MOVX/MOVC。
- `0xA5` 按 `wch_dptr1_movx` 扩展启用，切换 DPS 后写 xRAM 并递增 DPTR。
- 辅助函数：`push`/`pop`、`read_acc`/`write_acc`（更新 P 标志）、`read_dpl/dph`、`write_dpl/dph`、`read_cy/ac`、`write_cy/ac/ov`、`reg_bank_base`、`reg_read/write`、`direct_read/write`、`bit_read/write`、`add_a`、`subb_a`。
- 末尾统一 `regs.pc = pc`。

### `src/log.cpp`

日志级别过滤、输出到 stderr 或文件、互斥锁保护。

## 7. 测试

### `tests/test_event_stream.cpp`

验证环形队列 push/pop、满/空、背压标志。

### `tests/test_shared_memory.cpp`

验证共享内存布局、MMIO 读写、引脚表、复位、中断位图、注入区。

### `tests/test_cpu_minimal.cpp`

加载 dummy 模型和手写固件，验证 MMIO 写事件、中断进入、复位。

### `tests/test_timer.cpp`

验证定时器描述完整性和手动溢出置位逻辑。

### `tests/test_ch552g.cpp`

验证 CH552G 模型加载、别名寄存器、位地址解析、定时器描述。

## 8. 工具

### `tools/dump_shm.cpp`

按模型文件和共享内存名打开共享内存，打印控制区、MMIO 寄存器、引脚表、中断位图，用于调试。

## 9. 共享内存与事件流契约

外部程序不属于本项目，通过共享内存和事件流与模拟器交互：

- **共享内存布局**：控制区 → MMIO 寄存器区 → 引脚表 → 中断位图 → 注入区头 → 注入区槽 → dump 缓冲区。
- **事件流**：Header（head/tail/capacity/backpressure）+ `MmioEvent` 数组。模拟器 push，外部程序 pop。
- **控制区命令**：Pause、Resume、Step、Dump、Reset。
- **中断注入**：外部程序写挂起寄存器位、中断位图，或往注入区写 `InjectSlot`。
- **引脚**：外部程序读写 `PinState` 的 function/value/direction。

## 10. 当前遗留问题

### 10.1 `blink.bin` 的 `RET` 弹出错误地址

**现象**：用 SDCC 编译的 CH552G 闪灯固件 `blink.bin` 加载后，`pc` 在 `0x00AC` 到 `0x00AD` 之间执行 `delay()` 的 `DJNZ R6`，然后跳到 `0x13195` 或 `0x1491B` 等远超固件范围的地址，`P1` 一直是 `0xFF`。

**已排查**：

- `LCALL` 的 `push` 是 `pc + 3`，`RET` 的 `pop` 是 `hi`、`lo`，`pc = (hi << 8) | lo`，代码正确。
- `blink.bin` 里有 `MOV SP, #0x07`，`SP` 被设为 `7`。
- `delay()` 用 `volatile` 变量，SDCC 生成 `MOV R6, #0xC8`、`MOV R7, #0`、`DJNZ R6`、`INC R7`、`CJNE R7, #0xC8`、`JC`、`RET`，不占 `0x08`/`0x09`。
- `push`/`pop` 日志显示 `push iram[0x49] = 0x206`、`push iram[0x50] = 0x0`，说明 `SP` 是 `0x48`，不是 `7`；且 `push` 的 `v` 日志出现 `0x206`，`uint8_t` 不可能为 `0x206`，日志字符串拼接或 `v` 的类型存在异常。

**待确认**：

1. 为什么 `blink.bin` 的 `MOV SP, #0x07` 没有生效，`SP` 是 `0x48`（`ch552g.json` 的 `initial_sp`）。需要确认 `LCALL` 之前是否执行了 `MOV SP`，或者 `Cpu::reset` 的 `regs_.sp` 被后续操作覆盖。
2. `push iram[0x49] = 0x206` 的 `0x206` 不是 `uint8_t` 的可能值，需要检查 `push` 日志字符串拼接、`std::to_string` 的参数类型，以及 `push` 的调用方是否传入了非 `uint8_t` 的值。
3. `RET` 弹出的地址是 `0x00BD` 而不是 `0x00BE`，说明 `iram[0x31]` 或 `iram[0x32]` 在 `push` 和 `pop` 之间被修改。需要定位写 `iram[0x31]`/`iram[0x32]` 的指令或代码路径。

### 10.2 日志与调试代码残留

`src/cpu.cpp`、`src/isa_8051.cpp`、`src/simulator.cpp` 中仍保留部分排查用的 `MCUS_DEBUG` 日志（如 `read8 space=...`、`mem_offset A/B/C`、`push iram...`、`stored virtual_time...`）。在问题解决后需要清理，避免日志刷屏和性能开销。

### 10.3 外部监控程序偏移依赖

`ch552g_monitor/main.cpp` 里 `kRegisterCount` 硬编码为模型寄存器数量。如果 `ch552g.json` 的 `registers` 数组长度变化，`pins` 偏移会错。后续应改为读模型文件计算偏移，或在共享内存布局中导出偏移描述。

### 10.4 `Cpu::write8("code", ...)` 的潜在风险

`Cpu::write8` 允许写 `code` 区，`isa_8051.cpp` 里没有写 `code` 的指令，但若未来扩展指令或外部注入误传 `space == "code"`，会修改固件。建议在 `Cpu::write8` 里对 `code` 区写操作加警告或拒绝。
