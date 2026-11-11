# mcus 芯片模型文件 schema

模型文件的完整字段、约束和示例见 [docs/model_guide.md](../docs/model_guide.md)。

模型由 `arch_desc`、`regions`、`registers`、`pins`、`interrupts`、`timers` 以及外设描述
（`uarts`/`spis`/`pwms`/`adcs`/`tkeys`/`gpio_irq`/`usb`）组成；复位/栈/中断压栈等架构级参数
统一写在 `arch_desc` 中。可选顶层字段 `pin_poll_interval` 控制引脚全量采样间隔（默认 16），
外部 hold 的引脚和 `gpio_irq` 模型仍每步采样。
