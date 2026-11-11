# 外部程序与 mcus 的交互

> **说明**：事件流是双向 `DebugEvent` 流，记录类型为 `Mmio / Pin / Bus / Irq / Usb`；`Bus` 包含 UART/SPI/I2C/CAN/DAC/ADC/编码器/捕获事件；同一个共享内存对象里包含 out/in 两个 SPSC 环，`EventStream` 支持 `create=false` 的 attach 模式，以及 `push_in()`/`pop_in()` 反向收发。以 `include/mcus/types.h` 和 `include/mcus/event_stream.h` 为准。

外部程序不属于 mcus 项目，通过 POSIX 共享内存和事件流与模拟器交互。**外部程序不链接 `libmcus_core` 的模拟器部分，只通过共享内存和事件流读写；可以链接 `libmcus_core` 的 `SharedMemory`/`EventStream` 类简化访问。**

## 1. 总体架构

```
┌─────────────┐      POSIX shm       ┌──────────────┐
│   mcus      │ ───────────────────► │ 外部程序      │
│  (模拟器)    │      POSIX shm       │ (监控/外设)   │
│             │ ◄─────────────────── │              │
└─────────────┘                      └──────────────┘
       │
       │ 事件流 (环形队列)
       ▼
┌─────────────┐
│ 外部消费者   │
└─────────────┘
```

模拟器写共享内存和事件流，外部程序读；外部程序也可写控制区、注入区、引脚表、断点区，模拟器读。

## 2. 共享内存对象

- **名称**：配置文件中 `simulator.shm_name`，默认 `/mcus_shm`；
- **打开方式**：`shm_open(name, O_RDWR, 0600)` + `mmap`；
- **布局顺序**：控制区 → MMIO 寄存器区 → 引脚表 → 中断位图 → 注入区头 → 注入区槽 → dump 缓冲区 → 断点区头 → 断点槽；
- **各区域偏移由模型文件推导**，外部程序需加载同一模型文件，或用 `libmcus_core` 的 `SharedMemory` 类。

```c
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int fd = shm_open("/mcus_shm", O_RDWR, 0600);
struct stat st;
fstat(fd, &st);
void* base = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
close(fd);
```

**`st.st_size` 是共享内存总大小，布局由 `Model` 决定，外部程序不需要硬编码偏移。**

## 3. 控制区 `ControlBlock`

控制区在共享内存开头，`offset=0`，大小 `sizeof(ControlBlock)`：

```c
struct ControlBlock {
    atomic_uint32_t command;      // 外部写：控制命令
    atomic_uint32_t status;       // 模拟器写：运行状态
    atomic_uint32_t step_count;   // 外部写：单步步数
    atomic_uint32_t dump_addr;      // 外部写：dump 起始地址
    atomic_uint32_t dump_len;       // 外部写：请求长度；完成后写回实际长度
    atomic_uint32_t dump_space_idx; // 外部写：Model::space_order 索引；UINT32_MAX=自动
    atomic_uint32_t dump_ready;     // 模拟器写：dump 完成
    atomic_uint32_t reset_pin;    // 外部写：复位引脚
    atomic_uint64_t virtual_time; // 模拟器写：虚拟时间
    atomic_uint32_t pc;           // 模拟器写：当前 PC
    atomic_uint32_t halted;       // 模拟器写：停机标志
};
```

### 3.1 读状态

```c
struct ControlBlock* ctl = (struct ControlBlock*)base;
uint32_t pc = atomic_load(&ctl->pc);
uint64_t vt = atomic_load(&ctl->virtual_time);
uint32_t status = atomic_load(&ctl->status);
uint32_t halted = atomic_load(&ctl->halted);
```

### 3.2 写命令

```c
atomic_store(&ctl->command, 3);   // Step
atomic_store(&ctl->step_count, 1);
```

### 3.3 命令（`command`）

