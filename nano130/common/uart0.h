/*
/* 檔名刻意不叫 uart.h：include 路徑裡 vendor/StdDriver/inc 排在 common 前面，
 * 叫 uart.h 會被 BSP 那支蓋掉，而且症狀是「函式未宣告」這種看不出原因的錯。 */

/*
 * common/uart0.h —— UART0 的最小收發（N2 的 payload 通道）
 *
 * ⚠ 腳位有爭議，這裡採 BSP 的版本，理由寫在下面：
 *
 *   iot_lab_research/nano130_firmware_resource_map.md（描圖來的）寫
 *     TXD0 = PB.2、RXD0 = PB.1
 *   但 vendor/StdDriver/inc/sys.h（晶片的 multi-function 定義）只有
 *     PB.0 = UART0_RX、PB.1 = UART0_TX、PB.2 = UART0_RTS、PB.3 = UART0_CTS
 *
 * **PB.2 在這顆晶片上根本沒有「UART0 TX」這個選項** —— 所以資源表那兩格
 * 不可能同時成立，最可能是描圖時整體位移了一支腳。這裡先照 BSP 接
 * （RX=PB.0 / TX=PB.1），N2 Stage a 的用途之一就是判定這件事：
 *   - LCD 的 RX 計數會動 → PB.0 確實是接到外面的 TX，描圖錯了
 *   - RX 計數不動         → 再考慮資源表是對的、BSP 的腳位另有玄機
 * 兩種結果都要寫回資源表，不要只改一邊。
 *
 * UART1 不能拿來做這件事：資源表寫明它同時拉到 ESP8266 與 Bluetooth，互斥。
 */
#pragma once
#include "Nano100Series.h"

#define UART_RX_PIN   0u        /* PB.0 */
#define UART_TX_PIN   1u        /* PB.1 */

static inline void uart0_init(uint32_t baud)
{
    SYS_UnlockReg();

    CLK_EnableModuleClock(UART0_MODULE);
    /* UART0/1 共用同一個時脈來源選擇。用 HIRC 12 MHz：不必等 HXT 起振，
     * 也跟這個 repo 其他韌體的時脈假設一致。 */
    CLK_SetModuleClock(UART0_MODULE, CLK_CLKSEL1_UART_S_HIRC, 0);

    SYS->PB_L_MFP = (SYS->PB_L_MFP & ~(SYS_PB_L_MFP_PB0_MFP_Msk | SYS_PB_L_MFP_PB1_MFP_Msk))
                  | SYS_PB_L_MFP_PB0_MFP_UART0_RX
                  | SYS_PB_L_MFP_PB1_MFP_UART0_TX;

    SYS_LockReg();

    UART_Open(UART0, baud);     /* 8N1 */
}

static inline int uart0_rx_ready(void)
{
    return (UART0->FSR & UART_FSR_RX_EMPTY_F_Msk) == 0;
}

static inline uint8_t uart0_getc(void)
{
    while (!uart0_rx_ready())
        ;
    return (uint8_t)UART0->RBR;
}

static inline void uart0_putc(uint8_t c)
{
    while (UART0->FSR & UART_FSR_TX_FULL_F_Msk)
        ;
    UART0->THR = c;
}

static inline void uart0_puts(const char *s)
{
    while (*s)
        uart0_putc((uint8_t)*s++);
}
