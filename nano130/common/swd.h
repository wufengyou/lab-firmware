/*
 * common/swd.h —— header-only 的 bit-bang SWD master（給客製版當 debugger 用）
 *
 * master 腳位（客製版 J17 CON8 排針）：
 *   SWCLK = PA.8   SWDIO = PA.9   nRESET = PA.10（目前未用）
 * 接到 target 的 J4："1=3V3  2=SWDAT  3=SWCLK  4=GND"，共地。
 *
 * 提供：
 *   swd_pins_init()          設好 GPIO
 *   swd_connect()            line reset + JTAG-to-SWD + 讀 DPIDR + 電源握手 + 選 AP0
 *   swd_mem_read(addr,&val)  透過 MEM-AP 讀一個 32-bit 字
 *   swd_last_dpidr           最後一次讀到的 DPIDR
 *
 * bit 時序照 CMSIS-DAP 參考實作：資料在 SWCLK 低準位變動，target 上緣取樣。
 */
#pragma once
#include "Nano100Series.h"
#include "delay.h"

#define SWD_SWCLK   8u
#define SWD_SWDIO   9u
#define SWD_NRST    10u

#define SWD_DLY()   do { volatile uint32_t d = 6; while (d--) __NOP(); } while (0)

static uint32_t swd_last_dpidr;

static inline void swd__clk_lo(void) { PA->DOUT &= ~(1u << SWD_SWCLK); }
static inline void swd__clk_hi(void) { PA->DOUT |=  (1u << SWD_SWCLK); }
static inline void swd__dio_hi(void) { PA->DOUT |=  (1u << SWD_SWDIO); }
static inline void swd__dio_lo(void) { PA->DOUT &= ~(1u << SWD_SWDIO); }
static inline int  swd__dio_rd(void) { return (PA->PIN >> SWD_SWDIO) & 1u; }
static inline void swd__drive(void)  { GPIO_SetMode(PA, 1u << SWD_SWDIO, GPIO_PMD_OUTPUT); }
static inline void swd__float(void)  { GPIO_SetMode(PA, 1u << SWD_SWDIO, GPIO_PMD_INPUT); }

static inline void swd__wr(int b)
{
    if (b) swd__dio_hi(); else swd__dio_lo();
    swd__clk_lo(); SWD_DLY();
    swd__clk_hi(); SWD_DLY();
}

static inline int swd__rd(void)
{
    int b;
    swd__clk_lo(); SWD_DLY();
    b = swd__dio_rd();
    swd__clk_hi(); SWD_DLY();
    return b;
}

static inline void swd__pulse(void)          /* turnaround：純 clock 一拍 */
{
    swd__clk_lo(); SWD_DLY();
    swd__clk_hi(); SWD_DLY();
}

static inline void swd__wr_seq(uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        swd__wr((v >> i) & 1u);
}

static inline void swd_pins_init(void)
{
    GPIO_SetMode(PA, (1u << SWD_SWCLK) | (1u << SWD_SWDIO) | (1u << SWD_NRST), GPIO_PMD_OUTPUT);
    PA->PUEN |= (1u << SWD_SWDIO);            /* float 時把 SWDIO 拉高 */
    PA->DOUT |=  (1u << SWD_NRST);
    PA->DOUT &= ~(1u << SWD_SWCLK);
    swd__dio_lo();
}

static inline void swd__line_reset_and_switch(void)
{
    swd__drive();
    swd__wr_seq(0xFFFFFFFFu, 32);
    swd__wr_seq(0xFFFFFFFFu, 24);             /* 共 56 個高 = line reset */
    swd__wr_seq(0xE79Eu, 16);                 /* JTAG-to-SWD，LSB first */
    swd__wr_seq(0xFFFFFFFFu, 32);
    swd__wr_seq(0xFFFFFFFFu, 24);             /* 再一次 line reset */
    swd__dio_lo();
    swd__wr_seq(0, 8);                        /* idle */
}

/*
 * 一次 DP/AP 傳輸。reg 只用 bit2/bit3（暫存器位址 0x0/0x4/0x8/0xC）。
 * 回傳 ACK：1=OK、2=WAIT(已重試)、4=FAULT、7/0=無回應、-1=parity 錯。
 * 讀：*data 帶回值；寫：*data 是要寫的值。
 */