| 值 | 命令 | 说明 |
|---|---|---|
| 0 | None | 无 |
| 1 | Pause | 暂停 CPU |
| 2 | Resume | 继续 CPU |
| 3 | Step | 按 `step_count` 单步 |
| 4 | Dump | 按 `dump_addr`/`dump_len` dump，完成后 `dump_ready=1` |
| 5 | Reset | 复位 |

### 3.4 状态（`status`）

| 值 | 含义 |
|---|---|
| 0 | Running |
| 1 | Paused |
| 2 | DumpDone |
| 3 | Halted |

### 3.5 暂停/继续

```cpp
shm.control()->command.store(1);  // Pause
usleep(100000);
shm.control()->command.store(2);  // Resume
```

### 3.6 单步

```cpp
shm.control()->step_count.store(1);
shm.control()->command.store(3);  // Step
```

### 3.7 Dump

```cpp
shm.control()->dump_ready.store(0);
shm.control()->dump_space_idx.store(0);  // code 在 space_order 里的索引
shm.control()->dump_addr.store(0);
shm.control()->dump_len.store(256);
shm.control()->command.store(4);         // Dump
while (shm.control()->dump_ready.load() == 0) usleep(1000);
uint32_t actual = shm.control()->dump_len.load();
uint8_t* buf = shm.dump_buffer();        // actual 字节有效
```

### 3.8 复位

```cpp
shm.control()->reset_pin.store(1);
// 模拟器 check_reset_pin 看到 1，reset()，清 0
```

## 4. MMIO 寄存器区

MMIO 寄存器区在控制区之后，偏移 `mmio_offset_`，大小 `mmio_size_`。**外部程序不需要知道偏移，用 `SharedMemory` 的 `read_reg`/`write_reg`：**

```cpp
#include "mcus/model.h"
#include "mcus/shared_memory.h"

mcus::Model model = mcus::Model::load_from_file("models/ch552g.json");
mcus::SharedMemory shm(model, "/mcus_shm", false);  // create=false，只读

uint32_t p1 = shm.read_reg("sfr", 0x90, 8);
uint32_t status = shm.read_reg("sfr", 0x98, 8); // 地址以模型为准
uint32_t pc = shm.control()->pc.load();
```

**`create=false` 不 `memset`、不 `reset_all`，只读。**

**寄存器偏移计算：**

- 按 `Model::registers` 顺序，每个寄存器占 `align4((width+7)/8)` 字节；
- `reg_offset_fast_` 按 `(space_idx+1)*256 + addr` 索引，`space_idx` 是 `Model::space_index(space)`；
- `addr >= 256` 时走 `reg_offset_`（`unordered_map`）。

**外部程序链接 `libmcus_core`，用 `SharedMemory` 类，不用自己算偏移。**

## 5. 引脚表

引脚表在 MMIO 区之后，偏移 `pin_offset_`，每个引脚 `sizeof(PinState)`：

```c
struct PinState {
    atomic_uint32_t function;   // 复用功能编号
    atomic_uint32_t value;      // 电平 0/1
    atomic_uint32_t direction;  // 方向 0/1
};
```

**读 `P1.0` 引脚：**

```cpp
mcus::PinState* pins = shm.pins();
uint32_t p10_value = pins[0].value.load();   // P1.0 是 pins[0]
```

**引脚顺序按 `Model::pins`，`ch552g.json` 的 `P1.0`–`P1.7`、`P3.0`–`P3.7`、`RST`。**

## 6. 中断位图

中断位图在引脚表之后，偏移 `irq_offset_`，大小 `irq_bitmap_size()`：

```cpp
uint8_t* bm = shm.interrupt_bitmap();
size_t n = shm.interrupt_bitmap_size();
for (size_t i = 0; i < n; ++i) {
    for (int b = 0; b < 8; ++b) {
        if (bm[i] & (1 << b)) {
            uint32_t irq_num = i * 8 + b;
            // 中断 irq_num 挂起
        }
    }
}
```

**外部程序写 `bm[i] |= (1 << b)` 注入中断，`check_interrupt` 看到 `bp=true`，进中断。**

### 6.1 中断注入

