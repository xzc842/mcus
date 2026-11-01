# mcus 外部程序接口文档

本文档说明外部程序如何通过共享内存、事件流、日志和 `dump_shm` 工具读取模拟器的状态和输出。**外部程序不链接 `libmcus_core`，只通过 POSIX 共享内存和事件流交互。**

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

模拟器写共享内存和事件流，外部程序读；外部程序也可写控制区、注入区、引脚表，模拟器读。

## 2. 共享内存

### 2.1 打开共享内存

共享内存名由 `config.toml` 的 `simulator.shm_name` 指定（默认 `/mcus_shm`）。外部程序：

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

**`st.st_size` 是共享内存总大小，布局由 `Model` 决定，外部程序不需要硬编码偏移，用下面的 API。**

### 2.2 控制区 `ControlBlock`

控制区在共享内存开头，`offset=0`，大小 `sizeof(ControlBlock)`：

```c
struct ControlBlock {
    atomic_uint32_t command;      // 外部写：控制命令
    atomic_uint32_t status;       // 模拟器写：运行状态
    atomic_uint32_t step_count;   // 外部写：单步步数
    atomic_uint32_t dump_addr;    // 外部写：dump 起始地址
    atomic_uint32_t dump_len;     // 外部写：dump 长度
    atomic_uint32_t dump_ready;   // 模拟器写：dump 完成
    atomic_uint32_t reset_pin;    // 外部写：复位引脚
    atomic_uint64_t virtual_time; // 模拟器写：虚拟时间
    atomic_uint32_t pc;           // 模拟器写：当前 PC
    atomic_uint32_t halted;       // 模拟器写：停机标志
};
```

**读 `pc`/`virtual_time`/`status`/`halted`：**

```c
struct ControlBlock* ctl = (struct ControlBlock*)base;
uint32_t pc = atomic_load(&ctl->pc);
uint64_t vt = atomic_load(&ctl->virtual_time);
uint32_t status = atomic_load(&ctl->status);
uint32_t halted = atomic_load(&ctl->halted);
```

**写 `command`/`step_count`/`dump_addr`/`dump_len`/`reset_pin`：**

```c
atomic_store(&ctl->command, 3);   // Step
atomic_store(&ctl->step_count, 1);
```

**`status` 取值：**

| 值 | 含义 |
|---|---|
| 0 | Running |
| 1 | Paused |
| 2 | DumpDone |
| 3 | Halted |

**`command` 取值：**

| 值 | 含义 |
|---|---|
| 0 | None |
| 1 | Pause |
| 2 | Resume |
| 3 | Step |
| 4 | Dump |
| 5 | Reset |

### 2.3 MMIO 寄存器区

MMIO 寄存器区在控制区之后，偏移 `mmio_offset_`，大小 `mmio_size_`。**外部程序不需要知道偏移，用 `read_reg`/`write_reg` 的等价逻辑：**

**`read_reg(space, addr, width)` 读寄存器：**

```c
uint32_t read_reg(const char* space, uint32_t addr, uint8_t width);
```

**`write_reg(space, addr, value, width)` 写寄存器：**

```c
void write_reg(const char* space, uint32_t addr, uint32_t value, uint8_t width);
```

**这些函数在 `libmcus_core` 的 `SharedMemory` 类里，外部程序可以链接 `libmcus_core` 直接用，或者按下面的布局自己实现。**

**寄存器偏移计算：**

- 按 `Model::registers` 顺序，每个寄存器占 `align4((width+7)/8)` 字节；
- `reg_offset_fast_` 按 `(space_idx+1)*256 + addr` 索引，`space_idx` 是 `Model::space_index(space)`；
- `addr >= 256` 时走 `reg_offset_`（`unordered_map`），外部程序不常用。

**简化：外部程序链接 `libmcus_core`，用 `SharedMemory` 类：**

```cpp
#include "mcus/model.h"
#include "mcus/shared_memory.h"

mcus::Model model = mcus::Model::load_from_file("models/ch552g.json");
mcus::SharedMemory shm(model, "/mcus_shm", false);  // create=false，只读

uint32_t p1 = shm.read_reg("sfr", 0x90, 8);
uint32_t scon = shm.read_reg("sfr", 0x98, 8);
uint32_t pc = shm.control()->pc.load();
```

**`create=false` 不 `memset`、不 `reset_all`，只读。**

### 2.4 引脚表

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

### 2.5 中断位图

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

### 2.6 注入区

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

### 2.7 dump 缓冲区

dump 缓冲区在注入区之后，`dump_buffer()`：

```cpp
uint8_t* buf = shm.dump_buffer();
size_t cap = shm.dump_buffer_size();
```

**`Dump` 命令后，`do_dump` 导出内存到 `buf`，`dump_ready=1`，外部程序读 `buf`。**

## 3. 事件流

事件流是独立共享内存，`config.toml` 的 `simulator.stream_name`（默认 `/mcus_stream`），`stream_capacity`（默认 4096）。

### 3.1 布局

```
Header (head/tail/capacity/backpressure) → MmioEvent 数组
```

```c
struct Header {
    atomic_uint64_t head;         // 模拟器写
    atomic_uint64_t tail;         // 外部写
    atomic_uint64_t capacity;
    atomic_uint32_t backpressure; // 模拟器写，满时置 1
};

struct MmioEvent {
    uint64_t time;
    uint32_t addr;
    uint32_t value;
    uint8_t width;
    uint8_t access;   // 0=Read, 1=Write
    uint32_t space_idx;
};
```

### 3.2 打开事件流

