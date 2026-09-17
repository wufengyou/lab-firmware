/*
 * nano130/cpu_ctrl —— Stage 3：用客製版當 debugger，halt / step / run target 的 CPU
 *
 * master（燒這支）= 客製版 + keypad + LCD + common/swd.h。
 * target = 另一塊客製版（跑著 keypad_lcd），J4 接 master 的 J17（見 common/swd.h）。
 *
 * keypad：
 *   A  halt   —— 停住 target 的 CPU
 *   B  run    —— 放它繼續跑
 *   C  step   —— 單步一條指令（會遮中斷，不踩進 ISR）
 *   D  下一個核心暫存器（R0→R1→…→R15→xPSR→R0 輪流顯示）
 *   #  重讀狀態（DHCSR + PC）
 *   *  重新 connect
 *
 * LCD：
 *   第一行  <state> PC:XXXXXXXX     state = HALT / RUN  / LOCK / ????
 *   第二行  R n:XXXXXXXX  或  xPSR:XXXXXXXX  或  DHCSR:XXXXXXXX
 */
#include "Nano100Series.h"
#include "delay.h"
#include "lcd1602.h"
#include "keypad.h"
#include "swd.h"

static int connected;

static void show_state(void)
{
    uint32_t dhcsr = 0;
    lcd_goto(0, 0);
    if (!connected || swd_dhcsr(&dhcsr) != 1) {
        lcd_puts("SWD ERR         ");
        return;
    }

    int halted = (dhcsr >> 17) & 1;
    int lockup = (dhcsr >> 19) & 1;
    lcd_puts(lockup ? "LOCK" : halted ? "HALT" : "RUN ");
    lcd_puts(" PC:");
    if (halted) {
        uint32_t pc = 0;
        swd_core_reg(15, &pc);
        lcd_puthex8(pc);
    } else {
        lcd_puts("--------");           /* PC 只在 halt 時有效 */
    }
}

static void show_reg(uint32_t sel)
{
    uint32_t v = 0;
    int a = swd_core_reg(sel, &v);
    lcd_goto(1, 0);
    if (a != 1) {
        lcd_puts("reg err         ");
        return;
    }
    if (sel == 16) {
        lcd_puts("xPSR:");
        lcd_puthex8(v);
        lcd_puts("   ");
    } else {
        lcd_puts("R");
        if (sel >= 10) { lcd_putc('1'); lcd_putc((char)('0' + sel - 10)); }
        else           { lcd_putc((char)('0' + sel)); lcd_putc(' '); }
        lcd_puts(":");
        lcd_puthex8(v);
        lcd_puts("  ");
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
    lcd_puts("CPU CTRL");
    lcd_goto(1, 0);
    int cs = swd_connect();
    connected = (cs == 0);
    if (connected) {
        lcd_puts("DP:");
        lcd_puthex(swd_last_dpidr);
    } else {
        lcd_puts("CONNECT FAIL ");
        lcd_putc((char)('0' - cs));
    }
    delay_ms(1500);

    lcd_clear();
    show_state();
    lcd_goto(1, 0);
    lcd_puts("A halt B run    ");

    uint32_t regsel = 0;
    char held = 0;

    for (;;) {
        char k = kp_scan();

        if (k && !held) {
            held = k;

            switch (k) {
            case 'A':
                if (connected) swd_halt();
                show_state();
                regsel = 0;
                break;
            case 'B':
                if (connected) swd_run();
                show_state();
                lcd_goto(1, 0);
                lcd_puts("running...      ");
                break;
            case 'C':
                if (connected) swd_step();
                show_state();
                show_reg(15);            /* step 後直接秀新 PC */
                break;
            case 'D':
                show_reg(regsel);
                regsel = (regsel >= 16) ? 0 : regsel + 1;
                break;
            case '#':
                show_state();
                break;
            case '*': {
                cs = swd_connect();
                connected = (cs == 0);
                lcd_goto(0, 0);
                if (connected) { lcd_puts("DP:"); lcd_puthex(swd_last_dpidr); }
                else           { lcd_puts("RECONNECT FAIL  "); }
                break;
            }
            default:
                break;
            }
        } else if (!k) {
            held = 0;
        }

        delay_ms(10);
    }
}
