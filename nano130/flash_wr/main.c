/*
 * nano130/flash_wr —— N1：把客製版從 debugger 變成燒錄器
 *
 * master（燒這支）= 客製版 C3(UID 0615) + keypad + LCD + common/swd.h
 * target          = 另一塊客製版，J4 接 master 的 J17（接線見 common/swd.h）
 *                    2026-09-15 上午是 C1(062C)、下午換成 C2(04E1)。
 *                    **三塊外觀完全一樣，這支韌體也不會知道它在跟哪一塊講話**
 *                    —— 換板先用 A 板 NuLink 讀 UID 核對。
 *
 * 驗證階梯（設計目標：讓客製版自己就是燒錄器，PC 端只剩一支腳本）：
 *   Stage 1-3 完全不改變 target，Stage 4-5 才會寫 flash。
 *
 * keypad：
 *   A  Stage 1：讀 ISPCON（0x5000C000）—— 驗 FMC 基底位址對不對
 *   B  Stage 2：SYS 解鎖序列 0x59/0x16/0x88 → 讀回 RegLockAddr 應為 1
 *   C  Stage 3：開 ISPEN+APUEN，用 ISP READ 命令讀 0x00000000，
 *               跟 MEM-AP 直接讀的同一個位址比對 —— **N1 唯一的技術風險**：
 *               CPU halted 時 ISP 引擎到底會不會動。兩邊一致就證明會。
 *   0  解除寫入保險（只對下一次 1 或 2 有效，做完自動上鎖回去）
 *   1  Stage 4：PROGRAM 一個哨兵 word 到 APROM 最後一頁
 *   2  Stage 5：PAGE_ERASE 那一頁 → 整頁驗全 FF
 *   3  純觀察：MEM-AP 直接讀最後一頁第一個 word，不經 ISP
 *   9  放 target 繼續跑（收工用；每個 stage 都會 halt 它）
 *   D  把 target 鎖回去（ISPCON 清 0、SYS RegLock 上鎖）—— 收工前按一下
 *   #  halt target（每個 stage 前都會自動 halt，這支是手動補按用）
 *   *  重新 connect
 *
 * 安全規則（見 HANDOVER）：絕不設 CFGUEN、絕不碰 CONFIG(0x00300000)。
 * 這支程式裡兩者都不出現，寫入位址只有寫死的 LAST_PAGE 一個。
 */
#include "Nano100Series.h"
#include "delay.h"
#include "lcd1602.h"
#include "keypad.h"
#include "swd.h"

/* ── target 端暫存器地圖（查自 vendor/ 的 BSP header，不是猜的）───────── */
#define T_REGLOCK   0x50000100u     /* SYS->RegLockAddr：寫 0x59/0x16/0x88 解鎖 */
#define T_AHBCLK    0x50000204u     /* CLK->AHBCLK，bit2 = ISP_EN（FMC 時脈）   */
#define T_ISPCON    0x5000C000u
#define T_ISPADR    0x5000C004u
#define T_ISPDAT    0x5000C008u
#define T_ISPCMD    0x5000C00Cu
#define T_ISPTRG    0x5000C010u
#define T_ISPSTA    0x5000C040u

#define ISPCON_ISPEN   (1u << 0)
#define ISPCON_APUEN   (1u << 3)    /* APROM 更新。CFGUEN(bit4) 永遠不設 */
#define ISPCON_ISPFF   (1u << 6)    /* 寫 1 清除 */

#define ISPCMD_READ    0x00u
#define ISPCMD_PROGRAM 0x21u
#define ISPCMD_ERASE   0x22u

/* Stage 3 拿來對帳的位址：APROM 第 0 個 word（target 的初始 SP）。
 * 它一定不是 0xFFFFFFFF（target 跑著 keypad_lcd），所以「兩邊都讀到 FF」
 * 這種假通過不會發生。2026-09-15 實測兩邊都是 0x20004000。 */
#define PROBE_ADDR     0x00000000u

/* Stage 4/5 的動手範圍只有 APROM 的**最後一頁**：0x0001EA00 ~ 0x0001EBFF。
 * APROM 到 0x0001EC00 為止、頁大小 512 byte，所以這是最後一頁；
 * target 跑的 keypad_lcd 只有幾 KB，這一頁一定是空的。 */
