# mcus 项目文档

## 1. 项目定位

`mcus` 是一个**通用单片机模拟器**。它不模拟芯片内部的门级或电气行为，而是提供一个“时间对齐的指令执行环境 + 寄存器语义层”，让为单片机编译的固件以为自己运行在真实芯片上。

核心设计原则：

- **虚拟时间单一来源**：所有组件按虚拟晶振周期推进，不跟真实时间对齐。
- **芯片差异在模型文件**：地址空间、寄存器语义、定时器/外设结构、引脚复用、中断向量全部由 `models/*.json` 描述；核心不按芯片名、空间名或外设寄存器名做分支，换架构只换 ISA 与模型。
- **指令集可插拔**：按模型文件的 `arch` 字段加载 ISA 模块，当前有 `mcus-minimal`、`8051` 和 `ARM Cortex-M3` 三个 ISA。
- **外部程序不在本项目内**：外设模型通过共享内存和双向事件流与模拟器交互，只碰状态，不调用模拟器内部函数。
- **总线事件化**：UART/SPI/USB 的收发都走 `BusEvent`/`UsbEvent`；SPI 等同步总线在外部程序 attach 后冻结 CPU 等待响应。
- **流满背压暂停**：事件流 out 满时 CPU 线程暂停推进；in 满时外部 `push_in` 返回失败。
- **复位由 RST 引脚触发**：外部程序写 `reset_pin`，模拟器在指令边界检测并执行复位序列。

## 2. 目录结构

```
mcus/
├── CMakeLists.txt
├── README.md
├── doc.md
├── docs/
├── config/
│   ├── example.toml
│   └── debug.toml
├── examples/
│   └── stm32f103c8t6/
├── models/
│   ├── dummy.json
│   ├── ch552g.json
│   ├── stc90c51.json
│   ├── interface_test.json
│   ├── stm32f103c8t6.json
│   ├── generic.json
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
│   ├── bytecode.h
│   ├── jit.h
│   ├── firmware.h
│   ├── isa_plugin.h
│   ├── json.h
│   └── log.h
├── src/
│   ├── model.cpp
│   ├── shared_memory.cpp
│   ├── event_stream.cpp
│   ├── cpu.cpp
│   ├── simulator.cpp
│   ├── config.cpp
│   ├── firmware.cpp
│   ├── json.cpp
│   ├── isa_base.cpp
│   ├── isa_registry.cpp
│   ├── isa_8051.cpp
│   ├── isa_arm.cpp
│   ├── isa_jit_8051.cpp
│   ├── jit.cpp
│   ├── jit_x86.cpp
│   ├── main.cpp
│   └── log.cpp
├── tests/
│   ├── CMakeLists.txt
│   ├── test_event_stream.cpp
│   ├── test_shared_memory.cpp
│   ├── test_cpu_minimal.cpp
│   ├── test_interrupt.cpp
│   ├── test_peripherals.cpp
│   ├── test_arm_isa.cpp
│   ├── test_generic_model.cpp
│   ├── test_timer.cpp
│   ├── test_breakpoint.cpp
│   ├── test_firmware.cpp
│   ├── test_jit.cpp
│   ├── test_jit_diff.cpp
│   ├── test_jit_timing.cpp
│   ├── test_jit_event_timing.cpp
│   ├── test_jit_peripherals_timing.cpp
│   ├── test_jit_cache_helpers.cpp
│   └── test_ch552g.cpp
└── tools/
    ├── dump_shm.cpp
    └── bench.cpp
```

## 3. 构建与运行

### 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

ISA 可以单独构建，不构建主程序：

```bash
cmake --build build --target mcus_isa_8051
cmake --build build --target mcus_isa_arm
```

JIT 也可以单独构建：

```bash
cmake --build build --target mcus_jit_8051
```

详见 `docs/isa_build.md` 和 `docs/jit_build.md`；编写 JIT 插件见 `docs/jit.md`。

### 运行

```bash
./build/mcus --isa ./build/libmcus_isa_8051.so \
  --model <model.json> --firmware <firmware.hex|firmware.bin> \
  --config <config.toml>
```

四个必填参数：