static inline int swd_xfer(uint32_t apndp, uint32_t rnw, uint32_t reg, uint32_t *data)
{
    uint32_t a2 = (reg >> 2) & 1u, a3 = (reg >> 3) & 1u;
    uint32_t par = (apndp ^ rnw ^ a2 ^ a3) & 1u;
    uint32_t req = 1u | (apndp << 1) | (rnw << 2) | (a2 << 3) | (a3 << 4) | (par << 5) | (1u << 7);

    for (int retry = 0; retry < 16; retry++) {
        swd__drive();
        swd__wr_seq(req, 8);
        swd__float();
        swd__pulse();                        /* trn */

        int ack = 0;
        for (int i = 0; i < 3; i++)
            ack |= swd__rd() << i;

        if (ack == 1) {
            if (rnw) {
                uint32_t d = 0;
                for (int i = 0; i < 32; i++)
                    d |= (uint32_t)swd__rd() << i;
                int p = swd__rd();
                swd__pulse();                /* trn，host 收回 */
                swd__drive(); swd__dio_lo();
                if ((int)(__builtin_popcount(d) & 1) != p)
                    return -1;
                *data = d;
            } else {
                swd__pulse();                /* trn，host 收回 */
                swd__drive();
                uint32_t d = *data;
                for (int i = 0; i < 32; i++)
                    swd__wr((d >> i) & 1u);
                swd__wr(__builtin_popcount(d) & 1);
                swd__dio_lo();
            }
            return 1;
        }

        if (ack == 2) {                      /* WAIT：重試 */
            swd__pulse();
            swd__drive(); swd__dio_lo();
            delay_us(50);
            continue;
        }

        swd__pulse();                        /* FAULT / 無回應 */
        swd__drive(); swd__dio_lo();
        return ack;
    }
    return 2;
}

static inline int swd__dp_rd(uint32_t reg, uint32_t *v) { return swd_xfer(0, 1, reg, v); }
static inline int swd__dp_wr(uint32_t reg, uint32_t v)  { return swd_xfer(0, 0, reg, &v); }
static inline int swd__ap_wr(uint32_t reg, uint32_t v)  { return swd_xfer(1, 0, reg, &v); }

/* AP 讀是 posted：先 ap_rd 觸發，再讀 DP RDBUFF(0xC) 取真值 */
static inline int swd__ap_rd(uint32_t reg, uint32_t *v)
{
    int a = swd_xfer(1, 1, reg, v);
    if (a != 1)
        return a;
    return swd__dp_rd(0xC, v);
}

/* Stage 1：只做 line reset + 切換 + 讀 DPIDR。回傳 ACK（1=OK）*/
static inline int swd_read_idcode(uint32_t *id)
{
    swd__line_reset_and_switch();
    int a = swd__dp_rd(0x0, id);
    if (a == 1)
        swd_last_dpidr = *id;
    return a;
}

/* 回傳 0 成功；負值失敗（-1 DPIDR、-2 電源握手、-3 其他）*/
static inline int swd_connect(void)
{
    uint32_t v;

    swd__line_reset_and_switch();

    if (swd__dp_rd(0x0, &swd_last_dpidr) != 1)   /* 讀 DPIDR 才會啟用 DP */
        return -1;

    swd__dp_wr(0x0, 0x1Eu);                      /* ABORT：清 sticky error */
    swd__dp_wr(0x4, 0x50000000u);               /* CTRL/STAT：CSYS/CDBG PWRUPREQ */

    int ok = 0;
    for (int i = 0; i < 100; i++) {
        if (swd__dp_rd(0x4, &v) == 1 && (v & 0xA0000000u) == 0xA0000000u) {
            ok = 1;
            break;
        }
        delay_ms(1);
    }
    if (!ok)
        return -2;

    if (swd__dp_wr(0x8, 0x00000000u) != 1)      /* SELECT：AP0 / bank0 */
        return -3;

    /* CSW：read-modify-write，只把 Size 設成 word，其餘 Prot 位維持晶片預設 */
    if (swd__ap_rd(0x0, &v) != 1)
        return -3;
    v = (v & ~0x7u) | 0x2u;
    if (swd__ap_wr(0x0, v) != 1)
        return -3;

    return 0;
}

static inline int swd_mem_read(uint32_t addr, uint32_t *out)
{
    int a = swd__ap_wr(0x4, addr);              /* TAR */
    if (a == 1)
        a = swd__ap_rd(0xC, out);              /* DRW（posted，內含 RDBUFF）*/
    if (a != 1)
        swd__dp_wr(0x0, 0x1Eu);                /* 失敗後清 sticky error，工具才能繼續 */
    return a;
}

