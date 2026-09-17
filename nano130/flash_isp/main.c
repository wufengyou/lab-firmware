/*
 * nano130/flash_isp —— N2：客製版當燒錄器，payload 走 UART0 從 PC 來
 *
 * master（燒這支）= 客製版 C3(UID 0615) + keypad + LCD + UART0 + common/swd.h
 * target          = 另一塊客製版，J4 接 master 的 J17（接線見 common/swd.h）
 * PC              = USB-TTL 接 master 的 PB.0/PB.1（見 common/uart0.h），
 *                   host 端工具是 nano130/isp_send.ps1
 *
 * 這一支涵蓋 N2 的 Stage b～e，**階不是靠不同韌體分，是靠「送什麼」和
 * 「有沒有解保險」分**：
 *   Stage b  不解保險，送幾個封包 → 只驗 CRC 與位址，回 'D'(dry run)，不碰 target
 *   Stage c  解保險，送 1 個 512 byte 封包 → 抹該頁、寫入、回讀驗證
 *   Stage d  解保險，送多個連續封包 → 同上逐頁做，LCD 顯示進度
 *   Stage e  解保險，送整支 blink.bin 到 0x00000000，再**重置** target（'S' 或按 8）
 *
 * ── 協定（host → master，每塊獨立 ACK，master 端不需要大緩衝區）────────
 *   'P'                                            ping
 *   'W' addr[4 LE] len[2 LE] data[len] crc16[2 LE] 寫一塊
 *   'D' addr[4 LE] len[2 LE]                       讀一塊（回 'K' + data + crc16）
 *   'E' addr[4 LE] pages[2 LE]                     抹除連續幾頁（要先解保險）
 *   'R'                                            放 target 從停住的地方繼續跑
 *   'S'                                            重置 target（**燒完要用這個**）
 *   'H'                                            halt target
 * master → host 一律回一個 byte：
 *   'P' ping 回應 / 'K' 寫入並驗證通過 / 'D' dry run（沒解保險）通過
 *   'E' + 一個錯誤碼 byte（見 ERR_*）
 *
 * CRC16-CCITT（poly 0x1021、init 0xFFFF），涵蓋 addr + len + data。
 *
 * ── 安全規則 ──────────────────────────────────────────────────────────
 * **位址檢查寫在這一端，不是寫在 PC 的腳本裡** —— 腳本會被改、會被打錯、
 * 會被別人拿去跑；韌體不會。CONFIG(0x00300000) 與 CFGUEN 在這支程式裡
 * 一個字都不出現，**寫入**時 APROM 以外的位址一律拒收。
 *
 * **讀取（'D'）刻意不設限** —— 讀不改變任何東西，安全規則管的是寫。
 * 不設限反而讓它能當 peek 用（把 CONFIG 讀出來存檔、讀週邊暫存器除錯）。
 */
#include "Nano100Series.h"
#include "delay.h"
#include "lcd1602.h"
#include "keypad.h"
#include "uart0.h"
#include "swd.h"

/* ── target 端暫存器地圖（與 flash_wr 相同，查自 vendor/ 的 BSP header）── */
#define T_REGLOCK   0x50000100u
#define T_AHBCLK    0x50000204u
#define T_ISPCON    0x5000C000u
#define T_ISPADR    0x5000C004u
#define T_ISPDAT    0x5000C008u
#define T_ISPCMD    0x5000C00Cu
#define T_ISPTRG    0x5000C010u

#define ISPCON_ISPEN   (1u << 0)
#define ISPCON_APUEN   (1u << 3)    /* CFGUEN(bit4) 永遠不設 */
#define ISPCON_ISPFF   (1u << 6)

#define ISPCMD_PROGRAM 0x21u
#define ISPCMD_ERASE   0x22u

#define APROM_END      0x0001EC00u  /* APROM 是 0x00000000 ~ 0x0001EC00（123 KB）*/
#define PAGE_SIZE      512u         /* ⚠ 512 byte，不是 STM32L4 的 2 KB */
#define CHUNK_MAX      PAGE_SIZE