外部程序有三种方式：

1. **写挂起寄存器**：`shm.write_reg(irq.pending_space, irq.pending_reg, pending | (1 << irq.pending_bit), width)`；
2. **写中断位图**：`bm[irq.number / 8] |= (1 << (irq.number % 8))`；
3. **写注入区**：`InjectSlot{addr, value, width, ready=1}`。

**模拟器在指令边界消费。**

## 7. 注入区

注入区在中断位图之后，`InjectHeader` + `InjectSlot` 数组：

```c
struct InjectHeader {
    atomic_uint32_t capacity;   // 槽数，默认 64
    atomic_uint32_t head;       // 模拟器写
    atomic_uint32_t tail;       // 外部写
};

struct InjectSlot {
    atomic_uint32_t addr;
    atomic_uint32_t value;
    atomic_uint32_t width;
    atomic_uint32_t ready;      // 外部写 1，模拟器消费后清 0
    atomic_uint32_t space_idx;  // MMIO 空间索引
};
```

**外部程序注入 MMIO 写：**

```cpp
auto* ih = shm.inject_header();
auto* slots = shm.inject_slots();
uint32_t tail = ih->tail.load();
uint32_t head = ih->head.load();
if (tail - head < ih->capacity.load()) {
    InjectSlot& s = slots[tail % ih->capacity.load()];
    s.addr.store(0x98);        // SCON
    s.value.store(0x50);       // 值
    s.width.store(8);
    s.space_idx.store(0);      // sfr 是第 0 个 is_mmio 空间
    s.ready.store(1);
    ih->tail.store(tail + 1);
}
```

**模拟器 `check_inject_slots` 消费，写 `shm_.write_reg`。**

## 8. dump 缓冲区

dump 缓冲区在注入区之后，`dump_buffer()`：

```cpp
uint8_t* buf = shm.dump_buffer();
size_t cap = shm.dump_buffer_size();
```

**`Dump` 命令后，`do_dump` 导出内存到 `buf`，`dump_ready=1`，外部程序读 `buf`。**

## 9. 断点区

断点区在 dump 缓冲区之后，`BreakpointHeader` + 32 个 `BreakpointSlot`：

```c
struct BreakpointHeader {
    atomic_uint32_t count;      // 断点数
    atomic_uint32_t sorted;     // 1=按 addr 升序
    atomic_uint32_t min_addr;   // 最小断点地址
    atomic_uint32_t max_addr;   // 最大断点地址
    atomic_uint32_t hit;        // 0=未命中，1=命中
    atomic_uint32_t hit_addr;   // 命中的地址
};

struct BreakpointSlot {
    atomic_uint32_t addr;       // 断点地址
    atomic_uint32_t enabled;    // 1=启用
};
```

**最多 32 个断点（`kBreakpointCapacity`）。**

### 9.1 设断点

```cpp
auto* bh = shm.breakpoint_header();
auto* slots = shm.breakpoints();

// 写断点，按 addr 升序
slots[0].addr.store(0x00D3); slots[0].enabled.store(1);
slots[1].addr.store(0x00E5); slots[1].enabled.store(1);

bh->count.store(2);
bh->sorted.store(1);
bh->min_addr.store(0x00D3);
bh->max_addr.store(0x00E5);

// 继续
shm.control()->command.store(2);  // Resume
```

**`sorted=1` 时 `check_breakpoint` 二分，`O(log n)`，32 断点 5 次比较。** **`min_addr`/`max_addr` 范围过滤，`pc` 不在范围直接返回。**

### 9.2 等断点

```cpp
while (bh->hit.load() == 0) usleep(100);
uint32_t addr = bh->hit_addr.load();
printf("breakpoint hit at 0x%X, pc=0x%X\n", addr, shm.control()->pc.load());

// 读寄存器
uint32_t p1 = shm.read_reg("sfr", 0x90, 8);

// 单步
shm.control()->step_count.store(1);
shm.control()->command.store(3);  // Step

// 继续
shm.control()->command.store(2);  // Resume
```

