/*
 * common/keypad.h —— WENG NANO 客製版的 4x4 矩陣 keypad（header-only）
 *
 *   列 R1-R4 = PC.0 - PC.3      （推挽輸出，掃描時逐一拉低）
 *   行 C1,C2 = PC.4, PC.5       （輸入 + 內建上拉，板上沒有外部上拉）
 *   行 C3,C4 = PD.15, PD.14
 *
 * 掃描 / delay 的坑見 nano130/README.md。呼叫前要先 CLK_EnableModuleClock(GPIO_MODULE)。
 */
#pragma once
#include "Nano100Series.h"
#include "delay.h"

static const char KP_MAP[4][4] = {
    { '1', '2', '3', 'A' },
    { '4', '5', '6', 'B' },
    { '7', '8', '9', 'C' },
    { '*', '0', '#', 'D' },
};

static inline void kp_init(void)
{
    GPIO_SetMode(PC, 0x000Fu, GPIO_PMD_OUTPUT);   /* 列 */
    PC->DOUT |= 0x0Fu;
    GPIO_SetMode(PC, 0x0030u, GPIO_PMD_INPUT);    /* 行 C1,C2 */
    GPIO_SetMode(PD, 0xC000u, GPIO_PMD_INPUT);    /* 行 C3,C4 */
    PC->PUEN |= 0x0030u;
    PD->PUEN |= 0xC000u;
}

static inline uint32_t kp__col(uint32_t c)
{
    switch (c) {
    case 0:  return (PC->PIN >> 4)  & 1u;
    case 1:  return (PC->PIN >> 5)  & 1u;
    case 2:  return (PD->PIN >> 15) & 1u;
    default: return (PD->PIN >> 14) & 1u;
    }
}

static inline char kp_raw(void)
{
    char found = 0;
    for (uint32_t r = 0; r < 4 && !found; r++) {
        PC->DOUT = (PC->DOUT | 0x0Fu) & ~(1u << r);
        delay_us(300);
        for (uint32_t c = 0; c < 4; c++)
            if (kp__col(c) == 0) { found = KP_MAP[r][c]; break; }
    }
    PC->DOUT |= 0x0Fu;
    return found;
}

/* 相隔 5 ms 讀到兩次一樣才算 */
static inline char kp_scan(void)
{
    char a = kp_raw();
    if (!a)
        return 0;
    delay_ms(5);
    return (kp_raw() == a) ? a : 0;
}

/* ── 非阻塞版（kp_poll）────────────────────────────────────────────────
 *
 * `kp_scan()` 一次呼叫要 7~8 ms（沒按鍵時也要 1.2 ms：4 列 × 300 µs settle），
 * 而 UART 在 115200 下，晶片那 16 byte 的接收 FIFO 只撐得住約 1.4 ms ——
 * **掃描期間收到的資料會直接掉**。2026-09-15 的 flash_isp 為此被迫把 keypad
 * 壓到「UART 靜 3 秒才掃」，代價是按鍵要按住一秒才吃得到，很不直覺。
 *
 * kp_poll() 把同一件事拆成每次只做一小步：**一次呼叫只取樣一列**，
 * 靠 SysTick 當自由計時器來計 settle 與去彈跳的時間，不再 busy-wait。
 * 單次耗時降到幾十 µs，FIFO 吃得下，兩邊就不必再互搶。
 *
 * 回傳語意跟 kp_scan() 一樣：**按住期間每次呼叫都回同一個鍵，放開回 0**，
 * 所以呼叫端那套 `held` 判斷不用改。
 */
#define KP__US(n)   ((n) * 12u)          /* HIRC 12 MHz：1 µs = 12 個 SysTick */
#define KP__SETTLE  KP__US(300)          /* 換列後的 settle，同 kp_raw() */
#define KP__DEBNC   KP__US(5000)         /* 5 ms 去彈跳，同 kp_scan() */

static uint32_t kp__t0;                  /* 上次換列的時間戳 */
static uint32_t kp__row;                 /* 目前驅動的列 */
static char     kp__cand;                /* 候選鍵（還沒過去彈跳） */
static uint32_t kp__cand_t;
static char     kp__stable;              /* 已確認、目前按著的鍵 */
static uint32_t kp__miss;                /* 連續幾列沒看到候選/穩定鍵 */

static inline uint32_t kp__now(void) { return SysTick->VAL; }

/* SysTick 是 24-bit 遞減計數器，自由運轉會繞回；相減後遮 24 bit 才對。
 * 12 MHz 下繞一圈約 1.4 秒 —— 只要呼叫間隔遠小於它就準。 */
static inline uint32_t kp__elapsed(uint32_t since)
{
    return (since - kp__now()) & 0xFFFFFFu;
}

static inline void kp_poll_init(void)
{
    SysTick->LOAD = 0xFFFFFFu;
    SysTick->VAL  = 0;
    SysTick->CTRL = 5u;                  /* ENABLE | CLKSOURCE=core，不開中斷 */
    kp__row = 0;
    PC->DOUT = (PC->DOUT | 0x0Fu) & ~1u; /* 先把第 0 列拉低 */
    kp__t0 = kp__now();
}

static inline char kp_poll(void)
{
    if (kp__elapsed(kp__t0) < KP__SETTLE)
        return kp__stable;               /* 還沒 settle，直接回目前狀態 */

    char k = 0;
    for (uint32_t c = 0; c < 4; c++)
        if (kp__col(c) == 0) { k = KP_MAP[kp__row][c]; break; }

    /* 換到下一列並重新計時。**這裡就返回**，取樣留給下一次呼叫 —— 這就是
     * 「非阻塞」的全部：把等待換成「下次再來」，而不是站在原地等。 */
    kp__row = (kp__row + 1u) & 3u;
    PC->DOUT = (PC->DOUT | 0x0Fu) & ~(1u << kp__row);
    kp__t0 = kp__now();

    if (k) {
        kp__miss = 0;
        if (k == kp__stable)
            return kp__stable;           /* 按著不動 */
        if (k == kp__cand) {
            if (kp__elapsed(kp__cand_t) >= KP__DEBNC)
                kp__stable = k;          /* 過了去彈跳，正式認定 */
        } else {
            kp__cand = k;
            kp__cand_t = kp__now();
        }
        return kp__stable;
    }

    /* 這一列沒鍵不代表放開了 —— 鍵可能在別列。掃完一整圈都沒看到才算放開。 */
    if (++kp__miss >= 4u) {
        kp__miss = 0;
        kp__cand = 0;
        kp__stable = 0;
    }
    return kp__stable;
}