/* 回給 host 的錯誤碼。**碼要能直接指到一行程式**，所以分得細一點。 */
#define ERR_CRC        1u
#define ERR_ADDR       2u
#define ERR_LEN        3u
#define ERR_PREP       4u
#define ERR_ERASE      5u
#define ERR_PROGRAM    6u
#define ERR_VERIFY     7u
#define ERR_TIMEOUT    8u
#define ERR_NOLINK     9u
#define ERR_NOTARMED   10u          /* 2026-09-15 補：原本跟 ERR_NOLINK 共用一個碼，
                                     * 於是「沒解保險」和「SWD 沒連線」回同一個
                                     * 數字，從 host 端分辨不出來 —— 正好違反這支
                                     * 檔案自己寫的「碼要能直接指到一行程式」。
                                     * 當天就因為這個多繞了兩輪才發現是保險被
                                     * 亂按時切掉了。 */

#define BAUD           115200u

static int connected;
static int armed;                   /* 0 = dry run。燒整支 bin 要連續好幾十塊，
                                     * 所以這裡是「切換」而不是 flash_wr 那種
                                     * 「做完一次自動上鎖」—— 保險的粒度要配合
                                     * 動作的粒度，不然它只會被無視。 */
static uint8_t buf[CHUNK_MAX];
static uint32_t ok_chunks, err_chunks;

/* ── LCD ──────────────────────────────────────────────────────────────── */
static void put_nib(uint8_t v) { lcd_putc("0123456789ABCDEF"[v & 0xF]); }

static void put_hex32(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4)
        put_nib((uint8_t)(v >> i));
}

static void put_hex16(uint16_t v)
{
    for (int i = 12; i >= 0; i -= 4)
        put_nib((uint8_t)(v >> i));
}

static void show_arm(void)
{
    lcd_goto(0, 0);
    lcd_puts(armed ? "ARMED  " : "dryrun ");
    lcd_puts(connected ? "SWD ok " : "SWD -- ");
    lcd_puts("  ");
}

static void show_chunk(uint32_t addr, uint16_t len, const char *st)
{
    lcd_goto(1, 0);
    put_hex32(addr);
    lcd_putc(' ');
    put_hex16(len);
    lcd_putc(' ');
    lcd_puts(st);                   /* 3 格：ok / CRC / ADR ... */
}

/* ── CRC16-CCITT ──────────────────────────────────────────────────────── */
static uint16_t crc16(uint16_t crc, uint8_t b)
{
    crc ^= (uint16_t)b << 8;
    for (int i = 0; i < 8; i++)
        crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    return crc;
}

/* ── UART：帶逾時的讀，免得一個掉包就整支卡死 ─────────────────────────── */
static int rx_byte(uint8_t *out)
{
    for (uint32_t i = 0; i < 200000u; i++) {   /* 約幾百 ms，115200 下綽綽有餘 */
        if (uart0_rx_ready()) {
            *out = (uint8_t)UART0->RBR;
            return 1;
        }
    }
    return 0;
}

static void reply(uint8_t c) { uart0_putc(c); }

static void reply_err(uint8_t code)
{
    uart0_putc('E');
    uart0_putc(code);
    err_chunks++;
}

/* ── target 端 ISP（與 flash_wr 同一套，N1 已上機驗證）──────────────────
 * 錯誤碼 1..5 的意思見 flash_wr/main.c 的 isp_prepare()。 */
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

static int target_unlock(uint32_t *readback)
{
    if (swd_mem_write(T_REGLOCK, 0x59u) != 1) return 0;
    if (swd_mem_write(T_REGLOCK, 0x16u) != 1) return 0;
    if (swd_mem_write(T_REGLOCK, 0x88u) != 1) return 0;
    return swd_mem_read(T_REGLOCK, readback) == 1;
}