### 9.3 清断点

```cpp
slots[0].enabled.store(0);   // 禁用断点
bh->count.store(0);          // 或清空所有断点
```

### 9.4 断点语义

- **`check_breakpoint` 在 `Cpu::step` 开头，`execute` 之前**；
- **`pc` 命中，`paused_=true`，`step` 返回 0，`execute` 不执行，`pc` 是断点地址；**
- **`Resume`/`Step` 清 `hit`，`last_bp_pc_` 跳过当前断点，`execute` 执行，`pc` 变，`check_breakpoint` 查，`pc` 不是断点，继续；**
- **断点命中时 `pc` 精确，`P1` 是断点前的值。**

### 9.5 查看断点

`dump_shm` 输出：

```
=== breakpoints ===
count=2 sorted=1 min=0xD3 max=0xE5 hit=0 hit_addr=0x0
bp[0] addr=0xD3 enabled=1
bp[1] addr=0xE5 enabled=1
```

## 10. 事件流（双向）

事件流是独立共享内存，`config.toml` 的 `simulator.stream_name`（默认 `/mcus_stream`），`stream_capacity`（默认 4096）。

同一个共享内存对象里有两个独立的 SPSC 环：

- **out 环**：模拟器 → 外部程序。MMIO / Pin / Bus 请求 / Irq / USB 事件。
- **in 环**：外部程序 → 模拟器。UART RX / SPI MISO / USB SETUP/IN/OUT。
- 两个环共用一个全局 `seq` 分配器，`seq` 越大事件越新。

### 10.1 布局

```
Header → out[capacity] → in[capacity]      (每个元素都是 DebugEvent)
```

```c
struct Header {
    atomic_uint64_t out_head;      // 模拟器写
    atomic_uint64_t out_tail;      // 外部程序读
    atomic_uint64_t in_head;       // 外部程序写
    atomic_uint64_t in_tail;       // 模拟器读
    atomic_uint64_t capacity;      // 每个环的容量
    atomic_uint64_t seq;           // 全局事件序号
    atomic_uint32_t backpressure_out;
    atomic_uint32_t backpressure_in;
    atomic_uint32_t version;       // 3
    atomic_uint32_t host_attached; // attach 或 push_in 时置 1
};
```

`DebugEvent` 的完整定义见 `include/mcus/types.h`：

```c
struct DebugEvent {
    uint64_t seq;
    uint64_t time;
    uint8_t  type;      // 0=Mmio 1=Pin 2=Bus 3=Irq 4=Usb
    uint8_t  reserved[7];
    union {
        MmioEvent mmio;
        PinEvent  pin;
        BusEvent  bus;
        IrqEvent  irq;
        UsbEvent  usb;
    } u;
};
```

### 10.2 打开与消费

建议直接链接 `libmcus_core` 使用 `EventStream` 类（`create=false`）：

```cpp
#include "mcus/event_stream.h"

// 第二个参数在 create=false 时只用于校验，实际容量从共享内存头读取
mcus::EventStream stream("/mcus_stream", 0, false);

while (1) {
    mcus::DebugEvent ev;
    if (!stream.pop(ev)) {           // 从 out 环取
        usleep(1000);
        continue;
    }
    if (ev.type == mcus::DebugEventType::Bus) {
        const mcus::BusEvent& b = ev.u.bus;
        // b.bus_id / b.xfer_id / b.kind / b.direction / b.data
    }
}
```

**建议的启动顺序：** 配置 `start_paused = true` 启动 `mcus` → 外部程序 attach 事件流和共享内存 →
外部程序写 `command=Resume` → 固件开始执行。这样 SPI 第一次传输时 `host_attached` 已经为 1，
模拟器会等待外部 MISO 响应，而不是用默认 `0xFF` 抢跑。

模拟器只在 `host_attached=1` 时轮询 in 环；`attach()` 和 `push_in()` 都会置位，因此外部程序
只要正常写回总线响应/接收数据，不需要额外设置。

