/*
 * stm32/cpu_ctrl —— Stage 3：用 F446RE 當 debugger，halt / step / run target 的 CPU
 *
 * 這是 nano130/cpu_ctrl 的移植。SWD/DAP 協定碼一字不改（common/swd.h），
 * 換掉的只有「人機介面」：nano130 那邊是 keypad + LCD1602，這裡沒有那些週邊，
 * 改用 USART2 的虛擬 COM port —— PC 端終端機打字下指令、看結果。
 * 反而比 16x2 的 LCD 好用：暫存器可以一次全部列出來，不必按 D 一個一個翻。
 *
 * 接線與 swd_probe 完全相同（三條線），見 README「接線」。cpu_ctrl 不需要多接
 * 任何一條 —— halt/step/run 全部走 SWD 的 MEM-AP 寫暫存器，沒有用到 nRESET。
 *
 * 指令（在終端機直接按鍵，不用按 Enter）：
 *   h   halt      停住 target 的 CPU
 *   r   run       放它繼續跑
 *   s   step      單步一條指令（C_MASKINTS=1，會遮中斷，不踩進 ISR；
 *                   但 C_MASKINTS 必須先單獨寫一次才生效 —— 見 swd.h 的 swd_step）
 *   d   dump      列出 R0-R12 / SP / LR / PC / xPSR
 *   ?   status    重讀 DHCSR + PC
 *   c   connect   重新 connect（線鬆掉、target 斷電後用這個）
 *   m   memory    讀一個位址（接著打 8 位 hex，Enter 或滿 8 位就送出）
 */
#include "stm32f446.h"
#include "delay.h"
#include "uart.h"
#include "swd.h"

static int connected;

/* ── DHCSR 的位元（ARMv7-M B1.5.15）──
 * 讀出來的 DHCSR 高半字是 S_* 狀態位元，寫進去的高半字必須是 DBGKEY。
 * 這個「讀寫不對稱」是踩過的坑：拿讀回來的值原封不動寫回去，會因為
 * 高半字不是 0xA05F 而被整包丟掉，症狀是「寫了沒反應也不報錯」。 */
#define S_REGRDY    (1u << 16)
#define S_HALT      (1u << 17)
#define S_SLEEP     (1u << 18)
#define S_LOCKUP    (1u << 19)

static const char *reg_name[17] = {
    "R0 ", "R1 ", "R2 ", "R3 ", "R4 ", "R5 ", "R6 ", "R7 ",
    "R8 ", "R9 ", "R10", "R11", "R12", "SP ", "LR ", "PC ", "xPSR"
};

static void show_state(void)
{
    uint32_t dhcsr = 0;

    if (!connected || swd_dhcsr(&dhcsr) != 1) {
        uart_puts("  SWD ERR —— 按 c 重新 connect\r\n");
        connected = 0;
        return;
    }

    uart_puts("  ");
    if (dhcsr & S_LOCKUP)     uart_puts("LOCKUP");
    else if (dhcsr & S_HALT)  uart_puts("HALT  ");
    else if (dhcsr & S_SLEEP) uart_puts("SLEEP ");
    else                      uart_puts("RUN   ");

    uart_puts("  DHCSR=");
    uart_puthex(dhcsr);

    /* PC 只在 halt 的時候有意義 —— CPU 還在跑的時候讀核心暫存器，
     * 讀到的是「某個瞬間」的值，下一個 cycle 就變了，印出來會誤導。 */
    uart_puts("  PC=");
    if (dhcsr & S_HALT) {
        uint32_t pc = 0;
        if (swd_core_reg(15, &pc) == 1)
            uart_puthex(pc);
        else
            uart_puts("(讀失敗)");
    } else {
        uart_puts("--------");
    }
    uart_puts("\r\n");
}

static void dump_regs(void)
{
    uint32_t dhcsr = 0;

    if (!connected || swd_dhcsr(&dhcsr) != 1) {
        uart_puts("  SWD ERR\r\n");
        connected = 0;
        return;
    }
    if (!(dhcsr & S_HALT)) {
        uart_puts("  要先 halt（按 h）—— 跑著的時候讀暫存器沒有意義\r\n");
        return;
    }

    for (int i = 0; i < 17; i++) {
        uint32_t v = 0;
        /* DCRSR 的 REGSEL：0-12=R0-R12、13=SP、14=LR、15=PC、16=xPSR */
        uint32_t sel = (i == 16) ? 16u : (uint32_t)i;

        uart_puts("  ");
        uart_puts(reg_name[i]);
        uart_puts(" = ");
        if (swd_core_reg(sel, &v) == 1)
            uart_puthex(v);
        else
            uart_puts("(讀失敗)");
        uart_puts((i % 2) ? "\r\n" : "    ");
    }
    uart_puts("\r\n");
}