static int isp_trigger(uint32_t cmd, uint32_t addr, uint32_t dat)
{
    uint32_t v = 0;

    if (swd_mem_write(T_ISPCMD, cmd)  != 1) return -1;
    if (swd_mem_write(T_ISPADR, addr) != 1) return -1;
    if (swd_mem_write(T_ISPDAT, dat)  != 1) return -1;
    if (swd_mem_write(T_ISPTRG, 1u)   != 1) return -1;

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

static int isp_prepare(void)
{
    uint32_t v = 0;

    if (!ensure_halt())                                return 0;
    if (!target_unlock(&v) || !(v & 1u))               return 0;
    if (swd_mem_read(T_AHBCLK, &v) != 1)               return 0;
    if (swd_mem_write(T_AHBCLK, v | (1u << 2)) != 1)   return 0;
    if (swd_mem_write(T_ISPCON, ISPCON_ISPEN | ISPCON_APUEN | ISPCON_ISPFF) != 1) return 0;
    if (swd_mem_read(T_ISPCON, &v) != 1)               return 0;
    return (v & ISPCON_ISPEN) ? 1 : 0;
}

/*
 * 寫一塊：必要時先抹頁，再逐 word 寫，最後**逐 word 回讀比對**。
 *
 * 只在 addr 剛好落在頁首時抹頁。host 是照順序送連續的 512 byte 塊，
 * 所以每塊剛好對應一頁 —— 這讓「抹除」這個不可逆動作的時機變得可預測，
 * 而不是散在某個狀態機裡。
 */
static uint8_t write_chunk(uint32_t addr, uint16_t len)
{
    uint32_t v = 0;

    if (!isp_prepare())
        return ERR_PREP;

    if ((addr % PAGE_SIZE) == 0) {
        if (isp_trigger(ISPCMD_ERASE, addr, 0) != 0)
            return ERR_ERASE;
    }

    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = (uint32_t)buf[i]
                   | ((uint32_t)buf[i + 1] << 8)
                   | ((uint32_t)buf[i + 2] << 16)
                   | ((uint32_t)buf[i + 3] << 24);
        if (isp_trigger(ISPCMD_PROGRAM, addr + i, w) != 0)
            return ERR_PROGRAM;
    }

    /* 驗證走 MEM-AP 直接讀，不走 ISP READ —— 跟寫入是不同的路徑。
     * 用同一條路徑驗證自己寫的東西，等於只證明了「我記得我寫了什麼」。 */
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t w = (uint32_t)buf[i]
                   | ((uint32_t)buf[i + 1] << 8)
                   | ((uint32_t)buf[i + 2] << 16)
                   | ((uint32_t)buf[i + 3] << 24);
        if (swd_mem_read(addr + i, &v) != 1 || v != w)
            return ERR_VERIFY;
    }

    return 0;
}

/*
 * 重置 target —— 燒完真正要的是這個，不是 swd_run()。
 *
 * **'R'(run) 是「從停住的地方繼續」，不是「重開」。** target 的 CPU 是在跑著
 * 舊程式時被 halt 的，PC 停在舊程式的某個位址；把它腳下的 flash 整個換掉之後
 * 讓它「繼續」，等於從一個已經不存在的位址開始執行 —— 向量表、初始 SP、
 * reset handler 一個都不會被用到。2026-09-15 Stage e 第一次就是這樣：
 * 兩塊都寫入且驗證通過，LED 卻不閃。
 *
 * 客製版的 J4 沒有 nRESET 腳，但 Cortex-M 本來就有軟體重置：
 * 往 AIRCR 寫 VECTKEY(0x05FA) + SYSRESETREQ。**這也順帶補上了先前
 * 「沒有 nRESET 就沒有重置手段」這個以為是硬限制的空白。**
 */
#define T_AIRCR     0xE000ED0Cu
#define AIRCR_RESET 0x05FA0004u

static int target_reset(void)
{
    /* 先清掉 C_DEBUGEN，否則重置後核心可能還停在除錯狀態裡。
     * 寫 DHCSR 一定要帶 key，這點跟 halt/run 一樣。 */
    swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0x0u);

    /* 這一筆的回應本來就可能是錯的 —— 晶片在收下它的當下就重置了，
     * 常常來不及把 ACK 送完。所以不檢查回傳值，改用「重連得上」當證據。 */
    swd_mem_write(T_AIRCR, AIRCR_RESET);

    delay_ms(100);
    connected = (swd_connect() == 0);
    return connected;
}

/*
 * ── 'E' 抹除一段頁 ───────────────────────────────────────────────────────
 *
 * 'E' addr[4 LE] pages[2 LE] → 'K' 或 'E' + 碼。
 *
 * **為什麼需要它：** write_chunk() 只抹它自己要寫的那一頁，所以用小韌體覆蓋
 * 大韌體時，後面的頁會原封不動留著上一手的程式碼。2026-09-15 把 936 byte 的
 * blink 燒進 C2 之後整片讀回來，`0x400` 之後還是 keypad_lcd 的尾巴 ——
 * **是 dump 這個功能抓到的，寫入那一側完全看不出來。**
 *
 * 功能上通常無害（新韌體不會跳過去），但殘骸會留下上一手的資料 ——
 * 這個 repo 剛好有前科：C2/C3 原本的韌體裡有明文 WiFi 帳密。
 *
 * 位址一樣擋在韌體這端，而且**這是整支程式裡唯一會大量抹除的入口**，
 * 所以除了 APROM 範圍之外還要求解保險（armed）。
 */
