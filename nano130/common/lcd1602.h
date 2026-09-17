/* WENG NANO 客製版的 LCD1602（HD44780，8-bit 並列）header-only driver
 *
 *   DB0..DB7 = PD.0 .. PD.7      RS = PC.15   RW = PC.14（固定拉低=只寫）   E = PB.15
 *
 * 用法：lcd_init() 一次，然後 lcd_clear / lcd_goto / lcd_puts / lcd_putc / lcd_puthex。
 * 需要先 CLK_EnableModuleClock(GPIO_MODULE)。
 */
#pragma once
#include "Nano100Series.h"
#include "delay.h"

#define LCD_RS_H()   (PC->DOUT |=  (1u << 15))
#define LCD_RS_L()   (PC->DOUT &= ~(1u << 15))
#define LCD_E_H()    (PB->DOUT |=  (1u << 15))
#define LCD_E_L()    (PB->DOUT &= ~(1u << 15))

static inline void lcd_bus(uint8_t v)
{
    PD->DOUT = (PD->DOUT & 0xFF00u) | v;
}

static inline void lcd_strobe(void)
{
    LCD_E_H();
    delay_us(2);
    LCD_E_L();
    delay_us(50);
}

static inline void lcd_cmd(uint8_t c)
{
    LCD_RS_L();
    lcd_bus(c);
    lcd_strobe();
}

static inline void lcd_putc(char d)
{
    LCD_RS_H();
    lcd_bus((uint8_t)d);
    lcd_strobe();
}

static inline void lcd_goto(uint32_t row, uint32_t col)
{
    lcd_cmd(0x80u | (col + (row ? 0x40u : 0x00u)));
}

static inline void lcd_puts(const char *s)
{
    while (*s)
        lcd_putc(*s++);
}

static inline void lcd_puthex8(uint32_t v)        /* 8 個 16 進位字，無前綴 */
{
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t nib = (v >> i) & 0xFu;
        lcd_putc(nib < 10 ? (char)('0' + nib) : (char)('A' + nib - 10));
    }
}

static inline void lcd_puthex(uint32_t v)         /* 0xXXXXXXXX */
{
    lcd_putc('0');
    lcd_putc('x');
    lcd_puthex8(v);
}

static inline void lcd_clear(void)
{
    lcd_cmd(0x01);
    delay_ms(2);
}

/* 只設 LCD 用到的腳為推挽輸出。呼叫前要先 CLK_EnableModuleClock(GPIO_MODULE)。 */
static inline void lcd_init(void)
{
    GPIO_SetMode(PD, 0x00FFu, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PC, 0xC000u, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PB, 0x8000u, GPIO_PMD_OUTPUT);
    PC->DOUT &= ~(1u << 14);        /* RW 固定拉低 */
    LCD_E_L();

    delay_ms(50);
    lcd_cmd(0x30); delay_ms(5);
    lcd_cmd(0x30); delay_us(200);
    lcd_cmd(0x30); delay_us(200);
    lcd_cmd(0x38);                  /* 8-bit, 2 line, 5x8 */
    lcd_cmd(0x08);                  /* display off */
    lcd_clear();
    lcd_cmd(0x06);                  /* entry: 遞增、不捲動 */
    lcd_cmd(0x0C);                  /* display on, cursor off */
}
