# 性能基准

`mcus_bench` 测量三部分：

1. **CPU 指令吞吐**：用模型 + ISA + tight-loop 固件执行 N 条指令；
2. **事件流吞吐**：`EventStream` 的 push/pop 次数/秒；
3. **共享内存寄存器访问**：`SharedMemory` 的 read/write 次/秒。

## 构建

性能数字建议用 Release：

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j
```

Debug 构建（默认 `-O0`）的数字只适合相对比较，不代表模拟器上限。

## 运行

```bash
# 内置 mcus-minimal（dummy 模型）
./build-release/mcus_bench --model models/dummy.json

# 8051 / CH552G
./build-release/mcus_bench \
  --model models/ch552g.json \
  --isa ./build-release/libmcus_isa_8051.so

# ARM Cortex-M3 / STM32F103C8T6
./build-release/mcus_bench \
  --model models/stm32f103c8t6.json \
  --isa ./build-release/libmcus_isa_arm.so
```

常用参数：

| 参数 | 说明 |
|---|---|
| `--steps N` | 每次测量的指令数，默认 1,000,000 |
| `--warmup N` | 预热指令数，默认 10,000 |
| `--repeat N` | 重复次数，取最好成绩，默认 3 |
| `--firmware FILE` | 用指定固件替换默认 tight-loop |
| `--skip-events` | 不测事件流 |
| `--skip-shm` | 不测共享内存 |
| `--json` | 输出单行 JSON，便于 CI 收集 |

默认 tight-loop：

- `mcus-minimal`：`JMP 0`
- `8051`：`SJMP $`
- `arm`：`B .`，位于 Flash `reset_vector + 0x40`

## 指标

```text
model : stm32f103c8t6 (arm)
steps : 1000000 (warmup=10000, repeat=3)
cpu   : ... steps/s, ... virtual cycles/s, ... ns/step
events: ... events/s
shm   : ... register ops/s
```

- **steps/s**：每秒执行的指令数（MIPS 风格的粗略指标）；
- **virtual cycles/s**：每秒推进的虚拟晶振周期数，除以模型 `crystal_hz` 可估算“每真实秒推进多少虚拟秒”；
- **ns/step**：单条指令平均墙钟时间；
- **events/s**：事件流 push+pop 一对操作/秒；
- **shm ops/s**：共享内存寄存器 read+write 一对操作/秒。

## CTest 冒烟基准

`ctest -L bench` 会跑三条小规模基准：

- `bench_minimal`
- `bench_8051`
- `bench_arm`

它们只验证基准工具能跑通和防止严重性能回归，规模很小（20,000 步），不要把它们当成最终性能数字。

## 注意事项

- 首次 `dlopen` 和创建共享内存会计入进程启动成本，但不在 `steps` 计时范围内；
- `Cpu::step` 包含中断扫描、引脚采样和事件轮询，是完整单步路径；不是“裸取指”峰值；
- 事件流基准会创建独立共享内存对象，结束后 `shm_unlink`；
- 如果要测外设中断/总线事件吞吐，建议用真实固件配合 `--firmware`；bench 里 CPU 用的 256 槽事件流没有消费者，I/O 密集固件应改用事件流基准或单独写带消费者的测试。
