/* 粗略 busy-loop 延時。HIRC 12 MHz + -Os 下，×10 約等於真 µs
 * （校正紀錄見 nano130/README.md 的 keypad 那段）。 */
#pragma once
#include <stdint.h>
#include "Nano100Series.h"

static inline void delay_us(uint32_t us)
{
    volatile uint32_t n = us * 10u;
    while (n--) __NOP();
}

static inline void delay_ms(uint32_t ms)
{
    while (ms--) delay_us(1000);
}
