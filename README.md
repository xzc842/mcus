# mcus (MCU Simulator)

`mcus` 是一个**通用单片机模拟器**。它不模拟芯片内部的门级或电气行为，而是提供一个“时间对齐的指令执行环境 + 寄存器语义层”，让为单片机编译的固件以为自己运行在真实芯片上。

## 核心设计原则

- **虚拟时间单一来源**：所有组件按虚拟晶振周期推进，不跟真实时间对齐。
- **芯片差异在模型文件**：地址空间、寄存器语义与副作用、定时器/外设结构、引脚方向/复用、中断优先级与向量全部由 `models/*.json` 描述；核心不按芯片名、空间名或外设寄存器名做分支，换架构只换 ISA 与模型。
- **指令集可插拔**：ISA 编译成外部 `.so`，通过 `--isa` 加载；核心只调用 ISA 的 `execute`、`reset`、`save_context`、`restore_context`、`resolve_bit_addr`、`interrupt_vector` 等钩子。当前内置 `mcus-minimal`，`8051` 和 `ARM Cortex-M3` 是外部库；单独构建 ISA 见 [docs/isa_build.md](docs/isa_build.md)。
- **外部程序不在本项目内**：外设模型通过共享内存和事件流与模拟器交互，只碰状态，不调用模拟器内部函数。
- **统一双向事件流**：MMIO / GPIO / 总线 / 中断 / USB 都进入同一个 `DebugEvent` 共享内存对象，带全局 `seq`，保证顺序；out 环给外部程序消费，in 环把总线响应/UART RX/USB 包送回模拟器；支持 attach 模式，不重置队列。
- **GPIO 事件化**：引脚只在状态变化时发 PIN 事件，外部程序/插件不轮询引脚；当前值仍可通过 `PinState` 读取。
- **流满背压暂停**：事件流满时 CPU 线程暂停推进，避免丢事件导致行为不一致。
- **复位由 RST 引脚触发**：外部程序写 `reset_pin`，模拟器在指令边界检测并执行复位序列。

## 功能列表

### CH552G (8051)

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
| SPI0 寄存器 + 中断 | `spi0_test` | ✅ |
| PWM 寄存器 + 中断 + 引脚输出 | `pwm_test` + `test_peripherals` | ✅ |
| ADC 寄存器 + 转换 + 中断 | `adc_test` + `test_peripherals` | ✅ |
| TKEY 寄存器 + 扫描 + 中断 | `tkey_test` + `test_peripherals` | ✅ |
| WDOG 寄存器 | `wdog_test` | ✅ |
| WDOG 溢出复位 | `wdog_test` | ✅ |
| USB 寄存器 + 包事件 | `usb_test` + `test_peripherals` | ✅ |
| UART0 发送/接收 + TI/RI 中断 | `uart_test` + `test_peripherals` | ✅ |
| UART1 发送/接收 + TI1/RI1 中断 | `uart1_test` + `test_peripherals` | ✅ |
| GPIO 输入采样 + 边沿中断 | `test_peripherals` | ✅ |

### 总线与 USB 的收发模型

所有总线都不在核心里实现协议栈，统一走 `DebugEvent` 事件流：

- **out 环**：模拟器 → 外部程序（MMIO、Pin、Bus 请求、Irq、USB 事件）。
- **in 环**：外部程序 → 模拟器（UART RX、SPI MISO、USB SETUP/IN/OUT、注入）。
- 两个环共用一个全局 `seq`，见 [include/mcus/types.h](include/mcus/types.h) 和 [include/mcus/event_stream.h](include/mcus/event_stream.h)。
- 外部程序建议先 attach 事件流并 `Resume`，再让固件运行（`start_paused=true`）；SPI 在外部程序 attach 后会冻结 CPU 等待 MISO 响应，超时由 `bus_wait_timeout_ms` 控制。

UART/SPI/USB 的具体收发约定见 [docs/external.md](docs/external.md)，ARM/STM32 见 [docs/stm32.md](docs/stm32.md)。

