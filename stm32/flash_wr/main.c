/*
 * stm32/flash_wr —— Stage 3.5：用 SWD 燒 target 的 flash
 *
 * 到 Stage 3 為止，我們的 debugger 只會「讀」和「控制 CPU」。這一支跨過那道門檻：
 * 解鎖 target 的 flash 控制器、抹一頁、寫進去、讀回來驗證。做完之後 master
 * 就具備一台燒錄器的核心能力 —— 這正是 ST-LINK 在做的事。
 *
 * 接線跟 swd_probe / cpu_ctrl 完全相同（三條線），一條都不用加。
 *
 * ── 為什麼這一支比前面三個 Stage 危險 ──
 *
 * 前面全部是唯讀或只動除錯暫存器（斷電就恢復）。這一支會**不可逆地改變 target
 * 的 flash 內容**。所以有三道保險，每一道都是刻意的：
 *
 *   1. 只碰 bank 2 的最後一頁（0x080FF800）。原廠 demo 在 bank 1 前段，動不到。
 *   2. 絕不寫 FLASH_OPTKEYR（0x40022 00C）與 option bytes。抹一頁 flash 可以再燒
 *      回去，把 option bytes（尤其 RDP 讀保護等級）寫壞則可能救不回來。
 *      **本檔案從頭到尾只寫 KEYR(0x08) / SR(0x10) / CR(0x14) 這三個暫存器。**
 *   3. 動手前先驗 dev_id == 0x415。F4 的 flash 控制器在 0x40023C00、L4 在
 *      0x40022000 —— 接錯板子時若照 L4 的位址亂寫，等於對 F446 的隨機週邊下令。
 *      這是 Stage 2 那條「晶片資料庫」的延伸：debugger 每多一個功能，
 *      就要多認得一點晶片。
 *
 * ── L4 flash 的三個特性（跟 F4 不一樣，踩之前先知道）──
 *
 *   - **只能寫雙字（64-bit）**，而且位址要 8-byte 對齊。寫單字硬體會報 SIZERR。
 *   - **抹除以「頁」為單位，一頁 2 KB**（F4 是大小不一的 sector）。
 *   - 1 MB 的 L476 是**雙 bank**：bank1 0x08000000、bank2 0x08080000，各 512 KB。
 *     抹除時 PNB 是 bank 內的頁號（0-255），另外要用 BKER 指定哪一個 bank。
 *
 * 指令（終端機直接按鍵，不用 Enter）：
 *   c  connect      重新連線並驗晶片身分
 *   ?  status       讀 FLASH_SR / FLASH_CR，顯示鎖定狀態
 *   v  verify       印出目標頁開頭 4 個字 + bank1 開頭的「哨兵」
 *   u  unlock       解鎖 flash（KEY1/KEY2）
 *   k  lock         重新上鎖
 *   e  erase        抹掉目標頁（會再問一次 y/n）
 *   w  write        寫入簽章雙字（會再問一次 y/n）
 */
#include "stm32f446.h"
#include "delay.h"
#include "uart.h"
#include "swd.h"

/* ── target（STM32L476）的 flash 控制器 ──
 * RM0351 §3.7。**只用到這三個位址，其他一律不碰** —— 特別是 0x0C 的 OPTKEYR。 */
#define L4_FLASH_BASE   0x40022000u
#define L4_FLASH_KEYR   (L4_FLASH_BASE + 0x08u)
#define L4_FLASH_SR     (L4_FLASH_BASE + 0x10u)
#define L4_FLASH_CR     (L4_FLASH_BASE + 0x14u)

#define L4_KEY1         0x45670123u
#define L4_KEY2         0xCDEF89ABu

/* FLASH_CR */
#define CR_PG           (1u << 0)
#define CR_PER          (1u << 1)
#define CR_PNB_SHIFT    3u
#define CR_BKER         (1u << 11)
#define CR_STRT         (1u << 16)
#define CR_LOCK         (1u << 31)

