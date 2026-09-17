/* STM32F446 最小啟動碼：向量表 + Reset_Handler（複製 .data、清 .bss、開 FPU、跳 main）
 * 沒有 newlib crt0；ENTRY = Reset_Handler。不用中斷的話 16 個系統向量就夠。 */
#include <stdint.h>

extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss, _estack;
int main(void);

void Reset_Handler(void)
{
    uint32_t *src = &_sidata, *dst = &_sdata;
    while (dst < &_edata)
        *dst++ = *src++;
    for (dst = &_sbss; dst < &_ebss; )
        *dst++ = 0;

    /* CPACR：CP10/CP11 full access（M4F 用到浮點 ABI 前要開）*/
    *(volatile uint32_t *)0xE000ED88 |= (0xFu << 20);
    __asm volatile ("dsb; isb");

    main();
    for (;;)
        ;
}

void Default_Handler(void)
{
    for (;;)
        ;
}

#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))
void NMI_Handler(void)        WEAK_ALIAS;
void HardFault_Handler(void)  WEAK_ALIAS;
void MemManage_Handler(void)  WEAK_ALIAS;
void BusFault_Handler(void)   WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void)        WEAK_ALIAS;
void DebugMon_Handler(void)   WEAK_ALIAS;
void PendSV_Handler(void)     WEAK_ALIAS;
void SysTick_Handler(void)    WEAK_ALIAS;

__attribute__((section(".isr_vector"), used))
void (*const g_vectors[])(void) = {
    (void (*)(void)) & _estack,
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0, 0, 0, 0,
    SVC_Handler,
    DebugMon_Handler,
    0,
    PendSV_Handler,
    SysTick_Handler,
    /* 之後是外部 IRQ；沒開中斷就不需要 */
};