static void cmd_erase(void)
{
    uint8_t b;
    uint32_t addr = 0;
    uint16_t pages = 0;

    for (int i = 0; i < 4; i++) {
        if (!rx_byte(&b)) { reply_err(ERR_TIMEOUT); return; }
        addr |= (uint32_t)b << (8 * i);
    }
    for (int i = 0; i < 2; i++) {
        if (!rx_byte(&b)) { reply_err(ERR_TIMEOUT); return; }
        pages |= (uint16_t)b << (8 * i);
    }

    if (!armed)                      { reply_err(ERR_NOTARMED); return; }
    if (!connected)                  { reply_err(ERR_NOLINK); return; }
    if (pages == 0)                  { reply_err(ERR_LEN);    return; }
    if ((addr % PAGE_SIZE) != 0 || addr >= APROM_END
        || (addr + (uint32_t)pages * PAGE_SIZE) > APROM_END) {
        reply_err(ERR_ADDR);
        return;
    }
    if (!isp_prepare())              { reply_err(ERR_PREP);   return; }

    for (uint32_t p = 0; p < pages; p++) {
        uint32_t a = addr + p * PAGE_SIZE;
        if (isp_trigger(ISPCMD_ERASE, a, 0) != 0) {
            show_chunk(a, (uint16_t)PAGE_SIZE, "ERS");
            reply_err(ERR_ERASE);
            return;
        }
    }

    show_chunk(addr, pages, "ers");
    reply('K');
}

/*
 * ── 'D' 讀回一塊 ─────────────────────────────────────────────────────────
 *
 * 'D' addr[4 LE] len[2 LE] → 'K' + data[len] + crc16[2 LE]，或 'E' + 碼。
 *
 * **為什麼讀比寫更早該有：** 今天蓋掉 C2 的 keypad_lcd 時，靠的是「原始碼在
 * repo 裡」這個運氣。換成一塊原廠韌體沒有原始碼的板子，只會寫不會讀的工具會
 * 直接把它毀掉而且無法還原。stm32 那輪的結論是「**全備份把不可逆降級成可還原**」，
 * 這條保險 N2 一直缺著。
 *
 * 位址不設限，跟寫入那邊不同 —— **讀取不改變任何東西**，安全規則管的是寫。
 * 不設限反而讓它能當 peek 用（讀 CONFIG 存檔、讀週邊暫存器除錯）。
 */
static void cmd_read(void)
{
    uint8_t b;
    uint32_t addr = 0;
    uint16_t len = 0, crc = 0xFFFFu;

    for (int i = 0; i < 4; i++) {
        if (!rx_byte(&b)) { reply_err(ERR_TIMEOUT); return; }
        addr |= (uint32_t)b << (8 * i);
    }
    for (int i = 0; i < 2; i++) {
        if (!rx_byte(&b)) { reply_err(ERR_TIMEOUT); return; }
        len |= (uint16_t)b << (8 * i);
    }

    if (len == 0 || len > CHUNK_MAX || (len % 4) != 0 || (addr % 4) != 0) {
        reply_err(ERR_LEN);
        return;
    }
    if (!connected) {
        reply_err(ERR_NOLINK);
        return;
    }
    if (!ensure_halt()) {               /* 讀也要停住 —— 邊跑邊讀會讀到不一致的
                                         * 快照，而那種錯誤最難查：值是真的，
                                         * 只是彼此不屬於同一個時刻。 */
        reply_err(ERR_PREP);
        return;
    }

    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t v = 0;
        if (swd_mem_read(addr + i, &v) != 1) {
            reply_err(ERR_VERIFY);
            return;
        }
        buf[i]     = (uint8_t)v;
        buf[i + 1] = (uint8_t)(v >> 8);
        buf[i + 2] = (uint8_t)(v >> 16);
        buf[i + 3] = (uint8_t)(v >> 24);
    }

    show_chunk(addr, len, "rd ");       /* 慢動作做完才開始送，理由同 ACK */

    reply('K');
    for (uint16_t i = 0; i < len; i++) {
        uart0_putc(buf[i]);
        crc = crc16(crc, buf[i]);
    }
    uart0_putc((uint8_t)crc);
    uart0_putc((uint8_t)(crc >> 8));
    ok_chunks++;
}