/* FLASH_SR */
/* ⚠ EOP 在 L4 上**只有 FLASH_CR 的 EOPIE=1 時才會被設**（RM0351；F4 是無條件設）。
 * 我們不開 EOPIE —— 那會讓 target 的 flash 中斷線變成可觸發狀態，而 target 的 NVIC
 * 是原廠韌體在管的，為了一行提示訊息去動它不划算。
 * **真正的驗證一律是讀回來比對**，EOP 只是附帶資訊。
 * 2026-09-11 實測：寫入與抹除都成功，但 EOP 全程是 0。 */
#define SR_EOP          (1u << 0)
#define SR_BSY          (1u << 16)
/* 所有錯誤旗標（OPERR/PROGERR/WRPERR/PGAERR/SIZERR/PGSERR/MISERR/FASTERR/RDERR/OPTVERR）。
 * 這些是「寫 1 清除」的 —— 動作前要先清乾淨，否則分不出是這次的錯還是上次殘留的。 */
#define SR_ERR_MASK     0xC3FAu

/* 目標頁：bank2 的最後一頁。1 MB / 2 KB = 512 頁，bank2 是後 256 頁，頁號 0-255。 */
#define TGT_ADDR        0x080FF800u
#define TGT_BANK        1u      /* BKER=1 -> bank2 */
#define TGT_PNB         255u

/* 哨兵：bank1 的開頭（reset vector 那一段）。抹除前後各讀一次，
 * 如果它變了，代表 PNB/BKER 算錯、抹到了不該抹的地方 —— 救不回來，但至少要知道。 */
#define CANARY_ADDR     0x08000000u

static int connected;
static int chip_ok;             /* dev_id 驗過才是 1 */
static uint32_t canary[4];
static int canary_valid;

static void put2(const char *s, uint32_t v)
{
    uart_puts(s);
    uart_puthex(v);
    uart_puts("\r\n");
}

/* ── 等 flash 不忙 ──
 * 逾時是必要的：如果 target 根本沒在回應，輪詢 BSY 會變成無窮迴圈，
 * 終端機看起來就是「按了沒反應」。回傳 1 = 好了，0 = 逾時或 SWD 斷。 */
static int flash_wait(uint32_t *sr_out)
{
    for (int i = 0; i < 20000; i++) {
        uint32_t sr = 0;
        if (swd_mem_read(L4_FLASH_SR, &sr) != 1) {
            uart_puts("  SWD ERR（讀 FLASH_SR）\r\n");
            connected = 0;
            return 0;
        }
        if (!(sr & SR_BSY)) {
            if (sr_out)
                *sr_out = sr;
            return 1;
        }
    }
    uart_puts("  逾時：FLASH_SR 的 BSY 一直是 1\r\n");
    return 0;
}

/* 清掉 SR 的錯誤旗標與 EOP（寫 1 清除）。 */
static int flash_clear_sr(void)
{
    return swd_mem_write(L4_FLASH_SR, SR_ERR_MASK | SR_EOP) == 1;
}

static void show_sr_errors(uint32_t sr)
{
    if (!(sr & SR_ERR_MASK))
        return;
    uart_puts("  ⚠ FLASH_SR 有錯誤旗標：");
    uart_puthex(sr & SR_ERR_MASK);
    uart_puts("\r\n");
    if (sr & (1u << 3))  uart_puts("     PROGERR —— 寫到不是抹除狀態的位置\r\n");
    if (sr & (1u << 4))  uart_puts("     WRPERR  —— 寫保護\r\n");
    if (sr & (1u << 5))  uart_puts("     PGAERR  —— 位址沒有 8-byte 對齊\r\n");
    if (sr & (1u << 6))  uart_puts("     SIZERR  —— 寫的不是雙字\r\n");
    if (sr & (1u << 7))  uart_puts("     PGSERR  —— 程式設計順序錯（CR 的位元沒設對）\r\n");
    if (sr & (1u << 8))  uart_puts("     MISERR  —— 雙字只寫了一半\r\n");
}

