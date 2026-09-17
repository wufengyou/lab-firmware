/* USART2 @ PA2(TX)/PA3(RX)，115200 8N1。
 * 在 NUCLEO-F446RE 上 PA2/PA3 已橋到板載 ST-LINK 的 USB 虛擬 COM port，
 * 所以 PC 端開終端機（PuTTY / tio，115200）就直接看得到。 */
#pragma once
#include "stm32f446.h"

static inline void uart_init(void)
{
    RCC_AHB1ENR |= (1u << PORTA);
    RCC_APB1ENR |= (1u << 17);          /* USART2EN */

    gpio_mode(PORTA, 2, 2); gpio_af(PORTA, 2, 7);   /* PA2 -> AF7 USART2_TX */
    gpio_mode(PORTA, 3, 2); gpio_af(PORTA, 3, 7);   /* PA3 -> AF7 USART2_RX */

    USART2_BRR = 139;                   /* 16e6 / 115200 ≈ 138.9 */
    USART2_CR1 = (1u << 13) | (1u << 3) | (1u << 2);   /* UE | TE | RE */
}

static inline void uart_putc(char c)
{
    while (!(USART2_SR & (1u << 7)))    /* TXE */
        ;
    USART2_DR = (uint8_t)c;
}

static inline void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

/* 非阻塞讀一個字元；沒東西就回 -1。
 * cpu_ctrl 要「一邊輪詢 target 狀態、一邊等按鍵」，不能卡在阻塞讀上。 */
static inline int uart_getc_nb(void)
{
    if (!(USART2_SR & (1u << 5)))       /* RXNE */
        return -1;
    return (int)(USART2_DR & 0xFFu);
}

static inline void uart_puthex8(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t n = (v >> i) & 0xFu;
        uart_putc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
}

static inline void uart_puthex(uint32_t v)
{
    uart_puts("0x");
    uart_puthex8(v);
}
