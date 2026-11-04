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

构建产物：

- `build/mcus`：模拟器主程序
- `build/libmcus_core.so`：模拟器核心
- `build/libmcus_isa_8051.so`：8051 ISA 动态库（外部加载）

### 运行

ISA 是外部插件，启动时必须显式指定 ISA 库、芯片模型、固件和配置：

```bash
mcus --isa <isa.so> --model <model.json> --firmware <firmware> --config <config.toml>
```

四个启动参数：

- **ISA 库**：架构指令集动态库，如 `libmcus_isa_8051.so`；模拟器会校验库导出的 `arch` 与模型 `arch` 是否一致。
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

欢迎提交 Issue 和 Pull Request。请参阅[ToDo文档](ToDo.md)

## 联系方式
---

作者

Xue Zicheng <xuezicheng842@outlook.com>

GitCode: https://gitcode.com/xzc842

---