### 通用数据流接口

核心统一支持并走 `DebugEvent` 事件流：

- UART、SPI、I2C、CAN、USB；
- ADC、DAC、PWM、正交编码器；
- 所有 GPIO 的脉宽输入（`PinEvent.pulse_width`），并支持外部通过 `PulseWidth` 事件直接输入波宽；
- GPIO 波形输出；
- 模型驱动的引脚复用（`pins[].mux_*`）。

字段、事件格式和示例见 [docs/interfaces.md](docs/interfaces.md)。

### STC90C51RC（8051）

仓库提供 `models/stc90c51.json`：

- 4KB Flash、256B iRAM + 256B xRAM；
- P0–P4、Timer0/1/2、UART0、INT0–INT3；
- IE/IP/IPH、XICON、AUXR/AUXR1、WDT_CONTR、ISP 寄存器；
- UART/中断走 8051 ISA 与 `BusEvent`/中断机制。

限制和 SFR 细节见 [docs/stc90c51.md](docs/stc90c51.md)。

### STM32F103C8T6（ARM Cortex-M3）

仓库提供 `models/stm32f103c8t6.json` 和 `build/libmcus_isa_arm.so`：

- Flash 64KB @ `0x08000000`（只读）、SRAM 20KB @ `0x20000000`；
- GPIOA/B/C（ODR/IDR/BSRR/BRR）、USART1/2/3、SPI1/2、TIM2/3/4、ADC1、NVIC、SCB、SysTick、RCC/FLASH/AFIO/EXTI；
- NVIC `ISER/ICER/ISPR/ICPR`、PRIMASK（`CPSID/CPSIE`/`MRS/MSR`）、SysTick 异常和外部中断；
- USART/SPI/ADC 仍走 BusEvent/UsbEvent 事件流，外部设备模型通过 `push_in()` 注入 RX/MISO 或读取 TX/MOSI 事件；
- I2C/USB/CAN/DMA 目前只有寄存器骨架，没有协议引擎。

详细地址、限制和编译示例见 [docs/stm32.md](docs/stm32.md)。

### 性能基准

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j

./build-release/mcus_bench --model models/dummy.json
./build-release/mcus_bench --model models/ch552g.json \
  --isa ./build-release/libmcus_isa_8051.so
./build-release/mcus_bench --model models/stm32f103c8t6.json \
  --isa ./build-release/libmcus_isa_arm.so
