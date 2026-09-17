/*
 * common/swd.h（STM32F446 版）—— bit-bang SWD master
 *
 * 這是 nano130/common/swd.h 的移植：SWD/DAP 協定碼一字不改，只換掉腳位存取層
 * （Nuvoton GPIO -> STM32 GPIO）和 parity（不依賴 libgcc）。
 *
 * master 腳位（NUCLEO-F446RE Arduino 排針）：
 *   SWCLK  = PA9  (D8)
 *   SWDIO  = PA8  (D7)
 *   nRESET = PB10 (D6)   目前未用
 * 接到 target：SWCLK->PA14、SWDIO->PA13、GND 共地。這三支腳在 F446 和 L476 上
 * 位置相同，所以 target 換晶片不用改這裡 —— 換的只是要拔哪塊板子的 ST-LINK
 * 隔離跳線（NUCLEO-64 是 CN2；L476G-DISCO 是 MB1184 上的另一組，見 README）。
 *
 * 提供：swd_pins_init / swd_read_idcode / swd_connect / swd_mem_read / swd_mem_write
 *
 * ⚠ **兩套回傳慣例，不要混用**（2026-09-11 在 flash_wr 踩過）：
 *     swd_read_idcode / swd_mem_read / swd_mem_write -> 成功是 1（直接回 SWD 的 ACK 值）
 *     swd_connect                                    -> 成功是 0，負值是錯誤碼
 *   寫成 swd_connect() != 1 的話，連線成功會被判成失敗，而且症狀是
 *   「接線明明沒動卻說連不上」，很容易往硬體方向找。
 *   另外順序固定：先 swd_read_idcode（它做 line reset + JTAG-to-SWD 切換），再 swd_connect。
 *       swd_halt / swd_step / swd_run / swd_dhcsr / swd_core_reg
 */
#pragma once
#include "stm32f446.h"
#include "delay.h"

#define SWD_CLK_PORT  PORTA
#define SWD_CLK_PIN   9u
#define SWD_DIO_PORT  PORTA
#define SWD_DIO_PIN   8u
#define SWD_NRST_PORT PORTB
#define SWD_NRST_PIN  10u

#define SWD_DLY()   do { volatile uint32_t d = 4; while (d--) __asm volatile ("nop"); } while (0)

static uint32_t swd_last_dpidr;

static inline int swd__parity32(uint32_t x)
{
    x ^= x >> 16; x ^= x >> 8; x ^= x >> 4; x ^= x >> 2; x ^= x >> 1;
    return x & 1u;
}

static inline void swd__clk_lo(void) { gpio_clr(SWD_CLK_PORT, SWD_CLK_PIN); }
static inline void swd__clk_hi(void) { gpio_set(SWD_CLK_PORT, SWD_CLK_PIN); }
static inline void swd__dio_hi(void) { gpio_set(SWD_DIO_PORT, SWD_DIO_PIN); }
static inline void swd__dio_lo(void) { gpio_clr(SWD_DIO_PORT, SWD_DIO_PIN); }
static inline int  swd__dio_rd(void) { return gpio_get(SWD_DIO_PORT, SWD_DIO_PIN); }
static inline void swd__drive(void)  { gpio_mode(SWD_DIO_PORT, SWD_DIO_PIN, 1); }   /* output */
static inline void swd__float(void)  { gpio_mode(SWD_DIO_PORT, SWD_DIO_PIN, 0); }   /* input */

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

static inline void swd__pulse(void)
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
    RCC_AHB1ENR |= (1u << PORTA) | (1u << PORTB);

    gpio_mode(SWD_CLK_PORT, SWD_CLK_PIN, 1);
    gpio_mode(SWD_DIO_PORT, SWD_DIO_PIN, 1);
    gpio_pull(SWD_DIO_PORT, SWD_DIO_PIN, 1);           /* float 時拉高 */
    gpio_mode(SWD_NRST_PORT, SWD_NRST_PIN, 1);
    gpio_set(SWD_NRST_PORT, SWD_NRST_PIN);

    swd__clk_lo();
    swd__dio_lo();
}