static inline int swd_mem_write(uint32_t addr, uint32_t val)
{
    int a = swd__ap_wr(0x4, addr);              /* TAR */
    if (a == 1)
        a = swd__ap_wr(0xC, val);             /* DRW */
    if (a != 1)
        swd__dp_wr(0x0, 0x1Eu);
    return a;
}

/* ── Cortex-M debug：halt / step / run / 讀核心暫存器 ────────────────
 * 全部透過 MEM-AP 戳 SCS 裡的 Debug Core Block（0xE000EDF0 起）。       */
#define SWD_DHCSR   0xE000EDF0u
#define SWD_DCRSR   0xE000EDF4u
#define SWD_DCRDR   0xE000EDF8u
#define SWD_DBGKEY  0xA05F0000u                 /* 寫 DHCSR 必帶的 key */

/* C_DEBUGEN=bit0 C_HALT=bit1 C_STEP=bit2 C_MASKINTS=bit3
 * 狀態（讀）：S_REGRDY=bit16 S_HALT=bit17 S_SLEEP=bit18 S_LOCKUP=bit19 */
static inline int swd_halt(void) { return swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0x3u); }

/* ⚠ step / run 都必須拆成兩次寫入，不能各寫一次了事。
 *
 * ARMv7-M B1.5.15：**對 C_MASKINTS 的寫入會被忽略，除非該次寫入同時維持
 * C_HALT = 1**。而 step（放 CPU 跑一條）和 run（放它一直跑）本來就都必須把
 * C_HALT 清掉，所以「C_STEP／C_HALT 的變化和 C_MASKINTS 一起寫」在架構上
 * 不可能成立 —— 那次對 C_MASKINTS 的寫入會被硬體整個丟掉，而且不報錯。
 *
 * 兩個後果，第二個比第一個嚴重：
 *   step 只寫 0xD：C_MASKINTS 從來沒設進去，單步時中斷沒被遮，會踩進 ISR。
 *   run  只寫 0x1：C_MASKINTS 也清不掉，**step 過一次之後就永遠卡在 1**，
 *                  於是 target 是在中斷被遮的狀態下跑的 —— SysTick 不來、
 *                  週邊中斷不進 ISR，看起來像 target 當掉，但 DHCSR 顯示 RUN、
 *                  一切「正常」。
 *
 * 這一條是 2026-09-11 在 stm32/ 那邊上機才抓到的（見 stm32/README.md 同名小節），
 * 當時沒搬回來，2026-09-14 補上。**驗收位元是 DHCSR 的 bit 3**：修好之後
 * step 完低半字 = 0xF、run 完 = 0x1；沒修是 0x7 / 0x9。更有說服力的證據是
 * PC 的分佈 —— 修好之後每次 halt 落在完全不同的位址，代表 target 真的在整個
 * 程式裡跑；中斷被遮時每次都落在同一小段裡打轉。 */
static inline int swd_step(void)
{
    int a = swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0xBu);  /* DEBUGEN|HALT|MASKINTS */
    if (a != 1)
        return a;
    return swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0xDu);   /* DEBUGEN|STEP|MASKINTS */
}

static inline int swd_run(void)
{
    int a = swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0x3u);  /* DEBUGEN|HALT，趁 HALT=1 清掉 MASKINTS */
    if (a != 1)
        return a;
    return swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0x1u);   /* DEBUGEN，放它跑 */
}

static inline int swd_dhcsr(uint32_t *v) { return swd_mem_read(SWD_DHCSR, v); }

/* regsel：0-15 = R0-R15（15=PC）、16 = xPSR、17 = MSP、18 = PSP */
static inline int swd_core_reg(uint32_t regsel, uint32_t *out)
{
    int a = swd_mem_write(SWD_DCRSR, regsel & 0x1Fu);   /* REGWnR=0 → 讀 */
    if (a != 1)
        return a;
    uint32_t d = 0;
    for (int i = 0; i < 60; i++) {
        a = swd_mem_read(SWD_DHCSR, &d);
        if (a == 1 && (d & (1u << 16)))                 /* S_REGRDY */
            break;
        delay_us(100);
    }
    return swd_mem_read(SWD_DCRDR, out);
}