```c
int fd = shm_open("/mcus_stream", O_RDWR, 0600);
struct stat st;
fstat(fd, &st);
void* base = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
close(fd);

struct Header* hdr = (struct Header*)base;
struct MmioEvent* slots = (struct MmioEvent*)((char*)base + sizeof(struct Header));
```

### 3.3 消费事件

```c
while (1) {
    uint64_t tail = atomic_load(&hdr->tail);
    uint64_t head = atomic_load(&hdr->head);
    if (tail == head) {
        // 空，等
        usleep(1000);
        continue;
    }
    uint64_t cap = atomic_load(&hdr->capacity);
    struct MmioEvent ev = slots[tail % cap];
    atomic_store(&hdr->tail, tail + 1);
    // 处理 ev
    printf("time=%llu addr=0x%X value=0x%X width=%u access=%u space=%u\n",
           ev.time, ev.addr, ev.value, ev.width, ev.access, ev.space_idx);
}
```

**`space_idx` 是 `Model::space_order` 的索引，`ch552g.json` 的 `space_order` 是 `["code", "iram", "sfr", "xram"]`，`sfr` 是 `2`。**

### 3.4 背压

**`stream_.full()` 时 `hdr->backpressure=1`，`cpu_loop` `yield`，CPU 暂停。** **外部程序消费后 `hdr->backpressure` 由模拟器 `clear_backpressure` 清，或者外部程序 `clear_backpressure`。**

## 4. 日志

日志由 `config.toml` 的 `debug.log_level`/`debug.log_output` 控制：

- `log_level`：`trace`/`debug`/`info`/`warn`/`error`；
- `log_output`：`stderr` 或文件路径。

**日志格式：**

```
[LEVEL] file:line message
```

**外部程序不读日志，日志是模拟器调试用。** **要看模拟器状态，用共享内存。**

## 5. `dump_shm` 工具

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
P3               [sfr] @0x000000B0 width=8 value=0xFF
...
=== pins ===
P1.0     func=0 value=0 dir=0
...
=== interrupt bitmap ===
00 00 ...
```

**`dump_shm` 以 `create=false` 打开，不重置模拟器。**

## 6. 完整示例：读 `P1` 和 `pc`

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

## 7. 完整示例：消费事件流

```cpp
#include "mcus/model.h"
#include "mcus/event_stream.h"
#include <cstdio>

int main() {
    mcus::Model model = mcus::Model::load_from_file("models/ch552g.json");
    mcus::EventStream stream("/mcus_stream", 4096);  // 打开已有事件流

    while (1) {
        mcus::MmioEvent ev;
        if (!stream.pop(ev)) {
            usleep(1000);
            continue;
        }
        std::printf("time=%llu addr=0x%X value=0x%X width=%u access=%u space=%u\n",
                    (unsigned long long)ev.time, ev.addr, ev.value,
                    ev.width, static_cast<int>(ev.access), ev.space_idx);
    }
    return 0;
}
```

**`EventStream` 构造函数 `shm_open` + `mmap`，`pop` 消费。**

## 8. 外部程序写控制区

### 8.1 暂停/继续

```cpp
shm.control()->command.store(1);  // Pause
usleep(100000);
shm.control()->command.store(2);  // Resume
```

### 8.2 单步

```cpp
shm.control()->step_count.store(1);
shm.control()->command.store(3);  // Step
```

### 8.3 Dump

```cpp
shm.control()->dump_addr.store(0);
shm.control()->dump_len.store(256);
shm.control()->command.store(4);  // Dump
// 等 dump_ready
while (shm.control()->dump_ready.load() == 0) usleep(1000);
uint8_t* buf = shm.dump_buffer();
```

### 8.4 复位

```cpp
shm.control()->reset_pin.store(1);
// 模拟器 check_reset_pin 看到 1，reset()，清 0
```

### 8.5 注入中断

```cpp
uint8_t* bm = shm.interrupt_bitmap();
bm[0] |= (1 << 0);   // INT0 挂起
```

### 8.6 注入 MMIO 写

```cpp
auto* ih = shm.inject_header();
auto* slots = shm.inject_slots();
uint32_t tail = ih->tail.load();
uint32_t cap = ih->capacity.load();
InjectSlot& s = slots[tail % cap];
s.addr.store(0x98);
s.value.store(0x50);
s.width.store(8);
s.space_idx.store(0);
s.ready.store(1);
ih->tail.store(tail + 1);
```

## 9. 注意事项

1. **`SharedMemory` 构造函数 `create=true` 会 `memset` + `reset_all`，外部程序必须 `create=false`，否则重置模拟器状态；**
2. **`dump_shm` 已用 `create=false`，外部程序参考 `tools/dump_shm.cpp`；**
3. **`atomic` 操作跨进程，外部程序用 `std::atomic` 的 `load`/`store`，或者 `__atomic_load_n`/`__atomic_store_n`；**
4. **事件流 `head`/`tail` 是 `uint64_t`，外部程序 `tail` 递增，模拟器 `head` 递增，满时 `backpressure=1`；**
5. **中断位图 `bm[i] |= (1 << b)`，外部程序写，模拟器 `check_interrupt` 读，`enter_interrupt` 清；**
6. **引脚表 `value`/`function`/`direction` 是 `atomic`，外部程序读，模拟器 `handle_mmio_write` 写。**

## 10. 参考

- `include/mcus/shared_memory.h`：`ControlBlock`/`PinState`/`InjectSlot`/`SharedMemory`；
- `include/mcus/event_stream.h`：`Header`/`EventStream`；
- `include/mcus/types.h`：`MmioEvent`/`RegSemantics`/`ControlCommand`/`ControlStatus`；
- `tools/dump_shm.cpp`：`dump_shm` 完整实现；
- `docs/external_contract.md`：共享内存布局契约。

---