static inline void swd__line_reset_and_switch(void)
{
    swd__drive();
    swd__wr_seq(0xFFFFFFFFu, 32);
    swd__wr_seq(0xFFFFFFFFu, 24);      /* 56 個高 = line reset */
    swd__wr_seq(0xE79Eu, 16);          /* JTAG-to-SWD，LSB first */
    swd__wr_seq(0xFFFFFFFFu, 32);
    swd__wr_seq(0xFFFFFFFFu, 24);
    swd__dio_lo();
    swd__wr_seq(0, 8);
}

/* 回傳 ACK：1=OK、2=WAIT(重試過)、4=FAULT、7/0=無回應、-1=parity 錯 */
static inline int swd_xfer(uint32_t apndp, uint32_t rnw, uint32_t reg, uint32_t *data)
{
    uint32_t a2 = (reg >> 2) & 1u, a3 = (reg >> 3) & 1u;
    uint32_t par = (apndp ^ rnw ^ a2 ^ a3) & 1u;
    uint32_t req = 1u | (apndp << 1) | (rnw << 2) | (a2 << 3) | (a3 << 4) | (par << 5) | (1u << 7);

    for (int retry = 0; retry < 16; retry++) {
        swd__drive();
        swd__wr_seq(req, 8);
        swd__float();
        swd__pulse();

        int ack = 0;
        for (int i = 0; i < 3; i++)
            ack |= swd__rd() << i;

        if (ack == 1) {
            if (rnw) {
                uint32_t d = 0;
                for (int i = 0; i < 32; i++)
                    d |= (uint32_t)swd__rd() << i;
                int p = swd__rd();
                swd__pulse();
                swd__drive(); swd__dio_lo();
                if (swd__parity32(d) != p)
                    return -1;
                *data = d;
            } else {
                swd__pulse();
                swd__drive();
                uint32_t d = *data;
                for (int i = 0; i < 32; i++)
                    swd__wr((d >> i) & 1u);
                swd__wr(swd__parity32(d));
                swd__dio_lo();
            }
            return 1;
        }

        if (ack == 2) {
            swd__pulse();
            swd__drive(); swd__dio_lo();
            delay_us(50);
            continue;
        }

        swd__pulse();
        swd__drive(); swd__dio_lo();
        return ack;
    }
    return 2;
}

static inline int swd__dp_rd(uint32_t reg, uint32_t *v) { return swd_xfer(0, 1, reg, v); }
static inline int swd__dp_wr(uint32_t reg, uint32_t v)  { return swd_xfer(0, 0, reg, &v); }
static inline int swd__ap_wr(uint32_t reg, uint32_t v)  { return swd_xfer(1, 0, reg, &v); }

static inline int swd__ap_rd(uint32_t reg, uint32_t *v)
{
    int a = swd_xfer(1, 1, reg, v);
    if (a != 1)
        return a;
    return swd__dp_rd(0xC, v);          /* RDBUFF */
}

static inline int swd_read_idcode(uint32_t *id)
{
    swd__line_reset_and_switch();
    int a = swd__dp_rd(0x0, id);
    if (a == 1)
        swd_last_dpidr = *id;
    return a;
}

/* 0 成功；-1 DPIDR、-2 電源握手、-3 其他 */
static inline int swd_connect(void)
{
    uint32_t v;

    swd__line_reset_and_switch();
    if (swd__dp_rd(0x0, &swd_last_dpidr) != 1)
        return -1;

    swd__dp_wr(0x0, 0x1Eu);            /* ABORT：清 sticky error */
    swd__dp_wr(0x4, 0x50000000u);     /* CTRL/STAT：CSYS/CDBG PWRUPREQ */

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

    if (swd__dp_wr(0x8, 0x00000000u) != 1)
        return -3;
    if (swd__ap_rd(0x0, &v) != 1)      /* CSW read-modify-write：只設 Size=word */
        return -3;
    v = (v & ~0x7u) | 0x2u;
    if (swd__ap_wr(0x0, v) != 1)
        return -3;
    return 0;
}

static inline int swd_mem_read(uint32_t addr, uint32_t *out)
{
    int a = swd__ap_wr(0x4, addr);
    if (a == 1)
        a = swd__ap_rd(0xC, out);
    if (a != 1)
        swd__dp_wr(0x0, 0x1Eu);
    return a;
}