`type=Irq` 的 `IrqEvent{number, pending}` 在每次进入中断时写入 out 环；`type=Mmio/Pin` 分别记录寄存器访问和引脚变化。

### 10.3 总线请求/响应

`BusEvent` 用于 UART/SPI/I2C：

```c
enum BusEventKind { SpiXfer=0, I2cStart=1, I2cAddr=2, I2cData=3,
                    I2cStop=4, UartTx=5, UartRx=6, PinEdge=7 };
enum BusDirection { ToHost=0, ToMcu=1 };

struct BusEvent {
    uint32_t bus_id;      // 同类外设的索引：uarts[i] / spis[i]
    uint32_t xfer_id;     // 请求/响应配对
    uint16_t len;
    uint8_t  kind;
    uint8_t  direction;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[64];
};
```

- **UART TX**：模拟器写 `SBUF` 后发 `UartTx(ToHost)`。外部程序 `pop()` 收到即可；不需要回复。
- **UART RX**：外部程序 `push_in(UartRx(ToMcu), bus_id=0)`，模拟器写入接收 FIFO、置 RI。
- **SPI**：模拟器写 `SPI0_DATA` 后发 `SpiXfer(ToHost, xfer_id=N)`；如果已有外部程序
  attach 事件流，模拟器会**冻结 CPU/虚拟时间等待** `SpiXfer(ToMcu, xfer_id=N, data[0]=MISO)`，
  收到后写回 `SPI0_DATA` 并置 SPIF。等待超过 `simulator.bus_wait_timeout_ms` 或没有外部程序时，
  MISO 默认为 `0xFF` 并继续执行。

示例：

```cpp
mcus::DebugEvent req;
if (stream.pop(req) && req.type == mcus::DebugEventType::Bus &&
    req.u.bus.kind == (uint8_t)mcus::BusEventKind::SpiXfer) {
    mcus::DebugEvent resp{};
    resp.type = mcus::DebugEventType::Bus;
    resp.u.bus = req.u.bus;
    resp.u.bus.direction = (uint8_t)mcus::BusDirection::ToMcu;
    resp.u.bus.data[0] = 0x3C;
    stream.push_in(resp);
}
```

### 10.4 I2C / CAN / DAC / ADC / 编码器 / GPIO 波形

这些接口都使用 `DebugEventType::Bus`，详细字段、事件格式和模型示例见
[docs/interfaces.md](interfaces.md)。

- `I2cStart/I2cData/I2cStop`
- `CanFrame`
- `DacWrite`
- `AdcSample`
- `EncoderDelta`
- `PwmCapture`

### 10.5 USB 包事件

USB 使用 `DebugEventType::Usb`，`UsbEvent` 的 `data[64]` 正好是 Full-Speed 最大包：

```c
enum UsbToken { Setup=0, In=1, Out=2, Reset=3, Suspend=4, Resume=5, EpReady=6 };
enum UsbHandshake { None=0, Ack=1, Nak=2, Stall=3 };

struct UsbEvent {
    uint32_t xfer_id;
    uint16_t len;
    uint8_t  ep;
    uint8_t  token;
    uint8_t  handshake;
    uint8_t  reserved[3];
    uint8_t  data[64];
};
```

外部主机程序的角色：

- 发 `UsbEvent{token=Reset}`：清空端点缓冲、`USB_DEV_AD`。
- 发 `token=Setup`、`ep=0`：8 字节 SETUP 包，模拟器写入 EP0 缓冲、置 `USB_INT_ST` 的
  SETUP token 和 `USB_INT_FG.UIF_TRANSFER`。
- 发 `token=Out`：OUT 数据，模拟器写入端点缓冲、设置 `USB_RX_LEN`、置 `USB_INT_ST` 的 OUT
  token、`UIF_TRANSFER` 和 `U_TOG_OK`。
- 发 `token=In`、`len=N`：模拟器从端点 TX 区读取实际长度（由 `len_reg` 决定），回一个
  `token=In, handshake=Ack` 的 `UsbEvent` 到 out 环；取走后 mcus 清零 TX 长度。