- **ISA 库**：架构指令集动态库，如 `libmcus_isa_8051.so` / `libmcus_isa_arm.so`。
- **模型文件**：描述芯片静态结构。
- **固件**：`.bin` 或 `.hex`/`.ihx`。
- **配置文件**：共享内存名、事件流容量、晶振频率、日志级别等。

可选参数：

- `--jit <translator.so>`：启用 JIT；当前 8051 可用。也可以在 `[simulator] jit` 中配置。

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
- `simulator.stream_capacity`：out/in 每个事件环的容量，默认 4096。
- `simulator.bus_wait_timeout_ms`：外部程序 attach 后，同步总线等待响应的真实时间上限，默认 1000ms。
- `simulator.start_paused`：启动后是否立即暂停。
- `crystal.frequency_hz`：虚拟晶振频率。
- `crystal.cycles_per_instruction`：每条指令粗略虚拟耗时。
- `simulator.jit`：可选，8051 JIT Translator 插件路径；命令行 `--jit` 优先。
- `debug.log_level`：`trace`/`debug`/`info`/`warn`/`error`。
- `debug.log_output`：`stderr` 或文件路径。

### `config/debug.toml`

调试用配置，启动即暂停、缩小事件流容量、开详细日志。

### `models/schema.md`

模型文件字段说明入口，完整字段见 `docs/model_guide.md`。

### `models/dummy.json`

最小可运行模型：一段 Flash、一段 RAM、一段 MMIO、一个可写触发事件的寄存器、一个引脚、一个中断、一个定时器。用于验证框架闭环和测试。

### `models/ch552g.json`

WCH CH552G 模型：

- 地址空间：`code`（16KB）、`iram`（256B）、`sfr`（128B）、`xram`（1KB，`addr_mask=0x3FF`）。
- 寄存器：P1、P3、PSW、ACC、B、SP、DPL、DPH、TCON、TMOD、TL0/TH0、TL1/TH1、SCON、SBUF、T2CON、T2MOD、RCAP2L/H、TL2/TH2、PWM、SPI、UART1、ADC、TKEY、USB 等。
- 同地址多语义：`SAFE_MOD`/`CHIP_ID` 通过 `read_alias`/`write_alias` 互指；`ROM_CTRL`/`ROM_STATUS` 同理。
- 中断：14 个中断源，向量地址与手册一致。
- 定时器：Timer0/1 为 16 位，`overflow_action: "isa"`；Timer2 为 16 位自动重载，`overflow_action: "reload"`。
- 引脚：P1.0–P1.7、P3.0–P3.7、RST，`source_reg` 指向对应端口 SFR，`direction_reg` 指向 `P1_DIR_PU`/`P3_DIR_PU`。
- 外设：`uarts`（UART0/UART1）、`spis`（SPI0）、`pwms`、`adcs`、`tkeys`、`gpio_irq`、`usb`，寄存器/引脚/时序全部由模型描述。
- ISA 扩展：`wch_dptr1_movx`，启用 0xA5 扩展指令。

### `models/stc90c51.json`

STC90C51RC：4KB Flash、256B iRAM + 256B xRAM、P0–P4、Timer0/1/2、UART0、INT0–INT3。
详细限制见 `docs/stc90c51.md`。

### `models/stm32f103c8t6.json`

ARM Cortex-M3 模型：

- 地址空间：`flash`（64KB @ `0x08000000`）、`sram`（20KB @ `0x20000000`）、`periph`（`0x40000000`）、`system`（`0xE0000000`）。
- 外设：GPIOA/B/C、USART1/2/3、SPI1/2、TIM2/3/4、ADC1、NVIC/SCB/SysTick、RCC/FLASH/AFIO/EXTI。
- I2C/USB/CAN/DMA 目前是寄存器骨架，无协议引擎。
- ISA：`src/isa_arm.cpp`，外部库 `libmcus_isa_arm.so`。

## 5. 头文件

### `include/mcus/types.h`

基础类型：`VirtTime`（虚拟时间，晶振周期数）、`Addr`（32 位地址）、`SpaceId`（地址空间名）、`DebugEvent`（纯 POD 事件，含 `MmioEvent`/`PinEvent`/`BusEvent`/`IrqEvent`/`UsbEvent`）、`RegSemantics`、`ControlCommand`、`ControlStatus`。