/* ── 驗晶片身分 ──
 * 這是動 flash 暫存器之前的**必要**關卡，不是保險而已：位址在兩個系列上完全不同。 */
static int check_chip(void)
{
    uint32_t id = 0;

    chip_ok = 0;
    if (swd_mem_read(0xE0042000u, &id) != 1) {
        uart_puts("  讀 DBGMCU_IDCODE 失敗\r\n");
        connected = 0;
        return 0;
    }

    uart_puts("  IDCODE = ");
    uart_puthex(id);
    uart_puts("  dev_id=");
    uart_puthex(id & 0xFFFu);
    uart_puts("\r\n");

    if ((id & 0xFFFu) != 0x415u) {
        uart_puts("  ⛔ 不是 STM32L4（期待 dev_id=0x415）。\r\n");
        uart_puts("     這支韌體的 flash 暫存器位址只對 L4 成立，拒絕往下做。\r\n");
        return 0;
    }

    chip_ok = 1;
    return 1;
}

static void do_connect(void)
{
    uint32_t id = 0;

    connected = 0;
    chip_ok = 0;
    canary_valid = 0;

    /* ⚠ swd.h 有兩套回傳慣例，混用會出事（2026-09-11 踩過，見 swd.h 開頭）：
     *   swd_read_idcode / swd_mem_read / swd_mem_write -> 成功是 1（SWD 的 ACK 值）
     *   swd_connect                                    -> **成功是 0**，負值才是錯誤碼
     * 本來寫成 swd_connect() != 1，於是「連線成功」被判成失敗 —— 症狀是接線明明
     * 沒動卻一直印連線失敗，會害人往硬體方向找。
     *
     * 順序也不能顛倒：swd_read_idcode 裡面做 line reset + JTAG-to-SWD 切換，
     * 沒先跑它就 swd_connect，DP 還沒進入 SWD 模式。 */
    int ack = -9;
    for (int t = 0; t < 4; t++) {
        ack = swd_read_idcode(&id);
        if (ack == 1)
            break;
        delay_ms(50);
    }
    if (ack != 1) {
        uart_puts("  no ACK —— 檢查三條線 / 共地 / CN3 跳線帽\r\n");
        return;
    }
    put2("  DPIDR = ", id);

    if (swd_connect() != 0) {
        uart_puts("  connect fail —— DP 上電或 AP 選擇失敗\r\n");
        return;
    }
    connected = 1;

    if (!check_chip())
        return;

    /* flash 操作期間讓 target 的 CPU 停住。它正在從 flash 執行，抹除／寫入會讓
     * 匯流排暫停，跑著的程式可能取到半途的指令。halt 掉最乾淨。 */
    if (swd_halt() == 1)
        uart_puts("  target 已 halt（flash 操作期間不讓它跑）\r\n");
    else
        uart_puts("  ⚠ halt 失敗，仍可繼續，但建議先查清楚\r\n");
}

static int need_ready(void)
{
    if (!connected || !chip_ok) {
        uart_puts("  還沒連上或晶片身分沒驗過 —— 先按 c\r\n");
        return 0;
    }
    return 1;
}

/* ── 讀哨兵 ── */
static int read_canary(uint32_t *dst)
{
    for (int i = 0; i < 4; i++) {
        if (swd_mem_read(CANARY_ADDR + 4u * (uint32_t)i, &dst[i]) != 1) {
            connected = 0;
            return 0;
        }
    }
    return 1;
}