/* ── 'W' 封包 ─────────────────────────────────────────────────────────── */
static void cmd_write(void)
{
    uint8_t b;
    uint32_t addr = 0;
    uint16_t len = 0, crc = 0xFFFFu, want = 0;

    for (int i = 0; i < 4; i++) {
        if (!rx_byte(&b)) { reply_err(ERR_TIMEOUT); return; }
        addr |= (uint32_t)b << (8 * i);
        crc = crc16(crc, b);
    }
    for (int i = 0; i < 2; i++) {
        if (!rx_byte(&b)) { reply_err(ERR_TIMEOUT); return; }
        len |= (uint16_t)b << (8 * i);
        crc = crc16(crc, b);
    }

    /* 長度先驗：它決定接下來要收幾個 byte，錯了整個狀態機就跟著跑掉。 */
    if (len == 0 || len > CHUNK_MAX || (len % 4) != 0) {
        show_chunk(addr, len, "LEN");
        reply_err(ERR_LEN);
        return;
    }

    for (uint16_t i = 0; i < len; i++) {
        if (!rx_byte(&b)) { reply_err(ERR_TIMEOUT); return; }
        buf[i] = b;
        crc = crc16(crc, b);
    }
    for (int i = 0; i < 2; i++) {
        if (!rx_byte(&b)) { reply_err(ERR_TIMEOUT); return; }
        want |= (uint16_t)b << (8 * i);
    }

    if (crc != want) {
        show_chunk(addr, len, "CRC");
        reply_err(ERR_CRC);
        return;
    }

    /* 位址檢查擋在這裡，PC 端腳本說什麼都不算數。
     * 條件刻意寫成「整塊都要落在 APROM 內」而不是只驗起點。 */
    if ((addr % 4) != 0 || addr >= APROM_END || (addr + len) > APROM_END) {
        show_chunk(addr, len, "ADR");
        reply_err(ERR_ADDR);
        return;
    }

    if (!armed) {                       /* Stage b：驗完就好，完全不碰 target */
        ok_chunks++;
        show_chunk(addr, len, "dry");
        reply('D');
        return;
    }

    if (!connected) {
        show_chunk(addr, len, "LNK");
        reply_err(ERR_NOLINK);
        return;
    }

    uint8_t e = write_chunk(addr, len);
    if (e) {
        show_chunk(addr, len, "ERR");
        reply_err(e);
        return;
    }

    ok_chunks++;
    show_chunk(addr, len, "ok ");
    reply('K');
}