- 没有新 TX 长度时，`token=In` 返回 0 字节，不重复发送上一包。
- 模拟器在固件写 `UEPn_CTRL` 时发 `token=EpReady`，外部主机可据此安排 IN/OUT。

端点缓冲地址由模型显式描述：

```text
RX: usb.buffer_space : usb.buffer_base + endpoint.buffer_offset
TX: 再加 endpoint.tx_buffer_offset
```

`buffer_offset` 缺省时回退到 `index * buffer_stride`。`ch552g.json` 使用实际固件布局：

```text
EP0 = xram 0
EP1 = xram 10
EP2 = xram 20
EP2 TX = xram 84
```

因此不能假设所有端点等距 64 字节。

### 10.6 背压

- **out 满**：模拟器置 `backpressure_out=1`，`cpu_loop` 暂停 CPU，直到外部程序 `pop()`。
- **in 满**：`push_in()` 返回 `false` 并置 `backpressure_in=1`，外部程序应重试。
- 模拟器侧 `EventStream::full()`/`in_full()` 可查询状态。

### 10.7 全局序号

`push()` 和 `push_in()` 都会从共享内存头的 `seq` 原子取号，因此 out/in 两个方向的事件
可以直接按 `seq` 排序，不需要额外的关联字段；总线请求/响应仍用 `xfer_id` 配对。

## 11. 日志

日志由 `config.toml` 的 `debug.log_level`/`debug.log_output` 控制：

- `log_level`：`trace`/`debug`/`info`/`warn`/`error`；
- `log_output`：`stderr` 或文件路径。

**日志格式：**

```
[LEVEL] file:line message
```

**外部程序不读日志，日志是模拟器调试用。** **要看模拟器状态，用共享内存。**

## 12. `dump_shm` 工具

```bash
./build/dump_shm models/ch552g.json /mcus_shm
```

**输出：**

```
=== control ===
command     = 0
status      = 0
step_count  = 1
virtual_time= 145257
pc          = 0x00B8
halted      = 0
reset_pin   = 0

=== registers ===
P1               [sfr] @0x00000090 width=8 value=0xFF
...

=== pins ===
P1.0     func=0 value=0 dir=0
...

=== interrupt bitmap ===
00 00 ...

=== breakpoints ===
count=0 sorted=0 min=0x0 max=0x0 hit=0 hit_addr=0x0
```

**`dump_shm` 以 `create=false` 打开，不重置模拟器。**

## 13. 完整示例：读 `P1` 和 `pc`

```cpp
#include "mcus/model.h"
#include "mcus/shared_memory.h"
#include <cstdio>

int main() {
    mcus::Model model = mcus::Model::load_from_file("models/ch552g.json");
    mcus::SharedMemory shm(model, "/mcus_shm", false);  // 只读

    for (int i = 0; i < 10; ++i) {
        uint32_t p1 = shm.read_reg("sfr", 0x90, 8);
        uint32_t pc = shm.control()->pc.load();
        uint64_t vt = shm.control()->virtual_time.load();
        std::printf("P1=0x%02X pc=0x%04X virtual_time=%llu\n",
                    p1, pc, (unsigned long long)vt);
        usleep(100000);
    }
    return 0;
}
```

**编译：**

```bash
g++ -std=c++17 -I include monitor.cpp -L build -lmcus_core -o monitor
./monitor
```

## 14. 完整示例：设断点、等命中、读寄存器

```cpp
#include "mcus/model.h"
#include "mcus/shared_memory.h"
#include <cstdio>
#include <unistd.h>

int main() {
    mcus::Model model = mcus::Model::load_from_file("models/ch552g.json");
    mcus::SharedMemory shm(model, "/mcus_shm", false);

    auto* bh = shm.breakpoint_header();
    auto* slots = shm.breakpoints();
    slots[0].addr.store(0x00D3);
    slots[0].enabled.store(1);
    bh->count.store(1);
    bh->sorted.store(1);
    bh->min_addr.store(0x00D3);
    bh->max_addr.store(0x00D3);

    shm.control()->command.store(2);   // Resume

    while (bh->hit.load() == 0) usleep(100);
    uint32_t pc = shm.control()->pc.load();
    uint32_t p1 = shm.read_reg("sfr", 0x90, 8);
    printf("breakpoint hit at 0x%X, pc=0x%X, P1=0x%02X\n",
           bh->hit_addr.load(), pc, p1);

    shm.control()->command.store(2);   // Resume
    return 0;
}
```

