# mcus(MCU Simulator)

`mcus` 是一个**通用单片机模拟器**。它不模拟芯片内部的门级或电气行为，而是提供一个“时间对齐的指令执行环境 + 寄存器语义层”，让为单片机编译的固件以为自己运行在真实芯片上。

## 核心设计原则

- **虚拟时间单一来源**：所有组件按虚拟晶振周期推进，不跟真实时间对齐。
- **芯片差异在模型文件**：地址空间、寄存器语义、定时器结构、引脚复用、中断向量全部由 `models/*.json` 描述，核心不写死具体芯片。
- **指令集可插拔**：按模型文件的 `arch` 字段加载 ISA 模块，当前有 `mcus-minimal` 和 `8051` 两个 ISA。
- **外部程序不在本项目内**：外设模型通过共享内存和事件流与模拟器交互，只碰状态，不调用模拟器内部函数。
- **流满背压暂停**：事件流满时 CPU 线程暂停推进，避免丢事件导致行为不一致。
- **复位由 RST 引脚触发**：外部程序写 `reset_pin`，模拟器在指令边界检测并执行复位序列。

## 功能验证进度

CH552G 功能多，当前验证进度：

| 功能 | 状态 | 固件/说明 |
|---|---|---|
| GPIO | ✅ | `blink.bin` |
| `MOVX` / xRAM | ✅ | `xram_test.bin` |
| `MOVC` / code 读 | ✅ | `movc_test.bin` |
| 算术 / `DA A` | ✅ | `arith_test.bin` |
| 位操作（`iram` 位地址） | ✅ | `bit_test.bin` |
| `Timer0` + 中断 | ✅ | `timer_test.bin` |
| `INT0` 外部中断 | ✅ | `int0_test.bin` |
| `UART0` | ❌ | `uart_test.bin`，`SCON` 写 `0x80` 不是 `0x50` |
| `UART1` | ⬜ | 未测试 |
| `SPI` | ⬜ | 未测试 |
| `PWM` | ⬜ | 未测试 |
| `ADC` | ⬜ | 未测试 |
| `TKEY` | ⬜ | 未测试 |
| `USB` | ⬜ | 未测试（协议未模拟） |
| `WDOG` | ⬜ | 未测试 |
| `Timer2` 及捕捉 | ⬜ | 未测试 |

`ctest` 5/5 通过（`event_stream`、`shared_memory`、`cpu_minimal`、`timer`、`ch552g`）。

## 构建与运行

### 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

### 运行

```bash
./build/mcus <model.json> <firmware.bin> <config.toml>
```

三个启动参数：

- **模型文件**：描述芯片静态结构。
- **固件二进制**：加载到模型文件描述的非 MMIO 区域。
- **配置文件**：共享内存名、事件流容量、晶振频率、日志级别等。

示例：

```bash
./build/mcus models/ch552g.json blink.bin config/example.toml
```

### 调试工具

```bash
./build/dump_shm <model.json> <shm_name>
```

打印控制区、MMIO 寄存器、引脚表、中断位图。

**注意**：`dump_shm` 以只读方式打开共享内存（`create=false`），不会重置模拟器状态。`mcus` 运行时用另一个终端 `dump_shm`，可实时观察 `P1`、`pc`、`virtual_time`。

## 配置与模型文件

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

## 寄存器语义

- `plain`：普通读写。
- `write_only`：只写，读返回 `reset_value`。
- `read_only`：只读，写忽略。
- `write_trigger`：写时推 `MmioEvent`，**读返回共享内存当前值**（不是 `reset_value`）。用于 GPIO 端口等“写触发、读回显”的寄存器。
- `fifo`：写时推事件，用于 `SBUF`、`SPI0_DATA` 等。
- `write_one_clear`：写 1 清零。
- `read_clear`：读后清零。

**注意**：`write_trigger` 的读语义是共享内存当前值，不是 `reset_value`。这是 `P1` 等 GPIO 端口能正确 `CPL`/`SETB` 的关键。

## 共享内存与事件流契约

外部程序不属于本项目，通过共享内存和事件流与模拟器交互：

- **共享内存布局**：控制区 → MMIO 寄存器区 → 引脚表 → 中断位图 → 注入区头 → 注入区槽 → dump 缓冲区。
- **事件流**：Header（head/tail/capacity/backpressure）+ `MmioEvent` 数组。模拟器 push，外部程序 pop。
- **控制区命令**：Pause、Resume、Step、Dump、Reset。
- **中断注入**：外部程序写挂起寄存器位、中断位图，或往注入区写 `InjectSlot`。
- **引脚**：外部程序读写 `PinState` 的 function/value/direction。

### `SharedMemory` 的 `create` 参数

`SharedMemory(const Model&, const std::string& shm_name, bool create = true)`：

- `create=true`：`mcus` 启动时创建并初始化共享内存（`memset` + `reset_all`）。
- `create=false`：`dump_shm` 等只读工具打开已有共享内存，**不初始化、不重置**，只读当前值。

## 测试

```bash
ctest --test-dir build --output-on-failure
```