static inline int swd_mem_write(uint32_t addr, uint32_t val)
{
    int a = swd__ap_wr(0x4, addr);
    if (a == 1)
        a = swd__ap_wr(0xC, val);
    if (a != 1)
        swd__dp_wr(0x0, 0x1Eu);
    return a;
}

/* ── Cortex-M debug ── */
#define SWD_DHCSR   0xE000EDF0u
#define SWD_DCRSR   0xE000EDF4u
#define SWD_DCRDR   0xE000EDF8u
#define SWD_DBGKEY  0xA05F0000u

/* DHCSR 低半字：bit0 C_DEBUGEN、bit1 C_HALT、bit2 C_STEP、bit3 C_MASKINTS */
static inline int swd_halt(void) { return swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0x3u); }

/* run 也要寫兩次，理由跟 swd_step 是同一條規則的鏡像。
 *
 * 只寫一次 0x1 的話，意圖是「C_HALT=0 去跑，順便把 C_MASKINTS 清成 0」，
 * 但那次寫入清掉了 C_HALT，所以**對 C_MASKINTS 的寫入被忽略** —— 寫 1 被
 * 忽略，寫 0 也一樣被忽略。於是 step 過一次之後 C_MASKINTS 就永遠卡在 1。
 *
 * 2026-09-11 實測：step 修好之後按 r，讀回 DHCSR=0x01010009（bit3=1），
 * 而修好之前是 0x01010001。後果是 **target 在中斷被遮的狀態下跑**，
 * SysTick 不來、週邊中斷不進 ISR，看起來像 target 當掉 —— 比原本的 bug 還難查。
 *
 * 所以第一次寫趁 C_HALT 還是 1 把 C_MASKINTS 清掉，第二次才真的放它跑。 */
static inline int swd_run(void)
{
    int a = swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0x3u);  /* DEBUGEN|HALT，MASKINTS=0 */
    if (a != 1)
        return a;
    return swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0x1u);   /* DEBUGEN，放它跑 */
}

/* step 要寫兩次，不能只寫一次 0xD（= DEBUGEN|STEP|MASKINTS）。
 *
 * ARMv7-M B1.5.15：**對 C_MASKINTS 的寫入會被忽略，除非該次寫入同時維持
 * C_HALT = 1**。而 step 本來就必須把 C_HALT 清掉（要放 CPU 跑一條指令），
 * 所以「C_STEP 和 C_MASKINTS 一起寫」這件事在架構上不可能成立 —— 那次
 * C_MASKINTS=1 會被硬體整個丟掉，而且不報錯。
 *
 * 2026-09-11 實測抓到：只寫 0xD 的版本，step 之後讀回 DHCSR 低半字是 0x7
 * （DEBUGEN|HALT|STEP），bit3 是 0 —— C_MASKINTS 從來沒設進去過。後果是
 * 單步時中斷沒被遮，踩到 SysTick 之類的 ISR 時 PC 會跳進 ISR，看起來像
 * 「step 亂跳」。當時沒發作只是因為 target 那段剛好沒中斷。
 *
 * 所以第一次寫趁 C_HALT 還是 1 把 C_MASKINTS 設好，第二次才下 C_STEP。
 * 改對之後 step 完讀回來應該是 0xF（多了 bit3）。 */
static inline int swd_step(void)
{
    int a = swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0xBu);  /* DEBUGEN|HALT|MASKINTS */
    if (a != 1)
        return a;
    return swd_mem_write(SWD_DHCSR, SWD_DBGKEY | 0xDu);   /* DEBUGEN|STEP|MASKINTS */
}
static inline int swd_dhcsr(uint32_t *v) { return swd_mem_read(SWD_DHCSR, v); }

static inline int swd_core_reg(uint32_t regsel, uint32_t *out)
{
    int a = swd_mem_write(SWD_DCRSR, regsel & 0x1Fu);
    if (a != 1)
        return a;
    uint32_t d = 0;
    for (int i = 0; i < 60; i++) {
        a = swd_mem_read(SWD_DHCSR, &d);
        if (a == 1 && (d & (1u << 16)))
            break;
        delay_us(100);
    }
    return swd_mem_read(SWD_DCRDR, out);
}
