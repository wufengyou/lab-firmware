/*
 * nano130/swd_probe —— Stage 1：自製 SWD master，bit-bang 讀出 target 的 DPIDR
 *
 * 最小驗證：手動 clock 出 SWD 的每個 bit，跑 line-reset + JTAG-to-SWD 切換序列，
 * 讀 Debug Port IDCODE。讀到合理值（ARM Cortex-M0 SW-DP = 0x0BB11477）就代表
 * 自製的 SWD 線協定通了。SWD 引擎在 common/swd.h。
 *
 * 2026-09-10 上機驗證：DPIDR = 0x0BB11477，parity 通過。
 *
 * 接線見 common/swd.h 檔頭。結果顯示在 master 自己的 LCD1602。
 */
#include "Nano100Series.h"
#include "delay.h"
#include "lcd1602.h"
#include "swd.h"

int main(void)
{
    SYS_UnlockReg();
    CLK_EnableModuleClock(GPIO_MODULE);
    SYS_LockReg();

    swd_pins_init();
    lcd_init();
    lcd_goto(0, 0);
    lcd_puts("SWD PROBE");

    uint32_t id = 0;
    int ack = -9;
    for (int t = 0; t < 4; t++) {
        ack = swd_read_idcode(&id);
        if (ack == 1)
            break;
        delay_ms(50);
    }

    lcd_goto(1, 0);
    if (ack == 1) {
        lcd_puts("DPIDR:");
        lcd_puthex(id);
    } else if (ack == -1) {
        lcd_puts("PARITY ERR      ");
    } else {
        lcd_puts("NO ACK (");
        lcd_putc((char)('0' + (ack & 7)));
        lcd_puts(")       ");
    }

    for (;;)
        __NOP();
}
