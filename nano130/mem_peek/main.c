/*
 * nano130/mem_peek —— Stage 2：用客製版當 debugger，透過 SWD MEM-AP 讀 target 記憶體
 *
 * master（燒這支）= 客製版 + keypad + LCD + common/swd.h 的 bit-bang SWD。
 * target = 另一塊客製版，J4 接 master 的 J17（見 common/swd.h 檔頭）。
 *
 * 操作（master 的 keypad —— 只有 0-9 A-D，沒有 E/F）：
 *   0-9 A-D  輸入 16 進位位址（往左移入，滿 8 位就從高位溢出）
 *   *        位址歸零
 *   # 讀 target 該位址的 32-bit 字；連按 # → 位址自動 +4 往上走
 *
 *   * 之後接 A/B/C/D  = 跳到 preset（這幾個都在 0xE000... 手打不出來）：
 *     *A  0xE000ED00  SCB CPUID   -> 0x410CC200 (ARM Cortex-M0 r0p0)
 *     *B  0xE000EDF0  DHCSR       （Stage 3 halt CPU 會用）
 *     *C  0xE0001000  DWT
 *     *D  0xE00FF000  ROM table   （晶片自報有哪些 debug 元件）
 *
 * LCD：第一行 A:XXXXXXXX   第二行 D:XXXXXXXX 或 D:FAULT / D:ACK n / D:NO TARGET
 *
 * 手打得出來的驗證位址：
 *   0x00000000  target flash 第一個字 = 它現在跑的韌體初始 SP (0x2000xxxx)
 *   0x00000004  reset vector
 */
#include "Nano100Series.h"
#include "delay.h"
#include "lcd1602.h"
#include "keypad.h"
#include "swd.h"

static void show_addr(uint32_t a)
{
    lcd_goto(0, 0);
    lcd_putc('A'); lcd_putc(':');
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t n = (a >> i) & 0xFu;
        lcd_putc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
    }
    lcd_puts("      ");
}

static void show_data(const char *tag, uint32_t v, int hex)
{
    lcd_goto(1, 0);
    lcd_putc('D'); lcd_putc(':');
    if (hex) {
        for (int i = 28; i >= 0; i -= 4) {
            uint32_t n = (v >> i) & 0xFu;
            lcd_putc(n < 10 ? (char)('0' + n) : (char)('A' + n - 10));
        }
        lcd_puts("  ");
    } else {
        lcd_puts(tag);
    }
}

int main(void)
{
    SYS_UnlockReg();
    CLK_EnableModuleClock(GPIO_MODULE);
    SYS_LockReg();

    swd_pins_init();
    lcd_init();
    kp_init();

    lcd_goto(0, 0);
    lcd_puts("MEM PEEK");
    lcd_goto(1, 0);
    int cs = swd_connect();
    if (cs == 0) {
        lcd_puts("DP:");
        lcd_puthex(swd_last_dpidr);
    } else {
        lcd_puts("CONNECT FAIL ");
        lcd_putc((char)('0' - cs));      /* 1/2/3 */
    }
    delay_ms(1500);

    int connected = (cs == 0);
    uint32_t addr = 0;
    char held = 0;
    int last_was_hash = 0;
    int armed = 0;                        /* 剛按過 * ，下一鍵是 preset 選擇 */

    show_addr(addr);
    show_data("press #         ", 0, 0);

    for (;;) {
        char k = kp_scan();

        if (k && !held) {
            held = k;

            if (armed && k >= 'A' && k <= 'D') {
                switch (k) {
                case 'A': addr = 0xE000ED00u; break;   /* CPUID */
                case 'B': addr = 0xE000EDF0u; break;   /* DHCSR */
                case 'C': addr = 0xE0001000u; break;   /* DWT   */
                default:  addr = 0xE00FF000u; break;   /* ROM table */
                }
                armed = 0;
                show_addr(addr);
                last_was_hash = 0;
            } else if (k == '*') {
                addr = 0;
                armed = 1;
                show_addr(addr);
                show_data("preset? A-D     ", 0, 0);
                last_was_hash = 0;
            } else if (k == '#') {
                armed = 0;
                if (!connected)
                    connected = (swd_connect() == 0);

                if (!connected) {
                    show_data("NO TARGET       ", 0, 0);
                } else {
                    if (last_was_hash)
                        addr += 4;
                    show_addr(addr);

                    uint32_t val = 0;
                    int a = swd_mem_read(addr, &val);
                    if (a == 1)
                        show_data(0, val, 1);
                    else if (a == -1)
                        show_data("PARITY ERR      ", 0, 0);
                    else if (a == 4)
                        show_data("FAULT           ", 0, 0);
                    else {
                        lcd_goto(1, 0);
                        lcd_puts("D:ACK ");
                        lcd_putc((char)('0' + (a & 7)));
                        lcd_puts("         ");
                    }
                }
                last_was_hash = 1;
            } else {
                armed = 0;
                uint32_t nib = (k <= '9') ? (uint32_t)(k - '0')
                                          : (uint32_t)(k - 'A' + 10);
                addr = (addr << 4) | nib;
                show_addr(addr);
                last_was_hash = 0;
            }
        } else if (!k) {
            held = 0;
        }

        delay_ms(10);
    }
}