#define LAST_PAGE      0x0001EA00u
#define SIGNATURE      0xC0DE0001u  /* 哨兵值：非 FF、非 0，一眼就知道是我們寫的 */

static int connected;

/* 寫入類的動作要先按 0 解除保險，做完一次自動上鎖，不會一路開著。 */
static int armed;

static void line1(const char *s) { lcd_goto(1, 0); lcd_puts(s); }

static void show_val(const char *tag, uint32_t v)
{
    lcd_goto(1, 0);
    lcd_puts(tag);
    lcd_puthex8(v);
    lcd_puts("    ");
}

/* 每個 stage 之前都先確定 target 停住 —— ISP 引擎跟 CPU 共用 flash 介面，
 * 讓 target 邊跑邊被戳 flash 沒有意義（Stage 4/5 更是危險）。 */
static int ensure_halt(void)
{
    uint32_t d = 0;
    if (swd_halt() != 1)
        return 0;
    for (int i = 0; i < 50; i++) {
        if (swd_dhcsr(&d) == 1 && (d & (1u << 17)))
            return 1;
        delay_ms(1);
    }
    return 0;
}

/* SYS 解鎖：三個 magic 依序寫進同一個位址。讀回 1 = 已解鎖。 */
static int target_unlock(uint32_t *readback)
{
    if (swd_mem_write(T_REGLOCK, 0x59u) != 1) return 0;
    if (swd_mem_write(T_REGLOCK, 0x16u) != 1) return 0;
    if (swd_mem_write(T_REGLOCK, 0x88u) != 1) return 0;
    return swd_mem_read(T_REGLOCK, readback) == 1;
}

/* 觸發一次 ISP 命令，等 ISPGO 自清。回傳 0 成功、-1 匯流排錯、-2 逾時、-3 ISPFF */
static int isp_trigger(uint32_t cmd, uint32_t addr, uint32_t dat)
{
    uint32_t v = 0;

    if (swd_mem_write(T_ISPCMD, cmd)  != 1) return -1;
    if (swd_mem_write(T_ISPADR, addr) != 1) return -1;
    if (swd_mem_write(T_ISPDAT, dat)  != 1) return -1;
    if (swd_mem_write(T_ISPTRG, 1u)   != 1) return -1;   /* ISPGO */

    for (int i = 0; i < 200; i++) {
        if (swd_mem_read(T_ISPTRG, &v) != 1) return -1;
        if ((v & 1u) == 0) {
            if (swd_mem_read(T_ISPCON, &v) != 1) return -1;
            return (v & ISPCON_ISPFF) ? -3 : 0;
        }
        delay_us(200);
    }
    return -2;
}

/* 錯誤碼會直接秀在 LCD 上，所以 tag 一定要短：
 * tag + 8 位十六進位不能超過 16 格，否則碼的低位會被推出螢幕外看不見
 * （第一版用 "prep err:" 9 個字，剛好把最後一位吃掉，變成一串 0）。
 *   1 = halt 失敗  2 = 解鎖失敗  3 = AHBCLK 存取失敗
 *   4 = ISPCON 存取失敗  5 = ISPEN 寫不進去
 *
 * Stage 3/4/5 共用的前置：halt + 解鎖 + 開 ISP 時脈 + 設 ISPEN|APUEN。
 * 回傳 0 成功，非 0 是給 LCD 顯示的錯誤碼。 */
static int isp_prepare(void)
{
    uint32_t v = 0;

    if (!ensure_halt())                                return 1;
    if (!target_unlock(&v) || !(v & 1u))               return 2;
    /* FMC 的時脈。ISP_EN 沒開的話所有 ISP 暫存器都讀得到、寫不進去。 */
    if (swd_mem_read(T_AHBCLK, &v) != 1)               return 3;
    if (swd_mem_write(T_AHBCLK, v | (1u << 2)) != 1)   return 3;
    /* ISPEN + APUEN。CFGUEN / LDUEN 不設；順手寫 1 清掉舊的 ISPFF。 */
    if (swd_mem_write(T_ISPCON, ISPCON_ISPEN | ISPCON_APUEN | ISPCON_ISPFF) != 1) return 4;
    if (swd_mem_read(T_ISPCON, &v) != 1)               return 4;
    if (!(v & ISPCON_ISPEN))                           return 5;
    return 0;
}

