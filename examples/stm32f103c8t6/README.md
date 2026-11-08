# STM32F103C8T6 示例

需要 `arm-none-eabi-gcc`。从仓库根目录构建模拟器和 ARM ISA：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

编译并运行：

```bash
cd examples/stm32f103c8t6
make
make run
```

`blink.bin` 链接在 `0x08000000`，通过 `GPIOC_BSRR` 翻转 PC13。
运行中用另一个终端观察：

```bash
./build/dump_shm models/stm32f103c8t6.json /mcus_shm
```
