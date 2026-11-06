# 模型文件编写指南

`models/*.json` 描述一颗芯片的静态结构：地址空间、寄存器、引脚、中断、定时器、架构级参数。`Cpu`/`Simulator` 不含芯片硬编码，换芯片只换模型文件和 ISA 插件。

## 1. 文件位置

```
models/
├── ch552g.json
├── dummy.json
└── schema.md
```

新建芯片模型，在 `models/` 下加 `<chip_name>.json`，跑：

```bash
./build/mcus \
  --isa ./build/libmcus_isa_8051.so \
  --model models/<chip_name>.json \
  --firmware firmware.bin \
  --config config.toml
```

## 2. 顶层字段

```json
{
  "name": "ch552g",
  "description": "WCH CH552G, E8051 core, 16KB ROM, 256B iRAM, 1KB xRAM",
  "arch": "8051",
  "crystal_hz": 24000000,
  "cycles_per_instruction": 1,
  "reset_vector": 0,
  "reset_space": "code",
  "initial_sp": 7,
  "sp_space": "iram",
  "isa_extensions": ["wch_dptr1_movx"],
  "arch_desc": { ... },
  "regions": [ ... ],
  "registers": [ ... ],
  "pins": [ ... ],
  "interrupts": [ ... ],
  "timers": [ ... ]
}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 芯片名，日志用 |
| `description` | string | 描述 |
| `arch` | string | ISA 名，`"8051"`/`"mcus-minimal"`/自定义 |
| `crystal_hz` | uint32 | 虚拟晶振频率（Hz） |
| `cycles_per_instruction` | uint32 | 每条指令粗略虚拟耗时，默认 1 |
| `reset_vector` | uint32 | 复位向量地址 |
| `reset_space` | string | 复位向量所在空间 |
| `initial_sp` | uint32 | 初始 SP |
| `sp_space` | string | 栈空间 |
| `isa_extensions` | string[] | ISA 扩展名，如 `wch_dptr1_movx` |
| `arch_desc` | object | 架构级描述，见第 3 节 |
| `regions` | object[] | 地址空间，见第 4 节 |
| `registers` | object[] | 寄存器，见第 5 节 |
| `pins` | object[] | 引脚，见第 6 节 |
| `interrupts` | object[] | 中断，见第 7 节 |
| `timers` | object[] | 定时器，见第 8 节 |

**`reset_vector`/`reset_space`/`initial_sp`/`sp_space` 是顶层字段，`arch_desc` 里也有同名，`arch_desc` 优先。** **新模型建议只写 `arch_desc`，顶层字段兼容旧模型。**

## 3. `arch_desc`：架构级描述

```json
"arch_desc": {
  "global_irq_enable": { "space": "sfr", "reg": 168, "bit": 7, "present": true },
  "irq_push_bytes": 2,
  "stack_grows_down": false,
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

| 字段 | 类型 | 说明 |
|---|---|---|
| `global_irq_enable` | object | 全局中断使能，`space`/`reg`/`bit`，`present=false` 表示无 |
| `irq_push_bytes` | uint8 | 中断压栈宽度，8051 是 2，ARM 是 4/8 |
| `stack_grows_down` | bool | 栈增长方向，8051=false，ARM/RISC-V=true |
| `fast_spaces` | string[] | 快速空间，`code`/`iram`/`xram`，别的架构 `flash`/`ram` |
| `reset_space` | string | 复位向量空间 |
| `reset_vector` | uint32 | 复位向量地址 |
| `sp_space` | string | 栈空间 |
| `initial_sp` | uint32 | 初始 SP |
| `sp_reg` | uint32 | SP 寄存器地址（8051 是 `sfr` 0x81） |
| `sp_reg_space` | string | SP 寄存器所在空间 |
| `has_pc_reg` | bool | 是否有外部 PC 寄存器（8051 没有） |

**`global_irq_enable` 是 8051 的 `IE` bit7（`EA`），ARM 的 `PRIMASK`，RISC-V 的 `mstatus.MIE`。** **`ch552g.json` 的 `IE` 是 `sfr` 168，bit7。**

## 4. `regions`：地址空间

```json
"regions": [
  { "name": "code", "space": "code", "base": 0,    "size": 16384, "is_mmio": false },
  { "name": "iram", "space": "iram", "base": 0,    "size": 256,   "is_mmio": false },
  { "name": "sfr",  "space": "sfr",  "base": 128,  "size": 128,   "is_mmio": true  },
  { "name": "xram", "space": "xram", "base": 0,    "size": 1024,  "is_mmio": false, "addr_mask": 1023 }
]
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 区域名 |
| `space` | string | 空间名，`code`/`iram`/`sfr`/`xram`/`flash`/`ram`/`mmio` |
| `base` | uint32 | 基址 |
| `size` | uint32 | 大小 |
| `is_mmio` | bool | 是否 MMIO（寄存器空间） |
| `addr_mask` | uint32 | 地址掩码，`0` 表示不掩，`1023` 表示 1KB 循环 |

**`is_mmio=true` 的区域，`read8`/`write8` 走 `handle_mmio_read`/`handle_mmio_write`，按 `registers` 的 `semantics` 处理。** **`is_mmio=false` 的区域，直接读 `memory_[space].data[off]`；`read_only=true` 的区域拒绝 CPU 写（固件加载仍直接写入）。**

**`addr_mask` 用于 `xram` 这种小容量空间，`DPTR` 高 6 位被掩，`addr_mask=1023` 表示 1KB。**

## 5. `registers`：寄存器

```json
"registers": [
  { "name": "P1", "space": "sfr", "addr": 144, "width": 8, "semantics": "write_trigger", "reset_value": 255 },
  { "name": "SP", "space": "sfr", "addr": 129, "width": 8, "semantics": "plain", "reset_value": 7 },
  { "name": "SAFE_MOD", "space": "sfr", "addr": 161, "width": 8, "semantics": "write_only", "reset_value": 0, "read_alias": "CHIP_ID" },
  { "name": "CHIP_ID",  "space": "sfr", "addr": 161, "width": 8, "semantics": "read_only",  "reset_value": 82, "write_alias": "SAFE_MOD" }
]
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 寄存器名 |
| `space` | string | 空间 |
| `addr` | uint32 | 地址 |
| `width` | uint8 | 8/16/32 |
| `semantics` | string | 语义，见下表 |
| `reset_value` | uint32 | 复位值 |
| `read_alias` | string | 读别名（同地址多语义） |
| `write_alias` | string | 写别名 |
| `on_write` | array | 写后执行的通用动作，见下 |
| `on_read` | array | 读后执行的通用动作，见下 |

### `on_write` / `on_read`：寄存器副作用

用模型描述“写 A 后置 B 的某位”这类副作用，核心执行，不需要在 ISA 里写死：

```json
{ "name": "CMD", "space": "sfr", "addr": 0xC0, "width": 8, "semantics": "plain",
  "on_write": [ { "action": "set_bit", "space": "sfr", "addr": 0xC1, "bit": 0 } ] }
```

| `action` | 作用 |
|---|---|
| `set_bit` | 目标寄存器 `bit` 置 1 |
| `clear_bit` | 目标寄存器 `bit` 清 0 |
| `set_value` | 目标寄存器写 `value` |
| `clear_value` | 目标寄存器写 0 |

动作由 `Cpu` 在读写后直接写共享内存，不会再次触发引脚/事件/动作，适合状态位这类副作用。

**UART/SPI/USB 不要用 `on_write` 模拟收发时序**：它们由 `uarts`/`spis`/`usb` 段描述，写数据寄存器会发总线事件，标志由虚拟时间推进后置位。

### `semantics` 语义

| 值 | 读 | 写 | 用途 |
|---|---|---|---|
| `plain` | 返回 `shm_` 值 | 写 `shm_` | 普通寄存器 |
| `write_only` | 返回 `reset_value` | 写 `shm_` | 只写 |
| `read_only` | 返回 `shm_` 值 | 忽略，`WARN` | 只读 |
| `write_trigger` | 返回 `shm_` 值 | 写 `shm_`，推 `MmioEvent` | GPIO 端口 |
| `fifo` | 返回 `shm_` 值 | 写 `shm_`，推 `MmioEvent` | `SBUF`/`SPI0_DATA` |
| `write_one_clear` | 返回 `shm_` 值 | 写 1 清零 | 中断标志 |
| `read_clear` | 返回 `shm_` 值，读后清零 | 忽略 | 读清零 |

**`write_trigger` 读 `shm_` 当前值，不是 `reset_value`，`P1` 的 `CPL`/`SETB` 依赖这个。** **`fifo` 写推 `MmioEvent`，外部程序 `pop` 消费。**

### 同地址多语义

**`SAFE_MOD`/`CHIP_ID` 同地址 `sfr` 0xA1，`read_alias`/`write_alias` 互指：**

- 读 `0xA1`：`find_register("sfr", 0xA1)` 返回 `SAFE_MOD`，`read_alias="CHIP_ID"`，`find_register_by_name("CHIP_ID")`，返回 `CHIP_ID`，读 `shm_` 的 `CHIP_ID` 值；
- 写 `0xA1`：`find_register("sfr", 0xA1)` 返回 `SAFE_MOD`，`write_alias` 空，写 `shm_` 的 `SAFE_MOD` 值。

**`read_alias`/`write_alias` 必须互指，`validate` 检查同地址多语义必须有 alias。**

## 6. `pins`：引脚

```json
"pins": [
  { "name": "P1.0", "number": 0, "functions": ["gpio", "t2_cap1", "tin0"],
    "reset_function": 0, "source_space": "sfr", "source_reg": 144, "source_bit": 0 }
]
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 引脚名 |
| `number` | uint32 | 引脚编号 |
| `functions` | string[] | 复用功能 |
| `reset_function` | uint32 | 复位功能索引 |
| `source_space` | string | 源寄存器空间 |
| `source_reg` | uint32 | 源寄存器地址 |
| `source_bit` | uint8 | 源寄存器位 |
| `direction_space` | string | 方向寄存器空间 |
| `direction_reg` | uint32 | 方向寄存器地址 |
| `direction_bit` | uint8 | 方向位 |

**`source_space`/`source_reg`/`source_bit` 用于寄存器位到引脚表的同步。** **`P1` 写 `0xFE`，`P1.0` 的 `source_bit=0`，引脚表 `P1.0` 的 `value=0`。**

**`direction_*` 可选：写方向寄存器时同步更新 `PinState.direction`，核心不假设方向寄存器叫什么。复位时 `PinState.value/direction` 会从 `source_reg`/`direction_reg` 的复位值同步，不再一律置 0。**

## 7. `interrupts`：中断

```json
"interrupts": [
  { "name": "INT0", "number": 0, "vector_addr": 3, "vector_space": "code",
    "enable_reg": 168, "enable_space": "sfr", "enable_bit": 0,
    "pending_reg": 136, "pending_space": "sfr", "pending_bit": 1,
    "clear_pending": true }
]
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 中断名 |
| `number` | uint32 | 中断号，`interrupt(N)` 用 |
| `priority` | uint32 | 优先级，数值越小越优先；默认 `0`（同一优先级不能嵌套） |
| `vector_addr` | uint32 | 向量地址 |
| `vector_space` | string | 向量空间 |
| `enable_reg` | uint32 | 使能寄存器地址 |
| `enable_space` | string | 使能寄存器空间 |
| `enable_bit` | uint8 | 使能位 |
| `pending_reg` | uint32 | 挂起寄存器地址 |
| `pending_space` | string | 挂起寄存器空间 |
| `pending_bit` | uint8 | 挂起位 |
| `clear_pending` | bool | 进中断时是否清挂起位，默认 `true` |

**`clear_pending=false` 用于 UART/SPI/ADC/TKEY/PWM/GPIO/USB 这类“标志由软件清”的中断源。`enter_interrupt` 不清标志，但 `Cpu` 会记录中断服务状态，同号/同级中断在 `RETI` 前不会重入，因此不会出现中断风暴。**

## 8. `timers`：定时器

```json
"timers": [
  {
    "name": "TIMER0",
    "counter": { "bits": 16, "lo_reg": 138, "hi_reg": 140,
                 "reg_space": "sfr", "reload_lo_reg": 138, "reload_hi_reg": 140 },
    "enable":   { "reg": 136, "bit": 4 },
    "overflow": { "reg": 136, "bit": 5 },
    "irq": 1,
    "overflow_action": "isa"
  }
]
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `name` | string | 定时器名 |
| `counter.bits` | uint8 | 位数 |
| `counter.lo_reg` | uint32 | 低字节寄存器；`hi_reg=0` 时它是完整计数器寄存器 |
| `counter.hi_reg` | uint32 | 高字节寄存器；0 表示单寄存器计数器 |
| `counter.reg_space` | string | 计数器空间 |
| `counter.reload_lo_reg` | uint32 | 重载低字节 |
| `counter.reload_hi_reg` | uint32 | 重载高字节 |
| `counter.up` | bool | 递增/递减，默认 `true` |
| `enable.reg` | uint32 | 使能寄存器 |
| `enable.bit` | uint8 | 使能位 |
| `enable.space` | string | 使能寄存器空间，默认 `counter.reg_space` |
| `overflow.reg` | uint32 | 溢出标志寄存器 |
| `overflow.bit` | uint8 | 溢出位 |
| `overflow.space` | string | 溢出寄存器空间，默认 `counter.reg_space` |
| `irq` | uint32 | 中断号 |
| `overflow_action` | string | `isa`/`reload`/`reset` |
| `prescaler` | uint32 | 每 N 个虚拟周期计数一次，默认 1 |

**计数器有两种写法：**

- **双 8 位寄存器**：`bits > 8` 且 `hi_reg != 0`，`(hi << 8) | lo`，8051 Timer0/1/2 用这种；
- **单寄存器**：`hi_reg == 0`，直接读写 `lo_reg`，宽度取模型里该寄存器的 `width`（8/16/32 位通用）。

**`prescaler` 由模型描述，核心不再假设“每个周期都计数”。**

**`overflow_action`：**

- `"isa"`：溢出只置标志，不重载，由 ISA 处理（`Timer0`/`Timer1`）；
- `"reload"`：溢出重载 `reload_lo_reg`/`reload_hi_reg`（`Timer2`）；
- `"reset"`：溢出置 `reset_pin=1`，触发复位（`WDOG`）。

**`WDOG` 的 `counter.up=false`，递减，`enable`/`overflow` 是 `CLOCK_CFG` bit5，`overflow_action: "reset"`。**

## 9. 外设：`uarts` / `spis` / `pwms` / `adcs` / `tkeys` / `gpio_irq` / `usb`

外设的驱动逻辑在 `Cpu` 里，但**寄存器、引脚、时序全部由模型描述**。`Cpu` 只按这些描述生成事件、置标志和驱动引脚。

### 9.1 `uarts`

```json
"uarts": [
  { "name": "UART0", "irq": 4, "space": "sfr", "ctrl": 152, "data": 153,
    "tx_flag_bit": 1, "rx_flag_bit": 0, "tx_cycles": 64, "rx_fifo": 8 }
]
```

| 字段 | 说明 |
|---|---|
| `ctrl` | 状态/控制寄存器，发送/接收标志所在 |
| `data` | 数据寄存器 |
| `tx_flag_bit` / `rx_flag_bit` | 发送完成/接收标志位 |
| `tx_cycles` | 一个字节的虚拟发送时间；0=立即置 TI |
| `rx_fifo` | 接收 FIFO 深度 |

写 `data` 会发 `BusEvent(UartTx)`；外部 `push_in(BusEvent(UartRx))` 会写入 FIFO、置接收标志。读 `data` 会取走 FIFO 数据并在 FIFO 空时清接收标志。旧字段名 `scon`/`sbuf`/`ti_bit`/`ri_bit` 仍作为别名兼容。

### 9.2 `spis`

```json
"spis": [
  { "name": "SPI0", "irq": 6, "space": "sfr", "data": 249, "stat": 248,
    "ctrl": 250, "done_bit": 4, "busy_bit": 7, "xfer_cycles": 8 }
]
```

写 `data` 发 `BusEvent(SpiXfer, ToHost)`；外部在 `xfer_cycles` 内回 `SpiXfer(ToMcu, xfer_id)`，模拟器把 MISO 写回 `data` 并置 `done_bit`。没有外部响应时 MISO 默认 `0xFF`。

### 9.3 `pwms`

```json
"pwms": [
  { "name": "PWM", "irq": 11, "ctrl": 157, "data1": 156, "data2": 155,
    "ck_se": 158, "if_bit": 4, "enable_bit1": 0, "enable_bit2": 1,
    "period": 256, "prescaler": 1,
    "pins": [
      { "channel": 1, "pin": "P1.5", "function": "pwm1" },
      { "channel": 2, "pin": "P3.1", "function": "pwm2" }
    ] }
]
```

`period` 是计数器周期（按 `prescaler` 分频后的 tick），`pins[].pin` 是 `pins` 里的名字。PWM 运行时会把引脚方向置为输出、函数切到 `pins[].function` 指定的名字（由模型提供，不硬编码 `pwm1`/`pwm2`），并按占空比更新 `PinState.value`。

### 9.4 `adcs`

```json
"adcs": [
  { "name": "ADC", "irq": 9, "space": "sfr", "ctrl": 128, "cfg": 154,
    "data": 159, "start_bit": 0, "if_bit": 5, "busy_bit": 4,
    "conversion_cycles": 96, "channel_mask": 15, "channel_shift": 0,
    "channels": [ { "channel": 0, "pin": "P1.1" } ] }
]
```

写 `ctrl.start_bit=1` 启动转换；`conversion_cycles` 后按 `(cfg >> channel_shift) & channel_mask` 选通道，采样对应 `PinState.value`（外部程序写入的模拟量），按 `data` 寄存器宽度截断后写入并置 `if_bit`。

### 9.5 `tkeys`

```json
"tkeys": [
  { "name": "TKEY", "irq": 7, "space": "sfr", "ctrl": 195, "datl": 196,
    "dath": 197, "start_bit": 0, "if_bit": 7, "scan_cycles": 256,
    "data_bits": 16, "channels": ["P1.0", "P1.1", "P1.4", "P3.2"] }
]
```

写 `ctrl.start_bit=1` 启动扫描；`scan_cycles` 后把各通道引脚电平拼成 `data_bits` 位写入 `datl`/`dath`，并置 `if_bit`。

### 9.6 `gpio_irq`

```json
"gpio_irq": {
  "irq": 12, "enable_reg": 199, "enable_space": "sfr",
  "enable_bit_base": 0, "enable_bits": 8
}
```

外部程序改 `PinState.value` 时，模拟器在指令边界检测输入引脚边沿；对应 `GPIO_IE` 位为 1 时置 `GPIO` 中断挂起位。`enable_bit` 取 `(pin.number - enable_bit_base) % enable_bits`。

**方向约定：`direction=0` 是输入，`1` 是输出。** 端口读会始终把 `PinState.value` 叠加回寄存器，因此外部程序注入电平不依赖方向位；方向位主要用于 GPIO 边沿中断和对外描述。CH552 的 `P1_DIR_PU`/`P3_DIR_PU` 复位值是 `0xFF`（输出），GPIO 中断只在输入方向下触发。

### 9.7 `usb`

```json
"usb": {
  "irq": 8, "space": "sfr", "int_fg": 216, "int_st": 217, "mis_st": 218, "rx_len": 219,
  "int_en": 225, "ctrl": 226, "dev_ad": 227, "int_fg_bit": 1,
  "buffer_space": "xram", "buffer_base": 704, "buffer_stride": 64,
  "endpoints": [ { "index": 0, "ctrl": 220, "max_packet": 64 } ]
}
```

USB 数据包走 `DebugEventType::Usb`，端点缓冲放在 `buffer_space:buffer_base + index*buffer_stride`。外部主机通过 `UsbToken::Setup/Out/In/Reset` 与模拟器交互；固件写 `UEPn_CTRL` 时模拟器发 `EpReady`。

## 10. 编写步骤

### 1. 确定芯片架构

- `arch`：`"8051"`/`"mcus-minimal"`/自定义，新架构写 `isa_*.cpp`，`register_isa`；
- `arch_desc`：`global_irq_enable`、`irq_push_bytes`、`fast_spaces`、`reset_space`/`reset_vector`、`sp_space`/`initial_sp`/`sp_reg`/`sp_reg_space`。

### 2. 列地址空间

- `code`/`iram`/`sfr`/`xram`/`flash`/`ram`/`mmio`；
- `base`/`size`/`is_mmio`/`addr_mask`；
- `sfr`/`mmio` 是 `is_mmio=true`，其余 `false`。

### 3. 列寄存器

- 每个寄存器的 `name`/`space`/`addr`/`width`/`semantics`/`reset_value`；
- 同地址多语义用 `read_alias`/`write_alias`；
- GPIO 端口用 `write_trigger`，`SBUF`/`SPI0_DATA` 用 `fifo`。

### 4. 列引脚

- `name`/`number`/`functions`/`reset_function`；
- `source_space`/`source_reg`/`source_bit` 指向端口 SFR。

### 5. 列中断

- `number`/`vector_addr`/`enable_reg`/`enable_bit`/`pending_reg`/`pending_bit`；
- `UART`/`SPI0` 的 `clear_pending=false`。

### 6. 列定时器

- `counter`/`enable`/`overflow`/`irq`/`overflow_action`；
- `WDOG` 用 `up=false`、`overflow_action="reset"`。

### 7. 验证

```bash
./build/mcus --isa ./build/libmcus_isa_8051.so \
  --model models/<chip>.json --firmware blink.bin --config config/example.toml
./build/dump_shm models/<chip>.json /mcus_shm
ctest --test-dir build --output-on-failure
```

**`mcus` 启动时 `Model::validate()` 检查：区域不重叠、复位向量和初始 SP 在合法空间、寄存器宽度合法、同地址多语义有 alias、引脚编号唯一、中断向量和使能/挂起寄存器已定义、定时器寄存器已定义。**

## 11. 示例：最小模型

`models/dummy.json`：

```json
{
  "name": "dummy",
  "arch": "mcus-minimal",
  "crystal_hz": 16000000,
  "cycles_per_instruction": 4,
  "reset_vector": 0,
  "reset_space": "code",
  "initial_sp": 69628,
  "sp_space": "ram",
  "arch_desc": {
    "global_irq_enable": { "space": "mmio", "reg": 1073741844, "bit": 0, "present": true },
    "irq_push_bytes": 2,
    "fast_spaces": ["code", "ram"],
    "reset_space": "code",
    "reset_vector": 0,
    "sp_space": "ram",
    "initial_sp": 69628,
    "sp_reg": 69628,
    "sp_reg_space": "ram"
  },
  "regions": [
    { "name": "code", "space": "code", "base": 0, "size": 65536, "is_mmio": false },
    { "name": "ram",  "space": "ram",  "base": 65536, "size": 4096, "is_mmio": false },
    { "name": "mmio", "space": "mmio", "base": 1073741824, "size": 4096, "is_mmio": true }
  ],
  "registers": [
    { "name": "GPIO_OUT", "space": "mmio", "addr": 1073741824, "width": 32, "semantics": "write_trigger", "reset_value": 0 },
    { "name": "IRQ_ENABLE", "space": "mmio", "addr": 1073741844, "width": 32, "semantics": "plain", "reset_value": 0 },
    { "name": "IRQ_PENDING", "space": "mmio", "addr": 1073741848, "width": 32, "semantics": "plain", "reset_value": 0 }
  ],
  "interrupts": [
    { "name": "IRQ0", "number": 1, "vector_addr": 16, "vector_space": "code",
      "enable_reg": 1073741844, "enable_space": "mmio", "enable_bit": 0,
      "pending_reg": 1073741848, "pending_space": "mmio", "pending_bit": 0 }
  ],
  "timers": [
    { "name": "TIMER0",
      "counter": { "bits": 8, "lo_reg": 1073741836, "reg_space": "mmio", "reload_lo_reg": 1073741852 },
      "enable":   { "reg": 1073741832, "bit": 0 },
      "overflow": { "reg": 1073741840, "bit": 0 },
      "irq": 1 }
  ]
}
```

**`dummy.json` 是最小可运行模型，验证框架闭环和 `test_cpu_minimal`/`test_timer`。**

## 12. 常见问题

### `P1` 读 `0xFF`，`CPL` 不翻

**`P1` 的 `semantics` 必须是 `write_trigger`，`handle_mmio_read` 对 `write_trigger` 读 `shm_`，不是 `reset_value`。**

### `SBUF` 写后 `TI` 不置或延迟

**`uarts[].tx_cycles` 控制发送时间。写 `SBUF` 先发 `UartTx` 事件，`tx_cycles` 个虚拟周期后才置 `TI`；如果 `tx_cycles=0` 则立即置。不要再给 `SBUF` 配 `on_write: set_bit(TI)`，否则会绕过事件和时序。**

### `UART0`/`UART1`/`SPI0` 中断进了但 `jbc`/`jnb` 读到 0

**这些中断的 `clear_pending` 必须是 `false`：标志应由 ISR 自己清。`Cpu` 现在会屏蔽服务中的同号/同级中断，不会再出现“中断重入导致 ISR 不执行、SP 暴涨”的问题。**

### `WDOG` 不溢出复位

**`WDOG` 的 `counter.up=false`，`overflow_action="reset"`，`advance_clock` 的 `"reset"` 动作置 `reset_pin=1`，`check_reset_pin` 调 `reset()`。**

### 换芯片只换模型和 ISA

**`Cpu`/`Simulator` 不含芯片硬编码，换芯片只换 `models/*.json` 和 `isa_*.cpp`。** **`arch_desc` 驱动 `fast_spaces`/`global_irq_enable`/`irq_push_bytes`/`sp_reg`/`reset_space`。**

## 13. 参考

- `models/ch552g.json`：完整 CH552G 模型；
- `models/dummy.json`：最小模型；
- `models/schema.md`：字段说明；
- `include/mcus/model.h`：`Model`/`ArchDesc`/`RegisterDesc`/`PinDesc`/`InterruptDesc`/`TimerDesc`。
