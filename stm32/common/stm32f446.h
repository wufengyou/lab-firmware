/* STM32F446 —— 手刻的最小暫存器定義（只放這個骨架會用到的）。
 * 不用 ST 的 CMSIS device 標頭，自成一體、方便讀。晶片跑重置後預設的 HSI 16 MHz。 */
#pragma once
#include <stdint.h>

#define REG32(a)   (*(volatile uint32_t *)(a))

/* ---- RCC ---- */
#define RCC_AHB1ENR   REG32(0x40023830)   /* bit0 GPIOAEN, bit1 GPIOBEN, bit2 GPIOCEN */
#define RCC_APB1ENR   REG32(0x40023840)   /* bit17 USART2EN */

/* ---- GPIO（A=0x40020000，每個 port 間隔 0x400）---- */
#define GPIO_BASE(p)  (0x40020000u + (p) * 0x400u)
#define GPIOx_MODER(p) REG32(GPIO_BASE(p) + 0x00)
#define GPIOx_OTYPER(p) REG32(GPIO_BASE(p) + 0x04)
#define GPIOx_OSPEEDR(p) REG32(GPIO_BASE(p) + 0x08)
#define GPIOx_PUPDR(p) REG32(GPIO_BASE(p) + 0x0C)
#define GPIOx_IDR(p)  REG32(GPIO_BASE(p) + 0x10)
#define GPIOx_ODR(p)  REG32(GPIO_BASE(p) + 0x14)
#define GPIOx_BSRR(p) REG32(GPIO_BASE(p) + 0x18)
#define GPIOx_AFRL(p) REG32(GPIO_BASE(p) + 0x20)
#define GPIOx_AFRH(p) REG32(GPIO_BASE(p) + 0x24)

#define PORTA 0u
#define PORTB 1u
#define PORTC 2u

/* MODER 每腳 2 bit：00 input, 01 output, 10 alternate, 11 analog */
static inline void gpio_mode(uint32_t port, uint32_t pin, uint32_t m)
{
    GPIOx_MODER(port) = (GPIOx_MODER(port) & ~(3u << (pin * 2))) | (m << (pin * 2));
}
static inline void gpio_pull(uint32_t port, uint32_t pin, uint32_t p)   /* 0 none,1 up,2 down */
{
    GPIOx_PUPDR(port) = (GPIOx_PUPDR(port) & ~(3u << (pin * 2))) | (p << (pin * 2));
}
static inline void gpio_af(uint32_t port, uint32_t pin, uint32_t af)
{
    if (pin < 8)
        GPIOx_AFRL(port) = (GPIOx_AFRL(port) & ~(0xFu << (pin * 4))) | (af << (pin * 4));
    else
        GPIOx_AFRH(port) = (GPIOx_AFRH(port) & ~(0xFu << ((pin - 8) * 4))) | (af << ((pin - 8) * 4));
}
static inline void gpio_set(uint32_t port, uint32_t pin)  { GPIOx_BSRR(port) = (1u << pin); }
static inline void gpio_clr(uint32_t port, uint32_t pin)  { GPIOx_BSRR(port) = (1u << (pin + 16)); }
static inline int  gpio_get(uint32_t port, uint32_t pin)  { return (GPIOx_IDR(port) >> pin) & 1u; }

/* ---- USART2（0x40004400，APB1，重置後時脈 16 MHz）---- */
#define USART2_SR   REG32(0x40004400 + 0x00)
#define USART2_DR   REG32(0x40004400 + 0x04)
#define USART2_BRR  REG32(0x40004400 + 0x08)
#define USART2_CR1  REG32(0x40004400 + 0x0C)

/* ---- Cortex-M4 core debug / DWT（給 delay 用）---- */
#define CM_DEMCR    REG32(0xE000EDFC)     /* bit24 TRCENA */
#define CM_DWT_CTRL REG32(0xE0001000)     /* bit0 CYCCNTENA */
#define CM_DWT_CYCCNT REG32(0xE0001004)
