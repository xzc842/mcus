#include <stdint.h>

void Reset_Handler(void);
void Default_Handler(void) { while (1) {} }

/* 至少覆盖 16 个 Cortex-M3 异常入口；外部 IRQ 可按需追加。 */
__attribute__((section(".isr_vector")))
void (* const vectors[16])(void) = {
    (void (*)(void))0x20005000u, Reset_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler,
    Default_Handler, Default_Handler, Default_Handler, Default_Handler
};

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018u)
#define GPIOC_ODR   (*(volatile uint32_t *)0x4001100Cu)
#define GPIOC_BSRR  (*(volatile uint32_t *)0x40011010u)

static void delay(volatile uint32_t n) { while (n--) {} }

int main(void) {
    RCC_APB2ENR |= (1u << 4); // GPIOC clock
    for (;;) {
        GPIOC_BSRR = (1u << 13);
        delay(200000);
        GPIOC_BSRR = (1u << (16 + 13));
        delay(200000);
    }
}

void Reset_Handler(void) {
    main();
    while (1) {}
}