/* 打一個 8 位 hex 位址，Enter 或滿 8 位就送出。打錯按 Esc 取消。 */
static void read_mem(void)
{
    uint32_t addr = 0;
    int n = 0;

    uart_puts("  addr(hex)= ");
    for (;;) {
        int c = uart_getc_nb();
        if (c < 0)
            continue;

        if (c == 27) {                              /* Esc */
            uart_puts(" 取消\r\n");
            return;
        }
        if (c == '\r' || c == '\n') {
            if (n == 0) { uart_puts(" 取消\r\n"); return; }
            break;
        }

        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0)
            continue;                               /* 不是 hex 就無視 */

        uart_putc((char)c);
        addr = (addr << 4) | (uint32_t)d;
        if (++n == 8)
            break;
    }
    uart_puts("\r\n");

    /* MEM-AP 是 word access —— 位址沒對齊的話讀回來的東西會錯位，
     * 直接切掉低兩位並告知，比讓人對著怪數字猜好。 */
    if (addr & 3u) {
        uart_puts("  （位址非 4-byte 對齊，已對齊到 ");
        addr &= ~3u;
        uart_puthex(addr);
        uart_puts("）\r\n");
    }

    uint32_t v = 0;
    if (swd_mem_read(addr, &v) == 1) {
        uart_puts("  [");
        uart_puthex(addr);
        uart_puts("] = ");
        uart_puthex(v);
        uart_puts("\r\n");
    } else {
        uart_puts("  讀失敗（位址無效 / 該區塊沒開時脈 / 連線斷了）\r\n");
    }
}

static void do_connect(void)
{
    connected = 0;

    uint32_t id = 0;
    int ack = -9;
    for (int t = 0; t < 4; t++) {
        ack = swd_read_idcode(&id);
        if (ack == 1)
            break;
        delay_ms(50);
    }
    if (ack != 1) {
        uart_puts("  no ACK —— 檢查接線 / 共地 / target 的 CN3 跳線帽有沒有拔掉\r\n");
        return;
    }

    uart_puts("  DPIDR = ");
    uart_puthex(id);
    uart_puts("\r\n");

    if (swd_connect() != 0) {
        uart_puts("  connect fail —— DP 上電或 AP 選擇失敗\r\n");
        return;
    }

    connected = 1;

    /* halt/step/run 都要 C_DEBUGEN=1 才有效。swd_halt() 會順便設它，
     * 但 run 之後若沒人再 halt，C_DEBUGEN 還是留著的 —— 這裡先讀一次
     * 狀態，讓使用者看到 target 現在到底是跑著還是停著。 */
    show_state();
}

static void help(void)
{
    uart_puts("\r\n"
              "  h halt   r run    s step   d dump regs\r\n"
              "  m mem    ? status c connect\r\n");
}

int main(void)
{
    dwt_init();
    uart_init();
    swd_pins_init();

    uart_puts("\r\n\r\n=== STM32 cpu_ctrl（Stage 3）===\r\n");
    help();

    do_connect();
    uart_puts("> ");

    for (;;) {
        int c = uart_getc_nb();
        if (c < 0)
            continue;

        uart_putc((char)c);
        uart_puts("\r\n");

        switch (c) {
        case 'h':
            if (connected && swd_halt() == 1) show_state();
            else { uart_puts("  halt 失敗\r\n"); connected = 0; }
            break;

        case 'r':
            if (connected && swd_run() == 1) show_state();
            else { uart_puts("  run 失敗\r\n"); connected = 0; }
            break;

        case 's':
            /* step 前一定要是 halt 狀態。對著跑著的 CPU 寫 C_STEP，
             * 行為是 unpredictable（ARMv7-M B1.5.15），不要賭。 */
            {
                uint32_t d = 0;
                if (!connected || swd_dhcsr(&d) != 1) {
                    uart_puts("  SWD ERR\r\n"); connected = 0; break;
                }
                if (!(d & S_HALT)) {
                    uart_puts("  要先 halt（按 h）\r\n"); break;
                }
                if (swd_step() == 1) show_state();
                else { uart_puts("  step 失敗\r\n"); connected = 0; }
            }
            break;

        case 'd': dump_regs(); break;
        case 'm': read_mem();  break;
        case '?': show_state(); break;
        case 'c': do_connect(); break;

        case '\r':
        case '\n': break;

        default: help(); break;
        }

        uart_puts("> ");
    }
}
