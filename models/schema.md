# mcus 芯片模型文件 schema

> 完整字段（`arch`、`arch_desc`、`timers`、寄存器 `on_write`/`on_read`、引脚方向、中断优先级、定时器预分频）见 [docs/model_guide.md](../docs/model_guide.md)。本文件只列早期核心字段。

模型文件为 JSON，描述一颗芯片的静态结构。运行时状态（寄存器当前值、引脚电平、中断挂起）不在模型文件里，而在共享内存中。

## 顶层字段

| 字段 | 类型 | 必填 | 说明 |
|---|---|---|---|
| name | string | 是 | 芯片模型名 |
| description | string | 否 | 说明 |
| arch | string | 是 | 架构名，决定加载哪个 ISA 动态库 |
| crystal_hz | number | 是 | 默认晶振频率（Hz），可被配置文件覆盖 |
| cycles_per_instruction | number | 是 | 每条指令粗略虚拟耗时（晶振周期） |
| reset_vector | number | 是 | 复位后 PC 值，必须落在某个非 MMIO 区域内 |
| initial_sp | number | 是 | 复位后 SP 初值，必须落在 RAM 内 |
| regions | array | 是 | 内存区域列表 |
| registers | array | 是 | MMIO 寄存器描述列表 |
| pins | array | 是 | 引脚列表 |
| interrupts | array | 是 | 中断源列表 |
| timers | array | 否 | 定时器列表 |
| arch_desc | object | 否 | 架构级描述（复位/栈/中断压栈/全局中断使能） |

## regions[]

| 字段 | 类型 | 说明 |
|---|---|---|
| name | string | 区域名 |
| base | number | 起始地址 |
| size | number | 字节数 |
| is_mmio | bool | true 表示 MMIO 区域，访问走共享内存和事件流；false 表示普通内存 |

约束：区域之间不得重叠；reset_vector 必须落在非 MMIO 区域且在该区域内；initial_sp 必须落在非 MMIO 区域。

## registers[]

| 字段 | 类型 | 说明 |
|---|---|---|
| name | string | 寄存器名 |
| addr | number | 地址，必须落在某个 is_mmio=true 的区域内 |
| width | number | 访问宽度，8 / 16 / 32 |
| readable | bool | 是否可读 |
| writable | bool | 是否可写 |
| write_triggers_event | bool | 写操作是否推一条 MmioEvent 到事件流 |
| reset_value | number | 复位值 |

约束：同一地址不得重复定义；width 必须是 8/16/32；寄存器地址范围不得越过其所属 MMIO 区域。

## pins[]

| 字段 | 类型 | 说明 |
|---|---|---|
| name | string | 引脚名 |
| number | number | 引脚编号，全局唯一 |
| functions | string[] | 复用功能列表，索引 0 为默认功能 |
| reset_function | number | 复位后的功能索引，必须小于 functions 长度 |

## interrupts[]

| 字段 | 类型 | 说明 |
|---|---|---|
| name | string | 中断源名 |
| number | number | 中断号，全局唯一 |
| vector_addr | number | 中断向量地址，必须落在非 MMIO 区域内 |
| enable_reg | number | 使能位所在寄存器地址，必须是已定义的 MMIO 寄存器 |
| enable_bit | number | 使能位在寄存器中的位偏移（0-31） |
| pending_reg | number | 挂起位所在寄存器地址，必须是已定义的 MMIO 寄存器 |
| pending_bit | number | 挂起位在寄存器中的位偏移（0-31） |

## 共享内存布局（由模型推导，不在模型文件中）

模拟器按以下顺序在共享内存中排布：

1. 控制区（ControlBlock）
2. MMIO 寄存器区（每个寄存器按 width 对齐，顺序按 registers[] 定义）
3. 引脚表（每个引脚一个 PinState，顺序按 pins[] 定义）
4. 中断请求位图（按 interrupts[] 数量向上取整到 32 位）
5. 注入区（外部程序写入待注入事件，格式见 README）

外部程序只通过共享内存和事件流与模拟器交互，不调用模拟器内部函数。