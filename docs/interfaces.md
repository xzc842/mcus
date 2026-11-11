# 数据流接口与引脚复用

mcus 的所有外部接口都通过共享内存事件流（`DebugEvent`）与外部程序交互。芯片模型只描述
寄存器、引脚和时序；协议行为由 `Cpu` + `BusEvent` 完成。

## 1. 已支持的接口

| 接口 | 数据流 | 方向 | 说明 |
|---|---|---|---|
| UART | `BusEvent(UartTx/UartRx)` | 双向 | 已有接口，按字节收发 |
| SPI | `BusEvent(SpiXfer)` | 双向 | 主机写 DR 发请求，外部回 MISO |
| I2C | `BusEvent(I2cStart/I2cData/I2cStop)` | 双向 | 简化主机，支持 start/stop/数据回读 |
| CAN | `BusEvent(CanFrame)` | 双向 | 标准/扩展帧，payload 最多 8 字节 |
| USB | `UsbEvent` | 双向 | Setup/In/Out/Reset，已有接口 |
| ADC | `BusEvent(AdcSample)` 或 `PinState` | 输入 | 可注入模拟采样值，未注入时读引脚 |
| DAC | `BusEvent(DacWrite)` | 输出 | 写数据寄存器产生转换值事件 |
| PWM | `PinEvent` + PWM 寄存器 | 输出 | 已有 PWM 波形输出 |
| 正交编码器 | `PinEvent` / `BusEvent(EncoderDelta)` | 输入 | A/B 相解码并更新计数 |
| GPIO 脉宽输入 | `PinEvent.pulse_width` / `PwmCapture` | 输入 | 所有 GPIO 可直接读脉宽；`captures[]` 可写寄存器/触发中断 |
| 外部输入波宽 | `BusEvent(PulseWidth)` | 外部 → MCU | 向任意 GPIO 注入指定宽度的脉冲 |
| GPIO 波形输出 | `PinEvent` | 输出 | 按 period/high 产生方波 |

`BusEventKind` 定义在 `include/mcus/types.h`；事件流布局和 attach 见
[docs/external.md](external.md)。

## 2. 事件数据约定

`BusEvent` 的公共字段：

- `bus_id`：控制器索引（`i2cs/cans/...` 数组下标）；
- `xfer_id`：一次传输的序号，便于请求/响应配对；
- `flags`：接口相关标志；
- `data[64]`：内联数据。

### I2C

- `I2cStart` / `I2cStop`：`len=0`；写 `ctrl.start_bit` / `stop_bit` 时产生；
- `I2cData(ToHost)`：`data[0]` 是写出的字节，`bus_id` 是控制器；
- `I2cData(ToMcu)`：外部回 ACK/读数据，`data[0]` 写回 `data` 寄存器；
- 当前不建模地址阶段、时钟拉伸和仲裁；`I2cAddr` 保留。

### CAN

- `CanFrame(ToHost)`：`data[0..3]` 为 32 位 ID（小端），`data[4..]` 为 payload；
- `len = 4 + dlc`，`dlc <= 8`；`flags` bit0 表示扩展帧；
- 高 4 字节 payload 使用模型 `data_hi` 寄存器；
- `CanFrame(ToMcu)` 反向收帧，写回 `id/data/data_hi/dlc` 并置完成；
- 当前不建模邮箱、过滤器、位 timing 和错误帧。

### DAC

- `DacWrite(ToHost)`：`data[0..3]` 为 32 位转换值（低 `bits` 位有效），`flags` 为通道号；
- 这是数字事件，不生成真实模拟电压。

### ADC

- `AdcSample(ToMcu)`：`bus_id` 为 ADC 索引，`flags` 为通道号，`data[0..3]` 为采样值；
- 转换完成时优先使用最近一次注入值，没有注入才读取通道引脚电平。

### 正交编码器

- 外部写 A/B 两相 `PinState.value` 时，核心解码四相状态并更新 `count` 寄存器；
- 每次有效跳变发 `EncoderDelta(ToHost)`，`data[0..3]` 为有符号增量；
- 外部也可发 `EncoderDelta(ToMcu)` 直接注入增量；
- `reverse` 反转方向，`step` 为每步倍率。