### `include/mcus/model.h`

模型结构：

- `MemoryRegion`：空间名、基址、大小、是否 MMIO、`addr_mask`。
- `RegisterDesc`：寄存器名、空间、地址、宽度、语义、复位值、`read_alias`/`write_alias`、位别名。
- `PinDesc`：引脚名、编号、复用功能、复位功能、`source_space`/`source_reg`/`source_bit`。
- `InterruptDesc`：中断号、向量地址、使能/挂起寄存器与位。
- `CounterDesc`/`TimerDesc`：计数器结构、使能/溢出位、中断号、溢出行为。
- `UartDesc`/`SpiDesc`/`PwmDesc`/`AdcDesc`/`TkeyDesc`/`GpioIrqDesc`/`UsbDesc`：总线、定时外设、GPIO 边沿中断和 USB 端点描述。
- `Model`：聚合以上，提供 `load_from_file`、`validate`、`find_region`、`find_register`、`find_register_by_name`、`find_interrupt`、`space_index`。

### `include/mcus/shared_memory.h`

共享内存布局与控制结构：

- `ControlBlock`：命令、状态、单步计数、dump 地址/长度/就绪、复位引脚、虚拟时间、PC、停机标志。
- `PinState`：功能、值、方向。
- `InjectSlot`/`InjectHeader`：注入区。
- `SharedMemory`：POSIX 共享内存映射，提供 `control`、`pins`、`interrupt_bitmap`、`inject_header`、`inject_slots`、`read_reg`、`write_reg`、`reset_all`、`dump_buffer`、`export_mmio`。

### `include/mcus/event_stream.h`

同一个共享内存对象内 out/in 两个 SPSC 环 + 全局 `seq` + 双向背压标志。模拟器 `push`/`pop_in`，外部程序 `pop`/`push_in`；`host_attached` 供同步总线判断是否需要等待外部响应。

### `include/mcus/instruction.h`

指令集接口与注册表：

- `ExecResult`：消耗虚拟时间、是否停机。
- `MemoryBus`：按空间读写 8/16/32 位，`on_reti` 默认空实现。
- `InstructionSet`：`execute(CpuRegisters&, MemoryBus&)`。
- `IsaRegistry`：按 `arch` 注册和创建 ISA 实例，提供 `register_all_isas`。

### `include/mcus/jit.h`

JIT 插件接口：`JitTranslator`、`JitContext`、`JitCodeSource`、`BcProgram`、`NativeProgram`、
`JitEngine`，以及 `kJitAbiVersion`、`kJitContextLayoutHash` 和插件 ABI 符号。

### `include/mcus/bytecode.h`

JIT 字节码：固定 8 字节的 `BcInsn`、`BcOp`、`BcProgram`、字节码缓存文件头；
`CallHelper` 的参数布局是 `a=helper id, b=arg1(8bit), imm=arg0(32bit)`。

### `include/mcus/cpu.h`

CPU 状态与执行：

- `CpuRegisters`：通用寄存器、PC、SP、flags。
- `Cpu`：继承 `MemoryBus`，持有 ISA 实例、内存块、暂停/停机标志、中断服务栈、外设运行时状态。
- 方法：`step`、`run_jit`、`run_full_checks`、`enable_jit`、`jit_enabled`、`advance_peripherals`、`load_firmware`、`reset`、`export_memory`、`read8/16/32`、`write8/16/32`、`handle_mmio_write/read`、`check_interrupt`、`enter_interrupt`、`on_reti`、`check_reset_pin`、`check_inject_slots`、`poll_bus_in`、`poll_external_pins`、`waiting_for_bus`、`force_complete_spi`。

### `include/mcus/simulator.h`

`SimulatorConfig` 和 `Simulator`：持有模型、固件、配置、共享内存、事件流、CPU；提供 `run`、`stop`、`enable_jit`；内部 `cpu_loop` 和 `clock_loop`，处理控制区命令、事件流背压、同步总线等待/超时、虚拟时间推进、定时器/外设推进、dump、复位；启用 JIT 时按 chunk 调用 `Cpu::run_jit`，并用 tick 回调保持 `advance_clock` 粒度。

