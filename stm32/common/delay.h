/* DWT CYCCNT 週期計數延時。HSI 16 MHz。呼叫前 dwt_init() 一次。 */
#pragma once
#include "stm32f446.h"

static inline void dwt_init(void)
{
    CM_DEMCR    |= (1u << 24);   /* TRCENA */
    CM_DWT_CYCCNT = 0;
    CM_DWT_CTRL |= 1u;           /* CYCCNTENA */
}

static inline void delay_us(uint32_t us)
{
    uint32_t start = CM_DWT_CYCCNT;
    uint32_t ticks = us * 16u;   /* 16 MHz */
    while ((CM_DWT_CYCCNT - start) < ticks)
        ;
}

static inline void delay_ms(uint32_t ms)
{
    while (ms--)
        delay_us(1000);
}
