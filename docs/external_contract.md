# 外部程序与 mcus 的共享内存契约

外部程序不属于 mcus 项目，通过 POSIX 共享内存和事件流与模拟器交互。

## 共享内存对象

- 名称：配置文件中 `simulator.shm_name`，默认 `/mcus_shm`
- 打开方式：`shm_open(name, O_RDWR, 0600)` + `mmap`
- 布局顺序：控制区 → MMIO 寄存器区 → 引脚表 → 中断位图 → 注入区头 → 注入区槽 → dump 缓冲区
- 各区域偏移由模型文件推导，外部程序需加载同一模型文件或读取 mcus 导出的布局描述

## 事件流对象

- 名称：配置文件中 `simulator.stream_name`，默认 `/mcus_stream`
- 布局：Header（head/tail/capacity/backpressure）+ MmioEvent 数组
- 模拟器 push，外部程序 pop；pop 到空返回 false；外部程序消费慢导致满时模拟器置 backpressure 并暂停

## 控制区命令

| 值 | 命令 | 说明 |
|---|---|---|
| 1 | Pause | 暂停 CPU |
| 2 | Resume | 继续 |
| 3 | Step | 按 step_count 单步 |
| 4 | Dump | 按 dump_addr/dump_len dump，完成后 dump_ready=1 |
| 5 | Reset | 复位 |

## 中断注入

外部程序有三种方式：
1. 写 `IRQ_PENDING` 寄存器对应位
2. 写中断位图对应位
3. 往注入区写 `InjectSlot{addr, value, width, ready=1}`

模拟器在指令边界消费。

## 引脚

`PinState{function, value, direction}`，外部程序读写引脚电平、复用功能。