### GPIO 脉宽输入

- `PwmCapture(ToHost)`：`data[0..3]` 为脉冲宽度（虚拟周期），`bus_id` 为 capture 索引；
- `measure_high=true` 测高电平，`false` 测低电平；
- 宽度写入模型 `width` 寄存器，可配置 `irq`。

### GPIO 脉宽（所有引脚）

所有 GPIO 的 `PinEvent` 都带脉宽信息，不需要在模型里为每个引脚写 `captures`：

```c
struct PinEvent {
    uint32_t pin_id;
    uint32_t value;
    uint32_t function;
    uint32_t direction;
    uint32_t changed;       // bit3=1 表示 pulse_width 有效
    uint32_t pulse_width;   // 上一个电平持续的虚拟周期数
};
```

- 上升沿（`value=1`）报告上一个低电平宽度；
- 下降沿（`value=0`）报告上一个高电平宽度；
- 第一次边沿没有前一条边，`changed.bit3=0`；
- 所有 GPIO 每步采样，因此窄脉冲也能被记录；
- 需要写寄存器/触发中断时再配置 `captures[]`；只读取脉宽直接用 `PinEvent`。

### 外部输入波宽

外部程序可以直接向任意 GPIO 注入一个指定宽度的脉冲，不需要自己按虚拟时间翻转引脚：

```cpp
BusEvent b{};
b.bus_id = pin_index;              // Model::pins 下标
b.kind = static_cast<uint8_t>(BusEventKind::PulseWidth);
b.direction = static_cast<uint8_t>(BusDirection::ToMcu);
b.flags = 1;                       // bit0=1 高脉冲，0 低脉冲
b.len = 4;
uint32_t width = 70;               // 虚拟周期
std::memcpy(b.data, &width, 4);
// 放进 in 环
```

模拟器会：

1. 在当前虚拟时间把引脚驱动到脉冲起始电平；
2. 经过 `width` 个虚拟周期后驱动到相反电平；
3. 通过 `PinEvent.pulse_width` 和 `captures[]` 的 `PwmCapture` 给出测量结果。

`PulseWidth` 的 `bus_id` 是引脚号，不是 capture 索引；同一引脚连续输入多个脉冲会排队执行。
这与直接写 `PinState.value` 等价，但宽度由事件参数精确指定，不需要外部程序自己计时。

### 启动时的引脚电平

- 8051/CH552/STC 的准双向端口复位为 `0xFF`，悬空时读高是正常的（弱上拉）；
- STM32 的 GPIO 复位为浮空输入，IDR 不应假设固定为高或低；
- 外部要主动驱动一个低/高电平，建议：

```cpp
// 仿真器用 start_paused=true 启动
shm.set_pin_value(pin_index, 0);  // 拉低并置 hold 位
shm.control()->command.store(static_cast<uint32_t>(ControlCommand::Resume));
```

只写 `PinState.value` 而不通过 `set_pin_value()` 置 hold 位时，CPU 端口写可能把值覆盖回端口锁存器，
固件随后读到的就是锁存器电平。

### GPIO 波形输出

- 写 `gpio_waves[].ctrl` 的 `start_bit` 启动，`stop_bit` 停止；
- 读取 `period`/`high` 寄存器，输出方波到 `pin`；
- `repeat=0` 连续输出，大于 0 时输出指定个数高脉冲；
- 波形通过 `PinEvent` 反映到引脚表。

## 3. 模型字段

### 引脚复用

在 `pins[]` 中增加：

```json
{
  "name": "PA2",
  "functions": ["gpio", "usart2_tx", "tim2_ch3"],
  "reset_function": 0,
  "mux_space": "periph",
  "mux_reg": 1073763328,
  "mux_shift": 8,
  "mux_width": 2
}
```

`mux_reg` 的 `[mux_shift, mux_shift+mux_width)` 字段值就是 `functions` 的下标。
写该寄存器时核心更新 `PinState.function` 并发 `PinEvent(changed=2)`。
实际芯片的 AF 寄存器、PxSW 或方向复用寄存器按真实地址填写；没有复用寄存器的芯片可以
只写 `functions`，由外部程序或外设直接设置功能。