static void do_verify(void)
{
    uint32_t v[4], sr = 0, cr = 0;

    if (!need_ready())
        return;

    uart_puts("  目標頁 ");
    uart_puthex(TGT_ADDR);
    uart_puts("：\r\n");
    for (int i = 0; i < 4; i++) {
        if (swd_mem_read(TGT_ADDR + 4u * (uint32_t)i, &v[i]) != 1) {
            uart_puts("  讀取失敗\r\n");
            connected = 0;
            return;
        }
        uart_puts("    +");
        uart_puthex8(4u * (uint32_t)i);
        uart_puts(" = ");
        uart_puthex(v[i]);
        uart_puts("\r\n");
    }
    if (v[0] == 0xFFFFFFFFu && v[1] == 0xFFFFFFFFu)
        uart_puts("    （全 1 = 已抹除狀態）\r\n");

    if (read_canary(v)) {
        uart_puts("  哨兵 ");
        uart_puthex(CANARY_ADDR);
        uart_puts(" = ");
        uart_puthex(v[0]);
        uart_puts(" ");
        uart_puthex(v[1]);
        if (canary_valid) {
            if (v[0] == canary[0] && v[1] == canary[1] &&
                v[2] == canary[2] && v[3] == canary[3])
                uart_puts("   ✓ 沒被動到\r\n");
            else
                uart_puts("   ⛔ 變了！抹到不該抹的地方\r\n");
        } else {
            uart_puts("   （基準已記錄）\r\n");
            for (int i = 0; i < 4; i++)
                canary[i] = v[i];
            canary_valid = 1;
        }
    }

    if (swd_mem_read(L4_FLASH_SR, &sr) == 1 && swd_mem_read(L4_FLASH_CR, &cr) == 1) {
        put2("  FLASH_SR = ", sr);
        put2("  FLASH_CR = ", cr);
        uart_puts((cr & CR_LOCK) ? "  flash 是鎖著的\r\n" : "  flash 已解鎖\r\n");
        show_sr_errors(sr);
    }
}

static void do_status(void)
{
    uint32_t sr = 0, cr = 0;

    if (!need_ready())
        return;
    if (swd_mem_read(L4_FLASH_SR, &sr) != 1 || swd_mem_read(L4_FLASH_CR, &cr) != 1) {
        uart_puts("  讀取失敗\r\n");
        connected = 0;
        return;
    }
    put2("  FLASH_SR = ", sr);
    put2("  FLASH_CR = ", cr);
    uart_puts((cr & CR_LOCK) ? "  LOCK=1（鎖著）\r\n" : "  LOCK=0（已解鎖）\r\n");
    show_sr_errors(sr);
}

/* ── 解鎖 ──
 * KEY1、KEY2 依序寫進 FLASH_KEYR。**順序錯或寫錯值，硬體會把 flash 鎖到下次重置
 * 為止**（而且不報錯），所以寫完一定要讀 CR 確認 LOCK 真的變 0 —— 這跟 DHCSR
 * 那個 DBGKEY 是同一類的坑：寫入被丟掉但沒有任何錯誤回報。 */
static void do_unlock(void)
{
    uint32_t cr = 0;

    if (!need_ready())
        return;

    if (swd_mem_read(L4_FLASH_CR, &cr) != 1) {
        uart_puts("  讀 FLASH_CR 失敗\r\n");
        connected = 0;
        return;
    }
    if (!(cr & CR_LOCK)) {
        uart_puts("  本來就是解鎖的\r\n");
        return;
    }

    if (swd_mem_write(L4_FLASH_KEYR, L4_KEY1) != 1 ||
        swd_mem_write(L4_FLASH_KEYR, L4_KEY2) != 1) {
        uart_puts("  寫 KEYR 失敗\r\n");
        connected = 0;
        return;
    }

    if (swd_mem_read(L4_FLASH_CR, &cr) != 1) {
        connected = 0;
        return;
    }
    if (cr & CR_LOCK) {
        uart_puts("  ⛔ 解鎖失敗，LOCK 還是 1\r\n");
        uart_puts("     KEYR 的序列一旦寫錯，硬體會鎖到下次重置為止 —— 按 target 的 RESET 再試\r\n");
    } else {
        uart_puts("  ✓ 已解鎖（LOCK=0）\r\n");
    }
}

