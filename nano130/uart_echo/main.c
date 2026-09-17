/*
 * nano130/uart_echo —— N2 Stage a：確認 UART0 這條 payload 通道
 *
 * 燒在 master(C3)。**完全不碰 target**，連 swd.h 都沒 include。
 *
 * 這一階真正在測的是三件事，而且刻意讓它們分得開：
 *
 *   1. `J8/J9` 的 USB-UART 在這台機器上列舉成哪個 COM 埠
 *   2. **RX 方向**（PC → 板）—— 收到的 byte 數與最後一個 byte 直接顯示在 LCD 上，
 *      **不靠回傳**。所以就算 TX 方向是壞的，RX 也能單獨判定。
 *   3. **TX 方向**（板 → PC）—— 開機送一行招呼，之後每個收到的 byte 原樣回送
 *
 * 分得開這件事是設計，不是順手：UART 只有兩條線，一次判兩個方向的話，
 * 「終端機沒反應」會同時有四種解釋（TX 壞、RX 壞、埠錯、鮑率錯）。
 * LCD 把 RX 獨立出來之後，剩下的解釋只有一種。
 *
 * LCD：
 *   第一行  UART0 ECHO
 *   第二行  n:XXXX  c:XX  ch     n=累計收到幾 byte、c=最後一個 byte、ch=可列印字元
 */
#include "Nano100Series.h"
#include "delay.h"
#include "lcd1602.h"
#include "uart0.h"

#define BAUD  115200u

/* 自己拆十六進位，不用 lcd_puthex/lcd_puthex8 —— 那兩支分別是 10 格和 8 格，
 * 湊不進一行 16 格。這正是今天早上 flash_wr 的 "prep err:" 踩過的同一個坑：
 * LCD1602 一行只有 16 格，超出的字會被推到 DDRAM 的螢幕外區域，靜靜消失。 */
static void put_nib(uint8_t v)
{
    lcd_putc("0123456789ABCDEF"[v & 0xF]);
}

static void show(uint32_t n, uint8_t c)
{
    lcd_goto(1, 0);
    lcd_puts("n:");
    put_nib((uint8_t)(n >> 12)); put_nib((uint8_t)(n >> 8));
    put_nib((uint8_t)(n >> 4));  put_nib((uint8_t)n);      /* 4 格，夠看 */
    lcd_puts(" c:");
    put_nib(c >> 4); put_nib(c);
    lcd_putc(' ');
    lcd_putc((c >= 0x20 && c < 0x7F) ? (char)c : '.');
    lcd_puts("  ");                                        /* 共 16 格 */
}

int main(void)
{
    SYS_UnlockReg();
    CLK_EnableModuleClock(GPIO_MODULE);
    SYS_LockReg();

    lcd_init();
    uart0_init(BAUD);

    lcd_goto(0, 0);
    lcd_puts("UART0 ECHO      ");
    show(0, 0);

    /* 開機招呼：PC 端看得到這行，就代表 TX 方向、鮑率、COM 埠三者都對。
     * 看不到但 LCD 的計數會動 —— 那就只剩 TX 那條線有問題。 */
    uart0_puts("\r\nnano130 uart_echo 115200 8N1 ready\r\n");

    uint32_t n = 0;

    for (;;) {
        if (uart0_rx_ready()) {
            uint8_t c = (uint8_t)UART0->RBR;
            n++;
            uart0_putc(c);                     /* 原樣回送 */
            show(n, c);
        }
    }
}
