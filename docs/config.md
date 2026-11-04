# 如何编写配置文件

`mcus` 的配置文件是 TOML，`main.cpp` 读 `argv[3]`，`Config::load_from_file` 解析。本文说明字段含义和写法。

## 1. 启动

```bash
./build/mcus \
  --isa ./build/libmcus_isa_8051.so \
  --model models/ch552g.json \
  --firmware blink.bin \
  --config config/example.toml
```

四个参数：ISA 动态库、模型文件、固件（`.bin` 或 `.hex`/`.ihx`）、配置文件。配置文件决定共享内存名、事件流、晶振、日志。

## 2. 完整示例

```toml
[simulator]
# 共享内存对象名（POSIX shm_open，需以 / 开头）
shm_name = "/mcus_shm"

# 事件流对象名
stream_name = "/mcus_stream"

# 事件流环形队列容量（条数）。满时触发背压，CPU 暂停推进。
stream_capacity = 4096

# 启动后是否立即暂停，等待外部程序就绪
start_paused = false

# 控制区命令轮询间隔（虚拟时间片数），仅用于降低 CPU 空转
control_poll_interval = 64

# 每 N 步调 advance_clock（定时器推进）
# 1 = 真芯片语义，定时器每步计数，中断延迟 0 步，性能最低
# 100 = 定时器 100 步量化，中断延迟 0–99 步，性能最高
clock_step_interval = 100

[crystal]
# 虚拟晶振频率（Hz）。所有虚拟时间以此为基准，不跟真实时间对齐。
frequency_hz = 16000000

# 每条指令粗略虚拟耗时（虚拟周期数）
cycles_per_instruction = 4

[debug]
# 日志级别：trace / debug / info / warn / error
log_level = "info"

# 日志输出：stderr 或文件路径
log_output = "stderr"

# dump 文件前缀，Dump 命令落文件时用
dump_prefix = "/tmp/mcus_dump"
```

## 3. 字段说明

### 3.1 `[simulator]`

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `shm_name` | string | `/mcus_shm` | 共享内存名，POSIX `shm_open`，需以 `/` 开头 |
| `stream_name` | string | `/mcus_stream` | 事件流名，POSIX `shm_open`，需以 `/` 开头 |
| `stream_capacity` | int | `4096` | 事件流环形队列容量（条数）。满时 `cpu_loop` `yield`，背压暂停 CPU |
| `start_paused` | bool | `false` | 启动后是否立即暂停，`true` 时 `status=Paused`，等外部程序 `Resume` |
| `control_poll_interval` | int | `64` | 每 N 步 `handle_control_commands`，降低 CPU 空转 |
| `clock_step_interval` | int | `100` | 每 N 步 `advance_clock`，见下文 |

**`shm_name`/`stream_name` 必须唯一，多个 `mcus` 实例同时跑时不能冲突。** **`debug.toml` 用 `/mcus_shm_debug`/`/mcus_stream_debug`，和 `example.toml` 的 `/mcus_shm`/`/mcus_stream` 区分。**

### 3.2 `[crystal]`

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `frequency_hz` | int | `16000000` | 虚拟晶振频率（Hz）。`virtual_time` 以此为基准，不跟真实时间对齐 |
| `cycles_per_instruction` | int | `4` | 每条指令粗略虚拟耗时（虚拟周期数）。`Cpu::step` 返回 `model_.cycles_per_instruction`，`virtual_time_ += dt` |

**`frequency_hz` 覆盖模型文件的 `crystal_hz`，`cycles_per_instruction` 覆盖模型的 `cycles_per_instruction`。** **`Config::apply_to_model` 在 `main.cpp` 调，`Simulator` 构造前。**

### 3.3 `[debug]`

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `log_level` | string | `info` | `trace`/`debug`/`info`/`warn`/`error`。`trace` 最详细，`error` 只错误 |
| `log_output` | string | `stderr` | `stderr` 或文件路径（如 `/tmp/mcus.log`）。文件用 `std::ofstream` 追加 |
| `dump_prefix` | string | `/tmp/mcus_dump` | `Dump` 命令落文件时，文件名 `<prefix>_<addr>.bin` |