```

Release 实测（2026-11-08，Intel Core Ultra 5 225H，Linux 7.0.0-42-generic，
`-DCMAKE_BUILD_TYPE=Release`，1,000,000 步，warmup 10,000；CPU 项 repeat 3 取最好，
表中为多次运行的中位数/代表值）：

| 模型 / ISA | CPU steps/s | 虚拟周期/s | ns/step | 事件流 events/s | 共享内存 ops/s |
|---|---:|---:|---:|---:|---:|
| `dummy` / `mcus-minimal` | 16,650,301 | 66,601,202 | 60.06 | 26,200,000 | 29,200,000 |
| `ch552g` / `8051` | 15,460,790 | 15,460,790 | 64.68 | 26,100,000 | 43,050,000 |
| `stm32f103c8t6` / `arm` | 13,589,000 | 13,589,000 | 73.60 | 26,000,000 | 32,800,000 |

`dummy` 的虚拟周期是 steps 的 4 倍，因为模型 `cycles_per_instruction=4`；8051/ARM 为 1。
本机数据随 CPU 频率/负载有约 ±10% 波动；Debug `-O0` 下数字会低一个数量级，不要拿来比较。
按模型晶振折算，8051 约 0.64x 实时（24MHz），STM32F103 约 0.19x 实时（72MHz）。

`ctest -L bench` 里的三条基准只跑 20,000 步，用于冒烟和防回归，不代表上表性能。

`--json` 可机读；完整参数和指标说明见 [docs/benchmark.md](docs/benchmark.md)。

### 通用性

核心不包含 CH552/8051 的分支：空间名、寄存器地址、外设寄存器/位、引脚映射、中断向量、栈方向全部来自模型；架构差异由外部 ISA 动态库提供。仓库里的 `models/generic.json` 使用 `flash`/`ram`/`periph` 空间名和 `mcus-minimal` ISA，`tests/test_generic_model.cpp` 作为“换架构只换 ISA + 模型”的回归测试。

### 外部设备模型边界

- UART/SPI/I2C/CAN 的“真实对端”（串口设备、SPI/I2C 从机、CAN 节点）由外部程序通过 `push_in()` 模拟。
- USB 已实现端点缓冲、SETUP/IN/OUT 包事件和 USB 中断；但 CH552 的 USB DMA/T_LEN 等 xSFR 尚未建模，端点缓冲固定映射在模型 `usb.buffer_base` 指定的 xRAM 区域。
- ADC 可注入采样值，DAC 输出数字转换事件；GPIO 脉宽、波形和编码器都走 `PinEvent`/`BusEvent`，详细格式见 [docs/interfaces.md](docs/interfaces.md)。

## 构建与运行

### 构建

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

构建产物：

- `build/mcus`：模拟器主程序
- `build/libmcus_core.so`：模拟器核心
- `build/libmcus_isa_8051.so`：8051 ISA 动态库（外部加载）
- `build/libmcus_isa_arm.so`：ARM Cortex-M3 Thumb/Thumb-2 ISA 动态库（外部加载）
- `build/mcus_bench`：性能基准工具
- `build/libmcus_isa_sdk.a`：ISA SDK 静态库

### 运行

ISA 是外部插件，启动时必须显式指定 ISA 库、芯片模型、固件和配置：

```bash
mcus --isa <isa.so> --model <model.json> --firmware <firmware> --config <config.toml>
```

四个启动参数：

- **ISA 库**：架构指令集动态库，如 `libmcus_isa_8051.so` 或 `libmcus_isa_arm.so`；模拟器会校验库导出的 `arch` 与模型 `arch` 是否一致。
- **模型文件**：描述芯片静态结构。
- **固件**：加载到模型文件描述的非 MMIO 区域；支持 `.bin` 和 `.hex`/`.ihx`（Intel HEX）。
- **配置文件**：共享内存名、事件流容量、晶振频率、日志级别等。

示例：

```bash
./build/mcus \
  --isa ./build/libmcus_isa_8051.so \
  --model models/ch552g.json \
  --firmware blink.bin \
  --config config/example.toml
```

也可以直接喂 SDCC 生成的 `.hex`：

```bash
./build/mcus \
  --isa ./build/libmcus_isa_8051.so \
  --model models/ch552g.json \
  --firmware build/firmware.hex \
  --config config/example.toml
```

### 调试工具

```bash
./build/dump_shm <model.json> <shm_name>
```

打印控制区、MMIO 寄存器、引脚表、中断位图。

**注意**：`dump_shm` 以只读方式打开共享内存（`create=false`），不会重置模拟器状态。`mcus` 运行时用另一个终端 `dump_shm`，可实时观察 `P1`、`pc`、`virtual_time`。

## 致谢

本项目的开发离不开以下优秀开源项目和社区的贡献，在此我们表示由衷的感谢。

| 项目 | 用途 | 开源协议 |
| :--- | :--- | :--- |
| [nlohmann/json](https://github.com/nlohmann/json.git) | JSON 解析（模型/配置读取） | MIT  |
| [toml11](https://github.com/ToruNiina/toml11.git) | TOML 解析（配置文件读取） | MIT  |

## 开源许可

Apache-2.0 [LICENSE](LICENSE)

## 贡献

欢迎提交 Issue 和 Pull Request。请参阅[项目文档](doc.md)

## 联系方式
---

作者

Xue Zicheng <xuezicheng842@outlook.com>

GitCode: https://gitcode.com/xzc842

---
