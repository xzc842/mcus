# STM32F103C8T6 模型与 ARM ISA

## 1. 文件

| 文件 | 作用 |
|---|---|
| `models/stm32f103c8t6.json` | STM32F103C8T6 芯片模型 |
| `src/isa_arm.cpp` | ARM Cortex-M3 Thumb/Thumb-2 ISA 插件 |
| `build/libmcus_isa_arm.so` | 构建后的 ISA 动态库 |
| `tests/test_arm_isa.cpp` | 复位向量/GPIO/BL/PUSH/POP 回归测试 |

## 2. 运行

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

./build/mcus \
  --isa ./build/libmcus_isa_arm.so \
  --model models/stm32f103c8t6.json \
  --firmware firmware.bin \
  --config config/example.toml
```

固件必须链接到 Flash 起始地址 `0x08000000`，向量表第一项是初始 SP，第二项是
`Reset_Handler|1`（Thumb 位）。裸 `.bin` 加载到 `0x08000000`；Intel HEX 也支持。

## 3. 地址空间

| 区域 | 地址 | 大小 | 说明 |
|---|---|---|---|
| `flash` | `0x08000000` | 64KB | 只读固件/向量表 |
| `sram` | `0x20000000` | 20KB | 栈/全局变量 |
| `periph` | `0x40000000` | 192KB | GPIO/USART/SPI/TIM/ADC/RCC/... |
| `system` | `0xE0000000` | 64KB | NVIC/SCB/SysTick/PRIMASK |

## 4. 已建模外设

- **GPIOA/B/C**：`CRL/CRH/IDR/ODR/BSRR/BRR/LCKR`；PA0–15、PB0–15、PC0–15 映射到 `PinState`。
  `ODR` 写会同步引脚；`BSRR/BRR` 通过模型 `bit_writes` 原子置位/清零 `ODR`；读 `IDR` 会叠加外部 `PinState.value`。
- **USART1/2/3**：`SR/DR/BRR/CR1/CR2/CR3/GTPR`。写 `DR` 发 `BusEvent(UartTx)`；外部
  `push_in(UartRx)` 写接收 FIFO、置 RXNE 并触发 NVIC pending。读 `DR` 清 RXNE。
  发送完成标志 TXE 由 `tx_cycles` 延迟置位。
- **SPI1/2**：`CR1/CR2/SR/DR/...`。写 `DR` 发 `BusEvent(SpiXfer)`；外部回
  `SpiXfer(ToMcu)` 后写回 MISO、置 RXNE 并触发 NVIC。读 `DR` 清 RXNE。
- **TIM2/3/4**：`CR1/DIER/SR/CNT/PSC/ARR/CCR1-4`。计数器按 `PSC` 分频，计到 `ARR` 产生
  UIF 和 NVIC 中断；`reload_compare=true` 表示按 `ARR`/`RVR` 重载。
- **ADC1**：`SR/CR1/CR2/SQR1-3/DR`。写 `CR2.SWSTART` 启动，`conversion_cycles` 后采样
  对应引脚 `PinState.value`，写 `DR`、置 `SR.EOC` 并触发 NVIC。
- **NVIC**：`ISER0/1`、`ICER0/1`、`ISPR0/1`、`ICPR0/1`、`IABR0/1`、`IPR0`。
  `ICER/ICPR` 的写 1 清零由 ISA 的 `on_mmio_write` 处理。
- **SysTick**：`CSR/RVR/CVR/CALIB`；`RVR` 重载、递减到 0 产生 SysTick 异常（向量 `0x3C`）。
- **SCB**：`CPUID/ICSR/VTOR/AIRCR/SCR/CCR/SHPRx/SHCSR/CFSR/HFSR/...` 作为寄存器建模。
- **I2C1/2、CAN1**：基于事件流的主机/帧收发（简化模型，见 [docs/interfaces.md](interfaces.md)）；RCC/FLASH/AFIO/EXTI/DMA 仍为寄存器骨架。

## 5. NVIC 与 PRIMASK

外部 IRQ 号与 STM32 一致（`WWDG=0` … `USBWakeUp=42`），模型里：

- `enable_reg` 指向 `NVIC_ISER0/1`；
- `pending_reg` 指向 `NVIC_ISPR0/1`；
- `vector_addr = 0x08000000 + 0x40 + 4*irq`；
- `clear_pending=true`，ISA 应答时写 `NVIC_ICPR` 清 pending。

`PRIMASK` 是 Cortex-M3 的特殊寄存器，不能直接内存映射；模型用
`system:0xE000EFF0` 的 `CPU_PRIMASK` 表示，`arch_desc.global_irq_enable.invert=true`
表示“位为 0 时开中断”。ISA 实现 `CPSID i`、`CPSIE i`、`MRS/MSR PRIMASK`。

SysTick 是异常 15，不在 NVIC ISER 中：模型用 `SYST_CSR.TICKINT` 作为使能、`SYST_CSR.COUNTFLAG`
作为 pending，向量 `0x0800003C`。

## 6. ARM ISA 覆盖范围

实现的是 ARMv7-M Thumb/Thumb-2 子集，覆盖启动、GPIO/USART/SPI/定时器中断和常规 C 代码：

- 16 位 Thumb：移位、ADD/SUB/MOV/CMP、数据处理、高寄存器、LDR/STR（字/字节/半字）、
  LDR literal、SP 相对、ADR、PUSH/POP、LDM/STM、条件跳转、B、BLX/BX、REV/REV16/REVSH；
- 32 位 Thumb-2：BL/B.W、MOVW/MOVT、LDR.W/STR.W（立即/寄存器/PC literal）、
  modified immediate 数据处理（MOV.W/ADD.W/SUB.W/ORR.W/EOR.W/CMP.W/TST.W 等）、
  AND/BIC/ORR/ORN/EOR/ADD/SUB/CMP 寄存器、STM/LDM/PUSH.W/POP.W、UMULL、MRS/MSR/CPS。

**未实现**：浮点/NEON、完整 IT 条件块（目前 IT 当作 NOP，后续条件指令会无条件执行）、
SVC/除零/总线错误等异常、完整 VTOR 重定位、所有 Thumb-2 指令。遇到未实现 opcode 会
`WARN` 并停机，便于定位。

## 7. 已知限制

- `USB/DMA` 只有寄存器/端点模型，没有完整协议或 DMA 引擎；
- I2C/CAN 使用简化事件流，不含地址阶段、仲裁、邮箱/过滤器；
- `EXTI` 和 GPIO 边沿中断未接入 NVIC（外部引脚变化仍产生 PinEvent，但不会自动触发 EXTI）；
- 正交编码器只使用 TIMx_CNT 计数，不模拟 TIM 编码器模式寄存器；
- `NVIC_IPR` 写入不改变静态优先级（模型里的 `priority` 字段决定）；
- `SCB_VTOR` 不参与向量计算（`interrupt_vector` 直接读模型 `vector_addr`）；
- 没有 RCC 时钟门控，外设访问不会因为未开时钟而失效；
- GPIO `CRL/CRH` 只作为寄存器保存，未用于方向/复用判断；ODR 写会更新输出引脚，IDR 读会叠加 `PinState`；
- `SysTick` 使用 `SYST_CSR` 的 bit16 作为 COUNTFLAG，读 CSR 清标志。

## 8. 示例固件

最小启动/闪灯：

```c
#include <stdint.h>
void Reset_Handler(void);
void Default_Handler(void) { while (1) {} }
__attribute__((section(".isr_vector")))
void (* const vectors[16])(void) = {
  (void (*)(void))0x20005000u, Reset_Handler, Default_Handler, Default_Handler,
  Default_Handler, Default_Handler, Default_Handler, Default_Handler,
  Default_Handler, Default_Handler, Default_Handler, Default_Handler,
  Default_Handler, Default_Handler, Default_Handler, Default_Handler
};
#define GPIOC_ODR (*(volatile uint32_t *)0x4001100Cu)
void main(void) {
  while (1) { GPIOC_ODR ^= (1u << 13); }
}
void Reset_Handler(void) { main(); while (1) {} }
```

链接脚本把 `.isr_vector` 放到 `0x08000000`，`.text` 从 `0x08000040` 开始：

```ld
ENTRY(Reset_Handler)
SECTIONS {
  . = 0x08000000;
  .isr_vector : { KEEP(*(.isr_vector)) }
  . = 0x08000040;
  .text : { *(.text*) *(.rodata*) }
}
```

编译：

```bash
arm-none-eabi-gcc -mcpu=cortex-m3 -mthumb -O1 -ffreestanding \
  -nostdlib -nostartfiles -T link.ld -o fw.elf fw.c
arm-none-eabi-objcopy -O binary fw.elf fw.bin
```