### `include/mcus/config.h`

TOML 配置结构：`SimulatorSection`、`CrystalSection`、`DebugSection`、`Config`，提供 `load_from_file` 和 `apply_to_model`。

### `include/mcus/log.h`

分级日志：`LogLevel`、`log_set_level`、`log_set_output`、`log_write`，宏 `MCUS_TRACE/DEBUG/INFO/WARN/ERROR`。

## 6. 实现文件

### `src/model.cpp`

解析 JSON 模型文件，填充 `Model`，建立寄存器地址索引和空间顺序，执行校验：区域不重叠、复位向量和初始 SP 在合法空间、寄存器宽度合法、同地址多语义必须通过别名互指、引脚编号唯一、中断向量和使能/挂起寄存器已定义、定时器寄存器已定义、外设寄存器/IRQ/引脚映射合法、USB 端点缓冲不越界。

### `src/shared_memory.cpp`

创建/映射 POSIX 共享内存，按模型计算控制区、MMIO 区、引脚表、中断位图、注入区、dump 区、断点区偏移；提供按空间和地址读写 MMIO 寄存器；`reset_all` 恢复寄存器复位值、从端口/方向寄存器同步引脚初值、清中断位图和注入区；`export_mmio` 按寄存器顺序导出 MMIO 字节。

### `src/event_stream.cpp`

创建/映射事件流共享内存，out/in 两个 SPSC 环的头尾原子指针、容量、全局 seq、双向背压标志；`push`/`pop` 走 out，`push_in`/`pop_in` 走 in；attach 或 `push_in` 时置 `host_attached`。

### `src/cpu.cpp`

CPU 核心：

- 构造时按模型非 MMIO 区域分配内存块，初始化 ISA。
- `mem_offset` 按空间和 `addr_mask` 计算偏移。
- `load_firmware` 将固件拷贝到复位向量所在区域。
- `reset` 恢复寄存器、清中断栈、复位共享内存。
- `read8/16/32`、`write8/16/32` 按空间分发，MMIO 走 `handle_mmio_*`，普通内存直接读写。
- `handle_mmio_write` 按别名选语义，处理写 1 清零，更新引脚表，按 `WriteTrigger`/`Fifo` 推事件，并调用外设侧效应。
- `handle_mmio_read` 按别名选语义，端口读叠加 `PinState.value`，读清零后清零，并调用外设读侧效应。
- `advance_peripherals`：推进 UART 发送、SPI 完成、PWM 计数/引脚、ADC 转换、TKEY 扫描。
- `poll_bus_in`：消费 in 环的 UART RX、SPI MISO、USB SETUP/IN/OUT/Reset。
- `poll_external_pins`：每步检测外部 `PinState` 变化，发 Pin 事件、计算脉宽并触发 GPIO 边沿中断。
- `check_inject_slots` 消费注入区。
- `check_interrupt` 检查挂起位和中断位图；同号/同级中断在服务期间屏蔽，避免中断风暴。
- `enter_interrupt` 保存上下文、压 PC、按 `clear_pending` 清挂起位、跳向量，并记录服务状态。
- `on_reti` 恢复上下文并弹出中断服务状态。
- `check_reset_pin` 检测复位引脚并执行复位。
- `step` 依次检查停机、暂停、复位、总线入站、外部引脚、注入、断点、中断，然后执行一条指令；SPI 等待外部响应时返回 0 冻结时间。
- `run_jit` 用 8051 JIT 翻译块执行；每条 guest 指令后推进外设并做完整检查，未覆盖 opcode/冷代码回退解释器；命中字节码缓存时也会创建 translator。

### `src/simulator.cpp`

模拟器主循环：

- `cpu_loop`：处理控制区命令、检查暂停/背压/同步总线等待；JIT 开启时按 `control_poll_interval` 为 chunk 调用 `run_jit`，否则执行 `cpu_.step`；推进外设与虚拟时间、推进定时器；总线等待超时后强制完成。
- `clock_loop`：周期性诊断，停机后退出。
- `handle_control_commands`：暂停、继续、单步、dump、复位。
- `do_dump`：导出普通内存或 MMIO 到 dump 缓冲区，可选落文件。
- `advance_clock`：按 `overflow_action` 推进定时器，溢出时置标志和中断位图。