static void do_lock(void)
{
    uint32_t cr = 0;

    if (!need_ready())
        return;
    if (swd_mem_read(L4_FLASH_CR, &cr) != 1) {
        connected = 0;
        return;
    }
    if (swd_mem_write(L4_FLASH_CR, cr | CR_LOCK) == 1)
        uart_puts("  已重新上鎖\r\n");
    else
        connected = 0;
}

/* 確認鍵。**不可逆的動作一律要過這一關** —— 單鍵介面太容易誤按。 */
static int confirm(const char *what)
{
    uart_puts("  ⚠ ");
    uart_puts(what);
    uart_puts("\r\n     這是不可逆的。要做請按 y，其他鍵取消：");
    for (;;) {
        int c = uart_getc_nb();
        if (c < 0)
            continue;
        uart_putc((char)c);
        uart_puts("\r\n");
        return c == 'y';
    }
}

static void do_erase(void)
{
    uint32_t sr = 0, cr, v = 0;

    if (!need_ready())
        return;

    if (swd_mem_read(L4_FLASH_CR, &cr) != 1) {
        connected = 0;
        return;
    }
    if (cr & CR_LOCK) {
        uart_puts("  flash 還鎖著 —— 先按 u\r\n");
        return;
    }

    /* 抹除前先把哨兵記下來。之後才有東西可以比對。 */
    if (!canary_valid) {
        if (!read_canary(canary)) {
            uart_puts("  讀哨兵失敗，不做\r\n");
            return;
        }
        canary_valid = 1;
    }

    if (!confirm("要抹掉 bank2 的最後一頁（0x080FF800，2 KB）"))
        { uart_puts("  取消\r\n"); return; }

    if (!flash_clear_sr()) { connected = 0; return; }

    /* PER + 頁號 + bank，然後 STRT。**PNB 是 bank 內的頁號，不是全域頁號** ——
     * 算錯就抹到別的地方，這正是哨兵要防的事。 */
    cr = CR_PER | (TGT_PNB << CR_PNB_SHIFT) | (TGT_BANK ? CR_BKER : 0u);
    if (swd_mem_write(L4_FLASH_CR, cr) != 1 ||
        swd_mem_write(L4_FLASH_CR, cr | CR_STRT) != 1) {
        uart_puts("  寫 FLASH_CR 失敗\r\n");
        connected = 0;
        return;
    }

    if (!flash_wait(&sr))
        return;

    show_sr_errors(sr);
    if (sr & SR_EOP) {
        swd_mem_write(L4_FLASH_SR, SR_EOP);
        uart_puts("  ✓ 抹除完成（EOP=1）\r\n");
    } else if (!(sr & SR_ERR_MASK)) {
        /* L4 沒開 EOPIE 就不會設 EOP，這是正常路徑，不是異常。 */
        uart_puts("  沒有錯誤旗標（EOP 要 EOPIE=1 才會亮，見檔頭）—— 按 v 讀回來確認\r\n");
    }

    /* PER 一定要清掉。留著的話下一次寫入會踩到 PGSERR（程式設計順序錯）。 */
    swd_mem_write(L4_FLASH_CR, 0u);

    if (read_canary(&v) || 1) {
        uint32_t now[4];
        if (read_canary(now)) {
            if (now[0] != canary[0] || now[1] != canary[1] ||
                now[2] != canary[2] || now[3] != canary[3])
                uart_puts("  ⛔⛔ 哨兵變了 —— 抹到了 bank1！立刻停手，不要再寫\r\n");
            else
                uart_puts("  哨兵未變 ✓（bank1 沒被動到）\r\n");
        }
    }
}