### 外设数组

```json
"i2cs": [
  { "name": "I2C0", "irq": 20, "space": "periph", "ctrl": 0x40005400,
    "data": 0x40005410, "stat": 0x40005414,
    "start_bit": 8, "stop_bit": 9, "ack_bit": 10, "done_bit": 2,
    "if_bit": 1, "busy_bit": 0, "scl_pin": "PB6", "sda_pin": "PB7" }
],
"cans": [
  { "name": "CAN0", "irq": 19, "space": "periph", "id": 0x40006580,
    "data": 0x40006584, "data_hi": 0x40006588, "dlc": 0x4000658C,
    "ctrl": 0x40006400, "stat": 0x40006404,
    "done_bit": 0, "busy_bit": 1, "if_bit": 2,
    "tx_pin": "PA12", "rx_pin": "PA11" }
],
"dacs": [
  { "name": "DAC0", "space": "periph", "ctrl": 0x40007400,
    "data": 0x40007408, "channel": 0, "bits": 12, "vref_mv": 3300 }
],
"encoders": [
  { "name": "ENC0", "space": "periph", "count": 0x40000024,
    "ctrl": 0x40000014, "reset_bit": 0, "bits": 16,
    "a_pin": "PA0", "b_pin": "PA1", "step": 1, "reverse": false }
],
"captures": [
  { "name": "CAP0", "space": "periph", "width": 0x40000034,
    "pin": "PA2", "measure_high": true, "irq": 0 }
],
"gpio_waves": [
  { "name": "WAVE0", "space": "periph", "ctrl": 0x40000000,
    "period": 0x40000004, "high": 0x40000008,
    "start_bit": 0, "stop_bit": 1, "pin": "PA3", "repeat": 0 }
]
```

字段含义见 [docs/model_guide.md](model_guide.md)。所有 `*_pin` 可写引脚名或 `pins` 下标。

### ADC 数据流

```json
"adcs": [
  { "name": "ADC0", "space": "periph", "ctrl": 0x40012400,
    "cfg": 0x40012404, "data": 0x4001244C, "status": 0x40012400,
    "channels": [ { "channel": 0, "pin": "PA0" } ] }
]
```

外部程序可通过 `push_in(AdcSample)` 注入任意通道采样值，适合接传感器模型。

## 4. 外部程序示例

```cpp
mcus::DebugEvent ev{};
ev.type = mcus::DebugEventType::Bus;

// I2C 从机返回 ACK + 数据
ev.u.bus.bus_id = 0;
ev.u.bus.kind = static_cast<uint8_t>(mcus::BusEventKind::I2cData);
ev.u.bus.direction = static_cast<uint8_t>(mcus::BusDirection::ToMcu);
ev.u.bus.len = 1;
ev.u.bus.data[0] = 0x5A;
stream.push_in(ev);

// CAN 收帧：ID + payload
ev.u.bus.kind = static_cast<uint8_t>(mcus::BusEventKind::CanFrame);
ev.u.bus.len = 6;
uint32_t id = 0x123;
std::memcpy(ev.u.bus.data, &id, 4);
ev.u.bus.data[4] = 0x11;
ev.u.bus.data[5] = 0x22;
stream.push_in(ev);
```

完整可运行示例见 `tests/test_interfaces.cpp` 和 `models/interface_test.json`。

## 5. 限制

- I2C 是简化主机，不模拟地址阶段/仲裁/时钟拉伸；
- CAN 只提供单帧收发，不含邮箱、过滤器和总线错误；
- DAC 只输出数字事件，不产生模拟电压；
- 编码器按 A/B 四相跳变解码，不模拟机械抖动；
- 所有 GPIO 的脉宽单位为虚拟周期；`PinEvent` 结构增加了 `pulse_width` 字段，外部程序需要重新编译；
- GPIO 波形输出是简单方波，不模拟死区、互补输出和任意波形；
- 所有接口的数据都必须通过事件流，外部程序不要直接改外设内部状态。