static void stage1(void)
{
    uint32_t v = 0;
    lcd_goto(0, 0); lcd_puts("S1 ISPCON       ");
    if (!ensure_halt())                  { line1("halt fail       "); return; }
    if (swd_mem_read(T_ISPCON, &v) != 1) { line1("read fail       "); return; }
    show_val("CON:", v);
}

static void stage2(void)
{
    uint32_t v = 0;
    lcd_goto(0, 0); lcd_puts("S2 UNLOCK       ");
    if (!ensure_halt())     { line1("halt fail       "); return; }
    if (!target_unlock(&v)) { line1("write fail      "); return; }
    lcd_goto(0, 0);
    lcd_puts((v & 1u) ? "S2 UNLOCK OK    " : "S2 UNLOCK NO    ");
    show_val("LCK:", v);
}

/*
 * Stage 3 —— 這個階梯的關鍵設計。
 * 用 ISP READ 讀 PROBE_ADDR，再用 MEM-AP 直接讀同一個位址，兩邊比對。
 * 相同 = ISP 引擎在 core halted 時確實會執行，N1 唯一的未知就此排除，
 * 而且整段完全沒改變 target。
 */
static void stage3(void)
{
    uint32_t isp = 0, mem = 0;
    int r;

    lcd_goto(0, 0); lcd_puts("S3 ISP READ     ");
    if ((r = isp_prepare()) != 0) { show_val("prep:", (uint32_t)r); return; }

    r = isp_trigger(ISPCMD_READ, PROBE_ADDR, 0);
    if (r != 0) {
        lcd_goto(0, 0); lcd_puts("S3 TRIG ERR     ");
        show_val("err:", (uint32_t)(-r));
        return;
    }
    if (swd_mem_read(T_ISPDAT, &isp)   != 1) { line1("ispdat fail     "); return; }
    if (swd_mem_read(PROBE_ADDR, &mem) != 1) { line1("mem rd fail     "); return; }

    /* 兩個值都秀出來，不要只秀結論 —— 不一致時那兩個數字本身就是線索 */
    lcd_goto(0, 0);
    lcd_puts("ISP:");
    lcd_puthex8(isp);
    lcd_puts(isp == mem ? " OK " : " != ");
    show_val("MEM:", mem);
}

/*
 * Stage 4 —— 第一個真的會改變 target 的動作。
 *
 * 為什麼順序是「先 PROGRAM 再 PAGE_ERASE」而不是反過來：flash 只能 1→0，
 * 往空白頁寫一個 word 是最小的可逆動作（下一階抹掉就還原了），而先抹除等於
 * 在還沒證明寫入路徑能動之前就先做了比較大的動作。
 */
static void stage4(void)
{
    uint32_t before = 0, after = 0;
    int r;

    lcd_goto(0, 0); lcd_puts("S4 PROGRAM      ");
    if (!armed) { line1("press 0 to arm  "); return; }
    armed = 0;

    if ((r = isp_prepare()) != 0) { show_val("prep:", (uint32_t)r); return; }

    /* 先確認那一頁真的是空的。flash 只能 1→0，往非空白位址寫會得到垃圾值，
     * 而且那不是 ISP 的錯 —— 先驗這一條才不會把 bug 記到錯的地方。 */
    if (swd_mem_read(LAST_PAGE, &before) != 1) { line1("rd before fail  "); return; }
    if (before != 0xFFFFFFFFu) {
        lcd_goto(0, 0); lcd_puts("S4 NOT BLANK    ");
        show_val("was:", before);
        return;
    }

    r = isp_trigger(ISPCMD_PROGRAM, LAST_PAGE, SIGNATURE);
    if (r != 0) {
        lcd_goto(0, 0); lcd_puts("S4 TRIG ERR     ");
        show_val("err:", (uint32_t)(-r));
        return;
    }

    if (swd_mem_read(LAST_PAGE, &after) != 1) { line1("rd after fail   "); return; }
    lcd_goto(0, 0);
    lcd_puts(after == SIGNATURE ? "S4 WROTE OK     " : "S4 WROTE BAD    ");
    show_val("got:", after);
}