- `test_event_stream`：环形队列 push/pop、满/空、背压。
- `test_shared_memory`：共享内存布局、MMIO 读写、引脚表、复位、中断位图、注入区。
- `test_cpu_minimal`：加载 dummy 模型和手写固件，验证 MMIO 写事件、中断进入、复位。
- `test_timer`：定时器描述完整性和手动溢出置位逻辑。
- `test_ch552g`：CH552G 模型加载、别名寄存器、位地址解析、定时器描述。

当前 5/5 通过。

## 当前遗留问题

### 1. 串口 `UART0` 未通过

`uart_test.bin` 的 `SCON = 0x50` 未生效：`case 0x75` 的 `fetch(bus, 0xD5)` 返回 `0x80`，`imm` 不是 `0x50`，`SCON` 写成 `0x80`（模式 0），`SBUF` 写不推 `TI`，`uart_isr` 不触发。**`uart_test.bin` 的 `0xD5=0x50`，`load_firmware` 后 `code[0xD5]` 应为 `0x50`，但 `fetch` 返回 `0x80`。待查 `code_blk_->data[0xD5]` 被谁改成 `0x80`，或 `fetch` 的 `pc+2` 偏移错。**

### 2. 通信/模拟外设未验证

`UART1`、`SPI`、`PWM`、`ADC`、`TKEY`、`USB`、`WDOG`、`Timer2` 及捕捉均未写固件验证。模拟器只实现寄存器语义和事件推流，**USB 协议、ADC 采样、SPI 时序未模拟**，这些外设的固件测试需外部程序配合写共享内存。

### 3. `check_interrupt` 每步调的性能

`check_interrupt` 每步遍历 14 个中断，每个 `read_reg` 走 `reg_offset_fast_` O(1)。当前 `virtual_time` 每秒 28 万步（`clock_step_interval=100`），可接受。单步调试和 `test_cpu_minimal` 依赖每步调，**不要改成每 N 步**。性能优化已做：`reg_offset_fast_` 直接寻址、`read8` 缓存 `MemBlock*`、`check_interrupt` 先查 `EA`。

### 4. 外部监控程序偏移依赖

`ch552g_monitor/main.cpp` 里 `kRegisterCount` 硬编码为模型寄存器数量。如果 `ch552g.json` 的 `registers` 数组长度变化，`pins` 偏移会错。后续应改为读模型文件计算偏移，或在共享内存布局中导出偏移描述。

### 5. `Cpu::write8("code", ...)` 保护

`Cpu::write8` 已对 `code` 区写操作拒绝并 WARN，防止误写固件。**保留此保护。**

## 已修复的问题（记录）

1. **`SP` 双状态**：`regs_.sp` 和 SFR `0x81` 不同步。修：SP 只用 SFR `0x81`，`enter_interrupt` 压 2 字节 PC，与 `RETI` 对称。
2. **`write_trigger` 读 `reset_value`**：`handle_mmio_read` 对 `write_trigger` 返回 `reset_value`，`P1` 读永远 `0xFF`，`CPL` 永远写 `0xFE`。修：`write_trigger` 读 `shm_`，只 `write_only` 返回 `reset_value`。
3. **`dump_shm` 构造 `reset_all`**：`dump_shm` 打开共享内存时 `memset` + `reset_all`，把 `P1` 刷回 `0xFF`。修：`SharedMemory` 加 `create` 参数，`dump_shm` 传 `false`。
4. **`check_interrupt` 每 100 步**：单步调试和 `test_cpu_minimal` 依赖每步调。修：`check_interrupt` 每步调，性能用 `advance_clock` 每 100 步优化。
5. **日志十进制/十六进制混淆**：`std::to_string` 十进制前面加 `0x`，`pc=0x76` 实际是 `0x4C`，误导排查。修：日志用 `%04X`/`%02X`。
6. **`pc` 每步 `store` 性能**：`pc`/`virtual_time` 每步 `std::atomic::store` 有内存屏障。修：`store` 加 `std::memory_order_relaxed`，单步调试 `pc` 每步更新，性能 28 万步/秒。
7. **`reg_offset_` 的 `unordered_map::find` 慢**：`read_reg`/`write_reg` 每次 `find`。修：`reg_offset_fast_` 按 `(space_idx+1)*256 + addr` 直接寻址，`addr >= 256` 的 `mmio` 走 `reg_offset_` 回退。
8. **`read8` 的 `find_region`/`mem_offset` 慢**：`fetch` 每次 `read8("code", addr)` 走 `find_region`。修：`Cpu` 缓存 `code_blk_`/`iram_blk_`/`xram_blk_`，`read8` 快速路径。
9. **`SBUF` 写不置 `TI`**：`SBUF` 是 `fifo`，写只推事件，不置 `TI`。修：`handle_mmio_write` 的 `fifo` 分支，`SBUF`（`sfr` 0x99）写后置 `SCON` 的 `TI`（bit1）。`ch552g.json` 的 `UART0` `pending_bit` 改 `1`（`TI`）。
