/*
 * nano130/blink —— 第一支從原始碼建置、燒進客製版的韌體
 *
 * 目標板：WENG NANO 客製版。腳位速查：../../iot_lab_research/nano130_firmware_resource_map.md
 *
 * 為什麼閃這幾支腳：
 *   板上唯一「MCU 可控」的燈是 LED2（RGB，共陰極），R/G/B 三色分別接到
 *   PWM0 CH0/CH1/CH2，對應 PA.12 / PA.13 / PA.14（PWM0 CH0-3 的 MFP 就是
 *   PA.12-15，見 BSP PWM_DeadZone 範例）。LED1 那顆綠燈是電源指示，接在
 *   R2 到 VCC，MCU 控不到，別指望它。
 *   這裡把 PA.12~15 全部當一般 GPIO 推挽輸出、一起翻轉 —— 不管 R/G/B 實際
 *   是哪一支、共陰還是共陽，燈一定會閃。先求「看得到在跑」。
 *
 * 時脈：不設定，跑重置後預設的 HIRC 12 MHz。閃燈不需要準確頻率。
 * 復位線：客製版的 J4 只有 3V3/SWDAT/SWCLK/GND，沒有 nRESET —— 所以韌體
 *   開頭沒有任何會鎖死 SWD 的動作，讓 Nu-Link 之後還連得回來。
 */
#include "Nano100Series.h"

/*
 * 已實測（2026-09-10，C1 板）：PA.12~15 一起翻轉，LED2 會閃 —— 全鏈路成立。
 * 現在收斂成單色心跳：只驅 PA.13（PWM0 CH1，schematic 上是 RGB 的 G），
 * 約 1 Hz、亮 100 ms。若那顆不是綠色也無妨，反正是同一顆 LED2。
 */
#define LED_PIN    (1ul << 13)     /* PA.13 */

/* HIRC 12 MHz、-Os 下這個迴圈約 1 ms/1000 次，夠用就好，不追求準 */
static void delay_ms(uint32_t ms)
{
    volatile uint32_t n = ms * 1200u;
    while (n--)
        __NOP();
}

int main(void)
{
    SYS_UnlockReg();
    CLK_EnableModuleClock(GPIO_MODULE);   /* Nano100B 的 GPIO 有 AHB 時脈閘，要開 */
    SYS_LockReg();

    /* PA.13 推挽輸出（重置後 MFP 預設就是 GPIO 功能，不用再設） */
    GPIO_SetMode(PA, LED_PIN, GPIO_PMD_OUTPUT);

    for (;;) {
        PA->DOUT |= LED_PIN;   delay_ms(100);
        PA->DOUT &= ~LED_PIN;  delay_ms(900);
    }
}