static void stage5(void)
{
    uint32_t v = 0;
    int r, bad = 0;

    lcd_goto(0, 0); lcd_puts("S5 PAGE ERASE   ");
    if (!armed) { line1("press 0 to arm  "); return; }
    armed = 0;

    if ((r = isp_prepare()) != 0) { show_val("prep:", (uint32_t)r); return; }

    r = isp_trigger(ISPCMD_ERASE, LAST_PAGE, 0);
    if (r != 0) {
        lcd_goto(0, 0); lcd_puts("S5 TRIG ERR     ");
        show_val("err:", (uint32_t)(-r));
        return;
    }

    /* 抹除是整頁的，所以驗收也要看整頁 —— 只驗第一個 word 的話，
     * 「頁號算錯但剛好那個 word 本來就是 FF」會被當成通過。 */
    for (uint32_t off = 0; off < 512u; off += 4u) {
        if (swd_mem_read(LAST_PAGE + off, &v) != 1) { line1("verify rd fail  "); return; }
        if (v != 0xFFFFFFFFu) { bad = 1; break; }
    }

    lcd_goto(0, 0);
    lcd_puts(bad ? "S5 ERASE BAD    " : "S5 ERASED OK    ");
    show_val(bad ? "got:" : "all:", bad ? v : 0xFFFFFFFFu);
}

/* 純觀察用：直接 MEM-AP 讀最後一頁第一個 word，不經 ISP、不改任何東西。 */
static void peek_page(void)
{
    uint32_t v = 0;
    lcd_goto(0, 0); lcd_puts("PEEK 0001EA00   ");
    if (swd_mem_read(LAST_PAGE, &v) != 1) { line1("read fail       "); return; }
    show_val("val:", v);
}

/* 收工：把 ISPCON 清乾淨、SYS 重新上鎖。target 回到我們碰它之前的樣子。 */
static void relock(void)
{
    uint32_t v = 0;
    lcd_goto(0, 0); lcd_puts("RELOCK          ");
    swd_mem_write(T_ISPCON, ISPCON_ISPFF);   /* ISPEN/APUEN 清 0，順便清 ISPFF */
    swd_mem_write(T_REGLOCK, 0x00u);         /* 任意非 magic 值即重新上鎖 */
    armed = 0;
    if (swd_mem_read(T_REGLOCK, &v) == 1)
        show_val("LCK:", v);                 /* 應為 0 */
    else
        line1("read fail       ");
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
    lcd_puts("FLASH WR N1");
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
    lcd_goto(0, 0); lcd_puts("A:S1 B:S2 C:S3  ");
    lcd_goto(1, 0); lcd_puts("0arm 1wr 2ers 3?");

    char held = 0;

    for (;;) {
        char k = kp_scan();

        if (k && !held) {
            held = k;

            if (!connected && k != '*') {
                lcd_goto(0, 0); lcd_puts("NOT CONNECTED   ");
            } else switch (k) {
            case 'A': stage1(); break;
            case 'B': stage2(); break;
            case 'C': stage3(); break;
            case '0':
                armed = 1;
                lcd_goto(0, 0); lcd_puts("ARMED: 1=wr 2=er");
                line1("next write only ");
                break;
            case '1': stage4(); break;
            case '2': stage5(); break;
            case '3': peek_page(); break;
            case '9':
                /* 收工讓 target 回去跑自己的程式。第一版漏了這支，跑完只能按
                 * target 的 SW1 手動重開 —— cpu_ctrl 有 run，這裡忘了搬過來。 */
                lcd_goto(0, 0);
                lcd_puts(swd_run() == 1 ? "TARGET RUNNING  " : "run fail        ");
                break;
            case 'D': relock(); break;
            case '#':
                lcd_goto(0, 0);
                lcd_puts(ensure_halt() ? "HALTED          " : "halt fail       ");
                break;
            case '*':
                cs = swd_connect();
                connected = (cs == 0);
                lcd_goto(0, 0);
                if (connected) { lcd_puts("DP:"); lcd_puthex(swd_last_dpidr); lcd_puts("      "); }
                else           { lcd_puts("RECONNECT FAIL  "); }
                break;
            default: break;
            }
        } else if (!k) {
            held = 0;
        }

        delay_ms(10);
    }
}
