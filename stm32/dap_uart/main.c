/*
 * stm32/dap_uart —— Stage 4a：CMSIS-DAP 指令層，但走 UART 而不是 USB
 *
 * 目的是**把 DAP 指令層單獨驗完**，完全不碰 USB。接線跟前面三支一樣（三條線）。
 *
 * 主機端是 host/dap_host.py：它把 CMSIS-DAP 命令包起來送進 COM 埠，
 * 收回應、解碼、印出來。等 Stage 4b 把 USB 接上之後，dap.h 一個字都不用改 ——
 * 換掉的只有這一層的「包怎麼送到」。
 *
 * ── 框架格式 ──
 *
 * UART 是位元組流，沒有封包邊界，所以要自己框：
 *
 *   主機 -> MCU:  0xA5  len  payload[len]  xor
 *   MCU  -> 主機:  0x5A  len  payload[len]  xor
 *
 * xor 是 len 與 payload 全部 XOR 起來。**加這個 byte 不是為了糾錯**（USB CDC
 * 這段路很可靠），而是為了在「框架跑掉」時能立刻發現 —— 不然症狀會變成
 * 「回應內容莫名其妙」，比直接報錯難查十倍。
 *
 * 收到不合法的框架就整包丟掉、回去等下一個 0xA5，不試圖修復。
 */
#include "stm32f446.h"
#include "delay.h"
#include "uart.h"
#include "swd.h"
#include "dap.h"

#define SOF_IN    0xA5u
#define SOF_OUT   0x5Au

static uint8_t req[DAP_PACKET_SIZE];
static uint8_t resp[DAP_PACKET_SIZE];

/* 阻塞式讀一個 byte。這一支從頭到尾只做一件事，不需要非阻塞。 */
static uint8_t get_byte(void)
{
    for (;;) {
        int c = uart_getc_nb();
        if (c >= 0)
            return (uint8_t)c;
    }
}

static void send_frame(const uint8_t *p, uint32_t n)
{
    uint8_t x = (uint8_t)n;

    uart_putc((char)SOF_OUT);
    uart_putc((char)n);
    for (uint32_t i = 0; i < n; i++) {
        uart_putc((char)p[i]);
        x ^= p[i];
    }
    uart_putc((char)x);
}

int main(void)
{
    dwt_init();
    uart_init();
    swd_pins_init();

    /* 開機不印任何文字 —— 這條 UART 現在是二進位通道，
     * 印歡迎訊息會被主機端當成框架垃圾。要看狀態請用 host 端的腳本。 */

    for (;;) {
        uint8_t len, x;
        uint32_t n;

        if (get_byte() != SOF_IN)
            continue;                        /* 重新找框頭 */

        len = get_byte();
        if (len == 0 || len > DAP_PACKET_SIZE)
            continue;

        x = len;
        for (uint32_t i = 0; i < len; i++) {
            req[i] = get_byte();
            x ^= req[i];
        }
        if (get_byte() != x)
            continue;                        /* 校驗不過：丟掉整包 */

        /* 未用到的部分清成 0。DAP 命令的長度是變動的，殘留上一包的資料
         * 會在解析越界時變成難以重現的怪現象。 */
        for (uint32_t i = len; i < DAP_PACKET_SIZE; i++)
            req[i] = 0;

        n = dap_process(req, resp);
        if (n > DAP_PACKET_SIZE)
            n = DAP_PACKET_SIZE;
        send_frame(resp, n);
    }
}