**`log_level=debug` 时 `MCUS_DEBUG` 打印，`step pc`/`enter interrupt` 等调试日志出现。** **`log_level=info` 时 `MCUS_DEBUG` 不打印，日志干净。**

## 4. `clock_step_interval` 怎么选

| 值 | `virtual_time` 速度 | 定时器精度 | 中断延迟 | 适用 |
|---|---|---|---|---|
| `1` | 约 2.2 万步/秒 | 每步计数 | 0 步 | 定时器/中断固件，真芯片语义 |
| `100` | 约 35 万步/秒 | 100 步量化 | 0–99 步 | `blink.bin`、GPIO、寄存器测试 |

**`example.toml` 用 `100`，`debug.toml` 用 `1`。** **定时器固件（`timer_test`/`timer1_test`/`timer2_test`）用 `debug.toml` 的 `1`，`P1` 翻转准确。** **`blink.bin` 用 `example.toml` 的 `100`，`virtual_time` 快。**

## 5. 多个配置

| 文件 | `shm_name` | `log_level` | `clock_step_interval` | 用途 |
|---|---|---|---|---|
| `config/example.toml` | `/mcus_shm` | `info` | `100` | 常规运行 |
| `config/debug.toml` | `/mcus_shm_debug` | `debug` | `1` | 调试，真芯片语义 |

**`debug.toml` 的 `shm_name` 是 `/mcus_shm_debug`，`dump_shm` 读 `/mcus_shm_debug`。** **`example.toml` 的 `shm_name` 是 `/mcus_shm`，`dump_shm` 读 `/mcus_shm`。** **两个实例同时跑，互不冲突。**

## 6. 解析

`src/config.cpp` 的 `Config::load_from_file`：

```cpp
Config Config::load_from_file(const std::string& path) {
    Config cfg;
    toml::value data;
    try {
        data = toml::parse(path);
    } catch (const std::exception& e) {
        throw std::runtime_error("cannot parse config: " + std::string(e.what()));
    }

    {
        toml::value sim = get_section(data, "simulator");
        cfg.simulator.shm_name = toml::find_or<std::string>(
            sim, "shm_name", cfg.simulator.shm_name);
        ...
        cfg.simulator.clock_step_interval = static_cast<uint32_t>(
            toml::find_or<int64_t>(sim, "clock_step_interval",
                static_cast<int64_t>(cfg.simulator.clock_step_interval)));
        if (cfg.simulator.clock_step_interval == 0)
            cfg.simulator.clock_step_interval = 1;
    }
    ...
}
```

**`toml::find_or` 缺字段用默认值，`get_section` 缺 `[simulator]`/`[crystal]`/`[debug]` 段返回空 `toml::table`，全用默认。** **`clock_step_interval=0` 当 `1`，避免除零。**

## 7. 常见错误

- **`shm_name` 不以 `/` 开头**：`shm_open` 失败，`SharedMemory` 构造抛 `shm_open failed`；
- **`shm_name` 冲突**：两个 `mcus` 实例用同一个 `/mcus_shm`，`mmap` 同一块内存，状态互相覆盖；
- **`stream_capacity=0`**：`EventStream` 构造 `capacity=1`，队列只有 1 条，`push` 很快满，背压频繁；
- **`log_level` 拼错**：`parse_log_level` 返回 `Info`，`debug`/`trace` 日志不打印；
- **`log_output` 路径不可写**：`log_set_output` 失败，回退 `stderr`；
- **`clock_step_interval=0`**：`Config` 当 `1`，`advance_clock` 每步调，性能低；
- **`frequency_hz=0`**：`apply_to_model` 不覆盖，模型 `crystal_hz` 生效；
- **`cycles_per_instruction=0`**：`apply_to_model` 不覆盖，模型 `cycles_per_instruction` 生效。

## 8. 参考

- `config/example.toml`：常规运行；
- `config/debug.toml`：调试；
- `include/mcus/config.h`：`SimulatorSection`/`CrystalSection`/`DebugSection`/`Config`；
- `src/config.cpp`：`load_from_file`/`apply_to_model`；
- `docs/external_contract.md`：共享内存和事件流契约。