### `src/config.cpp`

解析 TOML 配置，填默认值（含 `stream_capacity`、`bus_wait_timeout_ms` 等），`apply_to_model` 用配置覆盖模型晶振频率和每指令周期数。

### `src/isa_registry.cpp`

`IsaRegistry` 单例，`register_isa` 注册工厂，`create` 按 `arch` 创建实例；`register_all_isas` 调用各 ISA 模块的注册函数。

### `src/isa_8051.cpp`

8051 指令集实现：

- `execute` 开头 `Addr pc = regs.pc;`，`pc` 始终指向 opcode。
- 覆盖标准 8051 指令：数据传送、算术、逻辑、位操作、跳转、调用返回、栈、乘除、移位、MOVX/MOVC。
- `0xA5` 按 `wch_dptr1_movx` 扩展启用，切换 DPS 后写 xRAM 并递增 DPTR。
- 辅助函数：`push`/`pop`、`read_acc`/`write_acc`（更新 P 标志）、`read_dpl/dph`、`write_dpl/dph`、`read_cy/ac`、`write_cy/ac/ov`、`reg_bank_base`、`reg_read/write`、`direct_read/write`、`bit_read/write`、`add_a`、`subb_a`。
- 末尾统一 `regs.pc = pc`。

### `src/jit.cpp`

JIT 插件加载、ABI/布局校验、字节码翻译缓存（`.mbc`）、字节码执行器；命中缓存时也会创建
translator，确保 helper 可用。

### `src/jit_x86.cpp`

x86-64 后端：`BcProgram` → 宿主机器码，ACC/PSW/IRAM/SFR 快速路径、`CallHelper`、
`CallIsa`、`BranchIfPc`，以及 native 机器码缓存。

### `src/isa_jit_8051.cpp`

8051 JIT Translator：guest 指令解码、块生成、SFR/GPIO/栈/中断/乘除 helper。

### `src/log.cpp`

日志级别过滤、输出到 stderr 或文件、互斥锁保护。

## 7. 测试

### `tests/test_event_stream.cpp`

验证 out/in 双环 push/pop、push_in/pop_in、满/空、双向背压标志、attach。

### `tests/test_shared_memory.cpp`

验证共享内存布局、MMIO 读写、引脚表、复位、中断位图、注入区。

### `tests/test_cpu_minimal.cpp`

加载 dummy 模型和手写固件，验证 MMIO 写事件、中断进入、复位。

### `tests/bench_*`

CTest 冒烟基准：`bench_minimal`、`bench_8051`、`bench_arm`，规模 20,000 步。

### `tests/test_arm_isa.cpp`

加载 `models/stm32f103c8t6.json`，执行包含向量表、MOVW/MOVT、STR、BL、PUSH/POP 的 ARM 固件，验证 Cortex-M3 复位/调用/GPIO。

### `tests/test_generic_model.cpp`

使用 `flash`/`ram`/`periph` 这类非 8051 空间名和 `mcus-minimal` ISA，验证核心没有硬编码空间名，换架构只换 ISA 和模型。

### `tests/test_interrupt.cpp`

验证 `clear_pending=false` 中断不会重入：进入 ISR 后 SP 只压一次，第二步执行向量处指令。

### `tests/test_peripherals.cpp`

用 CH552G 模型验证 UART0/UART1 收发与 TI/RI、SPI0 请求/响应、ADC 转换、TKEY 扫描、PWM 波形/中断、GPIO 输入采样/边沿中断、USB IN/OUT 包事件。

### `tests/test_stc90c51.cpp`

加载 STC90C51RC 模型，执行 `MOV P1,#0xAA`，验证 SFR 与引脚同步。

### `tests/test_interfaces.cpp`

用 `models/interface_test.json` 验证引脚复用、I2C、CAN、DAC、ADC 注入、正交编码器、GPIO 脉宽输入和波形输出。