int main(void)
{
    SYS_UnlockReg();
    CLK_EnableModuleClock(GPIO_MODULE);
    SYS_LockReg();

    swd_pins_init();
    lcd_init();
    kp_init();
    kp_poll_init();
    uart0_init(BAUD);

    lcd_goto(0, 0);
    lcd_puts("FLASH ISP N2    ");
    lcd_goto(1, 0);
    connected = (swd_connect() == 0);
    if (connected) { lcd_puts("DP:"); lcd_puthex(swd_last_dpidr); }
    else           { lcd_puts("SWD connect fail"); }
    uart0_puts("\r\nnano130 flash_isp ready\r\n");
    delay_ms(1200);

    lcd_clear();
    show_arm();

    char held = 0;

    /*
     * keypad 用非阻塞的 kp_poll()，所以主迴圈每輪都掃也不會吃掉 UART 的資料。
     *
     * **這一段先前踩了三次才對，值得記住走過的彎路：**
     *   1. 每輪呼叫阻塞式 kp_scan()（7~8 ms）→ host 一送下一塊就掉 byte，
     *      ERR_TIMEOUT。
     *   2. 改成「UART 靜 33 ms 才掃、掃完歸零」→ 變成每 33 ms 插隊一次，
     *      host 端任何較長的空檔（抹完 246 頁後印訊息、PowerShell 算 CRC）都中獎。
     *   3. 改成「不歸零、持續掃」→ **更糟**，盲窗從偶爾變成每 8 ms 一次。
     *   4. 門檻拉到 3 秒 → 傳輸安全了，但按鍵要按住一秒才吃得到，很不直覺。
     *
     * 前四次都在調「什麼時候可以掃」，而真正的病是**掃描本身會阻塞**。
     * kp_poll() 一次只取樣一列、用 SysTick 計時而不 busy-wait，單次幾十 µs，
     * 遠小於 16 byte FIFO 在 115200 下的約 1.4 ms 餘裕 —— 於是「什麼時候掃」
     * 這個問題整個消失了。
     *
     * （更徹底的是把 UART 改成中斷收進環形緩衝區。那是對的，但現在的瓶頸在
     *   bit-bang SWD 不在 UART，等真要提速或改走原生 USB 再做。）
     */
    uint32_t idle = 0;

    for (;;) {
        if (uart0_rx_ready()) {
            uint8_t c = (uint8_t)UART0->RBR;
            idle = 0;
            switch (c) {
            case 'P': reply('P'); break;
            case 'W': cmd_write(); break;
            case 'D': cmd_read();  break;
            case 'E': cmd_erase(); break;
            case 'H': reply(connected && ensure_halt() ? 'K' : 'E'); break;
            case 'R': reply(connected && swd_run() == 1 ? 'K' : 'E'); break;
            case 'S': reply(target_reset() ? 'K' : 'E'); break;   /* 燒完要用這個 */
            default:  break;            /* 雜訊直接丟掉，不要回應 —— 浮空的
                                         * UART 會冒出假 byte（Stage a 實測），
                                         * 對每個雜訊回一個 byte 只會製造更多雜訊 */
            }
            continue;                   /* 處理完立刻回頭看有沒有下一個 byte */
        }

        /* 掃描每輪都做（便宜），但**按鍵動作要等 UART 安靜約 50 ms 才執行**。
         *
         * 這兩件事必須分開，2026-09-15 的對抗測試才看清楚：把 kp_scan 換成
         * 非阻塞之後，傳輸途中狂按鍵仍然會掉資料 —— 因為慢的不再是掃描，
         * 是**按下去之後做的事**（按 `*` 會跑一次 swd_connect 加 1.2 秒 LCD，
         * 按 `0` 會重畫整行）。那些動作本來就慢，也沒必要變快。
         *
         * 而且這樣語意才對：**燒錄途中本來就不該去重連 SWD 或改保險狀態。**
         * 塊與塊之間的空檔只有幾毫秒，所以傳輸中永遠不會觸發；人在桌邊操作時
         * 50 ms 根本感覺不到。 */
        if (idle < 60000u) {            /* 約 50 ms */
            idle++;
            kp_poll();                  /* 還是要掃，否則放開/按下會被漏掉 */
            continue;
        }

        char k = kp_poll();
        if (k && !held) {
            held = k;
            switch (k) {
            case '0':                   /* 保險切換：燒整支 bin 要連續好幾十塊 */
                armed = !armed;
                show_arm();
                break;
            case '9':
                lcd_goto(0, 0);
                lcd_puts(connected && swd_run() == 1 ? "TARGET RUNNING  " : "run fail        ");
                break;
            case '8':
                lcd_goto(0, 0);
                lcd_puts(target_reset() ? "TARGET RESET    " : "reset fail      ");
                break;
            case '#':
                lcd_goto(0, 0);
                lcd_puts(connected && ensure_halt() ? "HALTED          " : "halt fail       ");
                break;
            case '*':
                /* 重連一定要有可見回饋。第一版按完只呼叫 show_arm()，印的是
                 * 同一行字 —— 狀態沒變時畫面一個字都不動，"成功" 和 "沒按到"
                 * 長得完全一樣。這種介面今天已經害我們誤判過一次（LCD 把
                 * 錯誤碼擠出螢幕那次），不要再留第二個。 */
                lcd_goto(0, 0);
                lcd_puts("RECONNECT...    ");
                connected = (swd_connect() == 0);
                lcd_goto(0, 0);
                if (connected) { lcd_puts("DP:"); lcd_puthex(swd_last_dpidr); lcd_puts("   "); }
                else           { lcd_puts("CONNECT FAIL    "); }
                delay_ms(1200);
                show_arm();
                break;
            case 'D':                   /* 收工：ISPCON 清乾淨、SYS 重新上鎖 */
                swd_mem_write(T_ISPCON, ISPCON_ISPFF);
                swd_mem_write(T_REGLOCK, 0x00u);
                armed = 0;
                lcd_goto(0, 0);
                lcd_puts("RELOCKED        ");
                break;
            case 'A':                   /* 計數：這一輪收了幾塊、錯幾塊 */
                lcd_goto(1, 0);
                lcd_puts("ok:");
                put_hex16((uint16_t)ok_chunks);
                lcd_puts(" er:");
                put_hex16((uint16_t)err_chunks);
                lcd_puts("  ");
                break;
            default: break;
            }
        } else if (!k) {
            held = 0;
        }
    }
}