## 15. 兼容性说明

- 共享内存的 `ControlBlock`、`PinState`、`Inject*`、`Breakpoint*`、事件流 `Header` 和 `DebugEvent` 布局未变，直接 mmap 的外部程序不需要改布局；
- 使用 C++ `SharedMemory`/`EventStream` 类的外部程序需要按当前头文件重新编译（类新增了私有缓存成员，类大小变了）；
- `EventStream::push_in()` 会置 `host_attached`；模拟器只在 `host_attached=1` 时轮询 in 环。直接 mmap 写 in 环的外部程序必须自己把 `Header.host_attached` 置 1；
- 无 `gpio_irq` 的模型引脚采样间隔为 16 步，外部引脚注入最多延迟 16 条指令。

## 16. 注意事项

1. **`SharedMemory` 构造函数 `create=true` 会 `memset` + `reset_all`，外部程序必须 `create=false`，否则重置模拟器状态；**
2. **`dump_shm` 使用 `create=false`，外部程序参考 `tools/dump_shm.cpp`；**
3. **`atomic` 操作跨进程，外部程序用 `std::atomic` 的 `load`/`store`，或者 `__atomic_load_n`/`__atomic_store_n`；**
4. **事件流有 out/in 两个环：外部程序 `pop()` 消费 out、`push_in()` 生产 in；两个环的 `head`/`tail` 都是 `uint64_t`，满时分别置 `backpressure_out`/`backpressure_in`；**
5. **中断位图 `bm[i] |= (1 << b)`，外部程序写，模拟器 `check_interrupt` 读，`enter_interrupt` 清；**
6. **引脚表 `value`/`function`/`direction` 是 `atomic`，外部程序可写 `value` 注入电平，模拟器端口读会把 `value` 叠加回寄存器；`direction=0` 时输入引脚变化还会触发 GPIO 边沿中断；**
7. **没有 `gpio_irq` 的模型默认每 16 步采样一次引脚，外部输入变化最多延迟 16 条指令；需要逐指令 GPIO 边沿时请在模型中配置 `gpio_irq`；**
8. **断点区 `bh->count`/`sorted`/`min_addr`/`max_addr` 写完后，`hit=0`，`Resume`，模拟器 `check_breakpoint` 二分查；**
9. **断点命中 `hit=1`，`Resume`/`Step` 清 `hit`，`last_bp_pc_` 跳过当前断点；**
10. **SPI 是同步总线：外部程序 attach 事件流后，模拟器会冻结 CPU 等待 `SpiXfer(ToMcu)` 响应；建议 `start_paused=true`，attach 后先 `Resume`，避免第一次传输用默认 MISO 抢跑；**
11. **`host_attached` 是粘滞标志，进程退出后不会自动清零；长时间无响应由 `simulator.bus_wait_timeout_ms` 兜底。**

## 17. 参考

- `include/mcus/shared_memory.h`：`ControlBlock`/`PinState`/`InjectSlot`/`BreakpointHeader`/`BreakpointSlot`/`SharedMemory`；
- `include/mcus/event_stream.h`：双向 `Header`/`EventStream`；
- `include/mcus/types.h`：`MmioEvent`/`PinEvent`/`BusEvent`/`UsbEvent`/`RegSemantics`/`ControlCommand`/`ControlStatus`；
- `tools/dump_shm.cpp`：`dump_shm` 完整实现；
- `docs/isa.md`：如何编写 ISA；
- `docs/config.md`：如何编写配置文件。