/* ── 寫入 ──
 * L4 只能寫雙字：設 PG，連續寫兩個 32-bit，硬體湊成 64-bit 一次寫進去。
 * 只寫一半會得到 MISERR。 */
static void do_write(void)
{
    uint32_t sr = 0, cr, v0 = 0, v1 = 0;

    if (!need_ready())
        return;

    if (swd_mem_read(L4_FLASH_CR, &cr) != 1) {
        connected = 0;
        return;
    }
    if (cr & CR_LOCK) {
        uart_puts("  flash 還鎖著 —— 先按 u\r\n");
        return;
    }

    /* 寫之前先確認那裡是抹除狀態。往非 0xFF 的位置寫，L4 會直接報 PROGERR ——
     * 與其讓硬體報錯，不如先講清楚為什麼。 */
    if (swd_mem_read(TGT_ADDR, &v0) != 1 || swd_mem_read(TGT_ADDR + 4u, &v1) != 1) {
        connected = 0;
        return;
    }
    if (v0 != 0xFFFFFFFFu || v1 != 0xFFFFFFFFu) {
        uart_puts("  那個雙字不是抹除狀態（");
        uart_puthex(v0);
        uart_puts(" ");
        uart_puthex(v1);
        uart_puts("）—— 先按 e 抹除\r\n");
        return;
    }

    if (!confirm("要在 0x080FF800 寫入簽章 0x5A5AF446 / 0x20260911"))
        { uart_puts("  取消\r\n"); return; }

    if (!flash_clear_sr()) { connected = 0; return; }

    if (swd_mem_write(L4_FLASH_CR, CR_PG) != 1 ||
        swd_mem_write(TGT_ADDR, 0x5A5AF446u) != 1 ||
        swd_mem_write(TGT_ADDR + 4u, 0x20260911u) != 1) {
        uart_puts("  寫入序列失敗\r\n");
        connected = 0;
        return;
    }

    if (!flash_wait(&sr))
        return;

    show_sr_errors(sr);
    if (sr & SR_EOP) {
        swd_mem_write(L4_FLASH_SR, SR_EOP);
        uart_puts("  ✓ 寫入完成（EOP=1）\r\n");
    }

    swd_mem_write(L4_FLASH_CR, 0u);     /* 清掉 PG */

    /* 讀回來才算數。EOP 只說「硬體做完了」，不說「內容是對的」。 */
    if (swd_mem_read(TGT_ADDR, &v0) == 1 && swd_mem_read(TGT_ADDR + 4u, &v1) == 1) {
        uart_puts("  讀回：");
        uart_puthex(v0);
        uart_puts(" ");
        uart_puthex(v1);
        if (v0 == 0x5A5AF446u && v1 == 0x20260911u)
            uart_puts("   ✓ 相符 —— 你的板子剛剛燒了另一顆晶片的 flash\r\n");
        else
            uart_puts("   ⛔ 不符\r\n");
    }
}

static void help(void)
{
    uart_puts("\r\n"
              "  c connect  ? status   v verify\r\n"
              "  u unlock   k lock     e erase    w write\r\n");
}

int main(void)
{
    dwt_init();
    uart_init();
    swd_pins_init();

    uart_puts("\r\n=== STM32 flash_wr（Stage 3.5：燒 target 的 flash）===\r\n");
    uart_puts("目標：0x080FF800（bank2 最後一頁）。bank1 的原廠 demo 不會被動到。\r\n");
    help();

    do_connect();
    uart_puts("> ");

    for (;;) {
        int c = uart_getc_nb();
        if (c < 0)
            continue;

        switch (c) {
        case 'c': do_connect(); break;
        case '?': do_status();  break;
        case 'v': do_verify();  break;
        case 'u': do_unlock();  break;
        case 'k': do_lock();    break;
        case 'e': do_erase();   break;
        case 'w': do_write();   break;

        case '\r':
        case '\n': break;

        default: help(); break;
        }
        uart_puts("> ");
    }
}
