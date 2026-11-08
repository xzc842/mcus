CROSS ?= arm-none-eabi-
CC    := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
MCUS  ?= ../../build/mcus
ISA   ?= ../../build/libmcus_isa_arm.so
MODEL ?= ../../models/stm32f103c8t6.json
CONFIG ?= ../../config/example.toml

all: blink.bin

blink.elf: blink.c link.ld
	$(CC) -mcpu=cortex-m3 -mthumb -O1 -ffreestanding -fno-builtin \
		-nostdlib -nostartfiles -T link.ld -o $@ blink.c

blink.bin: blink.elf
	$(OBJCOPY) -O binary $< $@

run: blink.bin
	$(MCUS) --isa $(ISA) --model $(MODEL) --firmware blink.bin --config $(CONFIG)

clean:
	rm -f blink.elf blink.bin

.PHONY: all run clean