### `tests/test_timer.cpp`

验证定时器描述完整性和手动溢出置位逻辑。

### `tests/test_ch552g.cpp`

验证 CH552G 模型加载、别名寄存器、位地址解析、定时器描述。

### JIT 测试

- `tests/test_jit.cpp`：native 编译/执行/缓存冒烟；
- `tests/test_jit_diff.cpp`：解释器 vs JIT 的 PC/now/ACC/PSW/内存差分，覆盖
  `CJNE Rn,#imm`、`LCALL/ACALL` 栈语义；
- `tests/test_jit_timing.cpp`：中断事件时间/入口 PC/最终 now；
- `tests/test_jit_event_timing.cpp`：UART TX `BusEvent` 时间戳；
- `tests/test_jit_peripherals_timing.cpp`：GPIO/PWM/SPI/UART RX/ADC/TKEY/USB；
- `tests/test_jit_cache_helpers.cpp`：命中字节码缓存后 helper 仍非空；
- `tests/test_simulator_timer.cpp` 的 `simulator_timer_jit*`：真实 `mcus --jit` 路径的
  Timer0 计数。

## 8. 工具

### `tools/bench.cpp`

性能基准：CPU 指令吞吐、事件流吞吐、共享内存寄存器访问；`--json` 可机读；
`--jit <translator.so>` 可测试 JIT 路径。

### `tools/dump_shm.cpp`

按模型文件和共享内存名打开共享内存，打印控制区、MMIO 寄存器、引脚表、中断位图，用于调试。

## 9. 共享内存与事件流契约

外部程序不属于本项目，通过共享内存和事件流与模拟器交互：

- **共享内存布局**：控制区 → MMIO 寄存器区 → 引脚表 → 中断位图 → 注入区头 → 注入区槽 → dump 缓冲区 → 断点区。
- **事件流**：同一个共享内存对象内 out/in 两个 SPSC 环，元素为 `DebugEvent`（Mmio/Pin/Bus/Irq/Usb），共用一个全局 `seq`。模拟器 `push`/`pop_in`，外部程序 `pop`/`push_in`。
- **控制区命令**：Pause、Resume、Step、Dump、Reset。
- **中断注入**：外部程序写挂起寄存器位、中断位图，或往注入区写 `InjectSlot`。
- **引脚**：外部程序读写 `PinState` 的 function/value/direction；输入引脚变化会被模拟器在指令边界采样，产生 Pin 事件和 GPIO 边沿中断。
- **总线**：UART/SPI/I2C/CAN/USB/DAC/ADC/编码器/捕获全部通过 `DebugEvent` 收发的数据流，详细字段见 `docs/interfaces.md`。

## 10. 限制

### 10.1 CH552 USB 的 xSFR 未建模

`ch552g.json` 已建模 USB 控制/中断寄存器和 5 个端点，端点缓冲固定映射在 `xram:0x2C0`（`usb.buffer_base`），每个端点 64 字节。CH552 的 `UEPn_DMA`/`UEPn_T_LEN` 等 xSFR 尚未加入模型，真实固件如果依赖这些寄存器设置缓冲地址/发送长度，需要补模型。

### 10.2 只读内存区域

`regions[].read_only=true` 的普通内存区域（CH552 的 `code`）拒绝 CPU 写并告警；固件加载仍可直接写入。核心不按“复位空间名字”判断，换架构只需在模型里标注只读区域。

### 10.3 其他架构限制

ARM Cortex-M3 的未实现指令、I2C/USB/CAN/DMA 协议引擎、EXTI 接入等限制见 `docs/stm32.md`。

### 10.4 JIT 覆盖范围

JIT 目前只有 8051 Translator；ARM 仍走解释器。8051 模型没有建模的 I2C/CAN 等外设没有
JIT 时序回归；新增 JIT 架构时需要补 `docs/jit.md` 第 7 节的测试。

## 11. JIT 文档

- [docs/jit.md](docs/jit.md)：如何编写 JIT Translator 插件
- [docs/jit_build.md](docs/jit_build.md)：JIT 插件单独编译/安装
- [docs/benchmark.md](docs/benchmark.md)：JIT benchmark 与时序测试
