/*
 * nano130/usb_min —— USB 最小列舉測試（CMSIS-DAP 那條路的 Stage 0）
 *
 * **這支不實作任何 USB 類別，也不回應任何請求。** 它只做一件事：把 USB PHY 打開、
 * 把 D+ 的內部上拉拉起來，讓主機「看得見」有東西插進來。
 *
 * 為什麼值得單獨做一支：把 CMSIS-DAP 那整包工作裡**唯一的硬體未知**先切出來。
 * 2026-09-15 看原理圖確認了 `USB_D+`/`USB_D-` 接到專用腳、`USB_VDD33_CAP` 有
 * 1 µF、`USB_VBUS` 有接 —— 但那是判讀。這支韌體是實測。
 *
 * ── 三個層次分開判定（這才是這支程式的設計重點）──────────────────────
 *
 *   1. LCD 的 `VBUS:` —— 讀 `USBD->BUSSTS` 的 FLDET 位元，**晶片自己看不看得到
 *      VBUS**。這一層不牽涉 D+/D-，單獨驗證 `USB_VBUS` 那條線。
 *   2. LCD 的 `PU:on` —— 上拉已經打開（我們寫下去的）。
 *   3. **主機端的反應** —— Windows 跳出任何東西（未知裝置、描述元請求失敗、
 *      驚嘆號）就代表 D+ 上拉被看到了，D+/D- 與 PHY 都活著。
 *
 * **完全列舉成功是不可能的**，因為我們不回應任何控制傳輸 —— 所以「裝置描述元
 * 請求失敗」這種錯誤訊息**就是成功的訊號**，不要當成失敗。這一點先寫下來，
 * 免得看到紅色驚嘆號就誤判。
 *
 * ⚠ USB FS 要 48 MHz。這裡用 PLL 從 HIRC(12 MHz) 倍頻出來，不需要外部石英
 *   （客製版上有沒有 USB 用的石英還沒確認）。HIRC 的精度對 USB 規格來說是
 *   偏鬆的，NANO130 有 HIRC 自動微調（`SYS->IRCTRIMCTL`，靠 USB 的 SOF 校正）
 *   可以補 —— **但那要先能收到 SOF，也就是要先列舉成功**，所以這一階用不上。
 *   如果主機看得到裝置但列舉不穩定，那個微調就是下一步要開的東西。
 */
#include "Nano100Series.h"
#include "delay.h"
#include "lcd1602.h"

int main(void)
{
    SYS_UnlockReg();
    CLK_EnableModuleClock(GPIO_MODULE);

    lcd_init();
    lcd_goto(0, 0);
    lcd_puts("USB MIN  PLL... ");

    /* PLL 48 MHz，來源 HIRC。USBD 的時脈固定吃 PLL，只有除頻可調。 */
    CLK_EnablePLL(CLK_PLLCTL_PLL_SRC_HIRC, 48000000);
    CLK_EnableModuleClock(USBD_MODULE);
    CLK_SetModuleClock(USBD_MODULE, 0, CLK_USB_CLK_DIVIDER(1));

    /* CTL 的低 4 位 = USB_EN | PHY_EN | PWRDN(PHY 通電) | DPPU_EN(D+ 上拉)。
     * BSP 的 USBD_ENABLE_USB() 就是一次寫 0xF。 */
    USBD_ENABLE_USB();

    /* 先拉 SE0 再放掉 —— 這是「重新插拔」給主機的信號，讓它重新列舉。
     *
     * ⚠ 這一步就是 stm32 Stage 4b 卡最久那個 bug 的對應物：那邊是 `DCTL.SDIS`
     *   忘了清，主機永遠看不到裝置，而韌體這端一切正常、不會報任何錯。
     *   兩顆晶片的暫存器名字不同，但「有一個位元把裝置從匯流排上斷開，
     *   而且預設是斷開的」這件事是一樣的。 */
    USBD_SET_SE0();
    delay_ms(50);
    USBD_CLR_SE0();

    SYS_LockReg();

    lcd_goto(0, 0);
    lcd_puts("USB MIN  PU:on  ");

    for (;;) {
        /* FLDET：晶片自己偵測到 VBUS 了嗎。這一層跟 D+/D- 無關，
         * 單獨驗證 `USB_VBUS` 那條線有沒有接對。 */
        int vbus = USBD_IS_ATTACHED() ? 1 : 0;

        lcd_goto(1, 0);
        lcd_puts("VBUS:");
        lcd_puts(vbus ? "YES" : "no ");
        lcd_puts("  CTL:");
        {
            uint32_t c = USBD->CTL & 0xFu;
            lcd_putc("0123456789ABCDEF"[c]);
        }
        lcd_puts("  ");

        delay_ms(200);
    }
}
