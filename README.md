# mcus(MCU Simulator)

`mcus` 是一个**通用单片机模拟器**。它不模拟芯片内部的门级或电气行为，而是提供一个“时间对齐的指令执行环境 + 寄存器语义层”，让为单片机编译的固件以为自己运行在真实芯片上。

## 核心设计原则

- **虚拟时间单一来源**：所有组件按虚拟晶振周期推进，不跟真实时间对齐。
- **芯片差异在模型文件**：地址空间、寄存器语义、定时器结构、引脚复用、中断向量全部由 `models/*.json` 描述，核心不写死具体芯片。
- **指令集可插拔**：按模型文件的 `arch` 字段加载 ISA 模块，当前有 `mcus-minimal` 和 `8051` 两个 ISA。
- **外部程序不在本项目内**：外设模型通过共享内存和事件流与模拟器交互，只碰状态，不调用模拟器内部函数。
- **流满背压暂停**：事件流满时 CPU 线程暂停推进，避免丢事件导致行为不一致。
- **复位由 RST 引脚触发**：外部程序写 `reset_pin`，模拟器在指令边界检测并执行复位序列。

## 功能列表

### CH552G(8051) 
已验证：

| 功能 | 测试 | 结果 |
|---|---|---|
| GPIO | `blink.bin` | ✅ |
| MOVX（xRAM） | `xram_test` | ✅ |
| MOVC（code 读） | `movc_test` | ✅ |
| 算术（ADD/DA A） | `arith_test` | ✅ |
| 位操作（iram 位地址） | `bit_test` | ✅ |
| Timer0 中断 | `timer_test` | ✅ |
| Timer1 中断 | `timer1_test` | ✅ |
| Timer2 中断 + reload | `timer2_test` | ✅ |
| 外部中断 INT0 | `int0_test` | ✅ |
| SPI0 寄存器 | `spi0_test` | ✅ |
| PWM 寄存器 | `pwm_test` | ✅ |
| ADC 寄存器 | `adc_test` | ✅ |
| TKEY 寄存器 | `tkey_test` | ✅ |
| WDOG 寄存器 | `wdog_test` | ✅ |
| WDOG 溢出复位 | `wdog_test` | ✅ |
| USB 寄存器 | `usb_test` | ✅ |

部分验证：

| 功能 | 测试 | 状态 |
|---|---|---|
| UART0 | `uart_test` | SCON/SBUF 寄存器通，TI 中断/P1 采样待查 |
| UART1 | `uart1_test` | SCON1/SBUF1/SBAUD1 寄存器通，TI1 中断待查 |
| SPI0 中断 | `spi0_test` | SPI0_DATA/CTRL 寄存器通，SPIF 中断/jnb acc.4 待查 |

未实现：

PWM/ADC/TKEY/GPIO 中断（需要 advance_clock 或 handle_mmio_write 置挂起位）、USB 协议层。


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
