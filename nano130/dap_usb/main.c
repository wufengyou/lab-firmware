/*
 * nano130/dap_usb —— 把客製版做成 CMSIS-DAP probe
 *
 * = `usb_enum` 的傳輸層 + `common/dap.h` 的指令層。**兩邊都沒有為對方改過**：
 * usb_enum 當初就是照 CMSIS-DAP 的形狀做的（HID + 一對 64 byte 中斷端點），
 * dap.h 是 2026-09-15 從 stm32/ 搬過來的（只改了 nRESET 的 GPIO 寫法和產品名）。
 *
 * 這支程式自己做的事只有一件：**把 EP3 收到的 64 byte 丟給 dap_process()，
 * 把回應從 EP2 送回去。** 其餘都是前面兩階已經驗過的東西。
 *
 * 驗收點跟 stm32 Stage 4b 一樣：`pyocd list` 看不看得到這塊板子。
 *
 * master = 客製版，J17 pin 3/4/8 → target 的 J4 pin 3/2/4（見 common/swd.h）。
 * ⚠ 這支**不是**燒錄器（那是 flash_isp）。它是除錯探針，讓 PC 上的標準工具
 *   （pyOCD / OpenOCD / Keil）直接操作 target。
 *
 * ── 這支程式自己刻的只有兩件事 ────────────────────────────────────────
 *   1. 描述元表（裝置 / 組態 / 字串 / HID report）
 *   2. 事件分派：輪詢 `USBD->INTSTS`，把事件轉給 BSP 的 `USBD_*` 函式
 * 標準請求、GET_DESCRIPTOR、SET_ADDRESS、控制傳輸的分段，全部由
 * `vendor/StdDriver/src/usbd.c` 處理 —— 那正是 stm32 那輪手刻最久的部分。
 *
 * ⚠ **不開 NVIC，用輪詢**，跟這個 repo 其他韌體一致。
 *
 * ⚠ **SysTick 有主權衝突**：BSP 的 `USBD_Start()` 內部呼叫 `CLK_SysTickDelay()`，
 *   而 `common/keypad.h` 的 `kp_poll()` 把 SysTick 當自由計時器用。這支程式沒用
 *   keypad 所以不衝突，**但之後要把 USB 併進 flash_isp 時會撞**。先寫在這裡。
 */
#include "Nano100Series.h"
#include "delay.h"
#include "lcd1602.h"
#include "swd.h"
#include "dap.h"

/* ── USB SRAM 配置 ──────────────────────────────────────────────────────
 * 前 8 byte 固定是 setup packet（BSP 的 USBD_ProcessSetupPacket 寫死從
 * USBD_BUF_BASE 讀 8 byte），其餘自己分。 */
#define SETUP_BUF_LEN   8
#define EP0_BUF_BASE    (SETUP_BUF_LEN)          /* 控制 IN  */
#define EP0_BUF_LEN     64
#define EP1_BUF_BASE    (EP0_BUF_BASE + EP0_BUF_LEN)  /* 控制 OUT */
#define EP1_BUF_LEN     64
#define EP2_BUF_BASE    (EP1_BUF_BASE + EP1_BUF_LEN)  /* 中斷 IN  */
#define EP2_BUF_LEN     64
#define EP3_BUF_BASE    (EP2_BUF_BASE + EP2_BUF_LEN)  /* 中斷 OUT */
#define EP3_BUF_LEN     64

#define HID_EP_IN       1u                        /* 0x81 */
#define HID_EP_OUT      1u                        /* 0x01 */

/* VID/PID 用 pid.codes 的測試配額 1209:0001。
 * **不要借用 Nuvoton 或任何廠商的 VID** —— 原廠範例常這麼寫，但那是別人的
 * 識別碼；這塊板子是我們自己的東西，插到別人電腦上不該冒充成別人的裝置。 */
#define USB_VID         0x1209u
#define USB_PID         0x0001u

static uint8_t dev_desc[] = {
    18, 0x01,                   /* bLength, DEVICE */
    0x00, 0x02,                 /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,           /* class/subclass/protocol：交給介面層決定 */
    64,                         /* EP0 最大封包 */
    USB_VID & 0xFF, USB_VID >> 8,
    USB_PID & 0xFF, USB_PID >> 8,
    0x00, 0x01,                 /* bcdDevice 1.00 */
    1, 2, 3,                    /* iManufacturer, iProduct, iSerial */
    1                           /* bNumConfigurations */
};

/* 組態描述元：組態(9) + 介面(9) + HID(9) + 端點(7)×2 = 41 byte */
#define CONFIG_TOTAL    41
#define HID_DESC_IDX    18      /* HID 描述元在上面這串裡的位移，BSP 要知道 */

static uint8_t cfg_desc[] = {
    9, 0x02, CONFIG_TOTAL, 0x00, 1, 1, 0,
    0x80,                       /* bmAttributes：匯流排供電、不支援遠端喚醒 */
    50,                         /* bMaxPower = 100 mA */

    9, 0x04, 0, 0, 2,           /* 介面 0，兩支端點 */
    0x03, 0x00, 0x00,           /* HID 類別，不是 boot 裝置 */
    0,

    9, 0x21,                    /* HID 描述元 */
    0x10, 0x01,                 /* bcdHID 1.10 */
    0x00, 1,
    0x22, 0, 0,                 /* report 描述元長度，開機時填 */

    7, 0x05, 0x80 | HID_EP_IN,  0x03, 64, 0, 1,   /* 中斷 IN，1 ms */
    7, 0x05, HID_EP_OUT,        0x03, 64, 0, 1,   /* 中斷 OUT */
};

/*
 * HID report 描述元：廠商自訂用途、64 byte 進 + 64 byte 出。
 * **這正是 CMSIS-DAP 用的那一份**（DAP 是把指令塞進 HID report 傳的），
 * 所以下一階不用動它。
 */
static uint8_t hid_report[] = {
    0x06, 0x00, 0xFF,           /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,                 /* Usage 1 */
    0xA1, 0x01,                 /* Collection (Application) */
    0x15, 0x00,                 /*   Logical Minimum 0 */
    0x26, 0xFF, 0x00,           /*   Logical Maximum 255 */
    0x75, 0x08,                 /*   Report Size 8 */
    0x95, 0x40,                 /*   Report Count 64 */
    0x09, 0x01, 0x81, 0x02,     /*   Input  (Data,Var,Abs) */
    0x09, 0x01, 0x91, 0x02,     /*   Output (Data,Var,Abs) */
    0xC0                        /* End Collection */
};
static uint32_t hid_report_size = sizeof(hid_report);
static uint32_t cfg_hid_idx = HID_DESC_IDX;

/* 字串描述元：UTF-16LE，第一個 byte 是長度、第二個是型別(0x03) */
static uint8_t str_lang[]  = { 4, 0x03, 0x09, 0x04 };            /* en-US */
static uint8_t str_vendor[] = {
    26, 0x03, 'l',0, 'a',0, 'b',0, '-',0, 'f',0, 'i',0, 'r',0, 'm',0,
              'w',0, 'a',0, 'r',0, 'e',0
};
/* 產品字串**必須帶 "CMSIS-DAP"** —— pyOCD 就是靠它在一堆 HID 裝置裡認出
 * 哪個是 debug probe（USB VID/PID 我們用的是測試配額，不在它的白名單裡）。
 * 長度 = 2 + 17×2 = 36。 */
static uint8_t str_product[] = {
    36, 0x03, 'N',0, 'A',0, 'N',0, 'O',0, '1',0, '3',0, '0',0, ' ',0,
              'C',0, 'M',0, 'S',0, 'I',0, 'S',0, '-',0, 'D',0, 'A',0, 'P',0
};
static uint8_t str_serial[] = {
    10, 0x03, '0',0, '0',0, '0',0, '1',0
};
static uint8_t *str_desc[] = { str_lang, str_vendor, str_product, str_serial };
static uint8_t *hid_report_tbl[] = { hid_report, 0, 0, 0 };

static S_USBD_INFO_T usb_info = {
    dev_desc, cfg_desc, str_desc, hid_report_tbl, &hid_report_size, &cfg_hid_idx
};

/* HID 的類別請求（GET/SET_REPORT…）。這一階不需要真的支援，
 * 但**不能不回應** —— 主機拿不到回應會判定裝置有問題。 */
static void hid_class_request(void)
{
    uint8_t buf[8];
    USBD_GetSetupPacket(buf);

    if (buf[0] & 0x80) {                /* 主機要讀 */
        USBD_SET_DATA1(EP0);
        USBD_SET_PAYLOAD_LEN(EP0, 0);   /* 回一個零長度封包，禮貌性地說「沒有」*/
        USBD_PrepareCtrlIn(0, 0);
    } else {                            /* 主機要寫 */
        USBD_SET_DATA1(EP1);
        USBD_SET_PAYLOAD_LEN(EP1, 0);
    }
}

static void usb_init(void)
{
    USBD_Open(&usb_info, hid_class_request, NULL);

    /* 控制端點：EP0 = IN、EP1 = OUT，端點編號都是 0 */
    USBD_CONFIG_EP(EP0, USBD_CFG_CSTALL | USBD_CFG_EPMODE_IN  | 0);
    USBD_SET_EP_BUF_ADDR(EP0, EP0_BUF_BASE);
    USBD_CONFIG_EP(EP1, USBD_CFG_CSTALL | USBD_CFG_EPMODE_OUT | 0);
    USBD_SET_EP_BUF_ADDR(EP1, EP1_BUF_BASE);

    /* HID 的一對中斷端點 */
    USBD_CONFIG_EP(EP2, USBD_CFG_EPMODE_IN  | HID_EP_IN);
    USBD_SET_EP_BUF_ADDR(EP2, EP2_BUF_BASE);
    USBD_CONFIG_EP(EP3, USBD_CFG_EPMODE_OUT | HID_EP_OUT);
    USBD_SET_EP_BUF_ADDR(EP3, EP3_BUF_BASE);
    USBD_SET_PAYLOAD_LEN(EP3, EP3_BUF_LEN);   /* 先掛一個收的緩衝 */

    USBD_Start();
}

/*
 * 收到一包 DAP 命令 → 交給指令層 → 回應從 EP2 送回去 → 重新掛好收的緩衝。
 *
 * ⚠ **順序是有意義的**：一定要先把回應排好、再把 EP3 的緩衝重新掛回去。
 *   反過來的話，主機可能在我們還沒把回應放上 EP2 之前就送下一包命令進來，
 *   而 DAP 是嚴格的一問一答 —— 那會錯開一整包，症狀是「回應對不上請求」
 *   這種最難查的錯。跟 flash_isp 那條「ACK 最後才送」是同一個道理。
 *
 * USB SRAM 不能直接當一般記憶體讀寫，要走 USBD_MemCopy（BSP 的 inline 函式，
 * 處理了 byte 存取模式）。
 */
static uint8_t dap_req[DAP_PACKET_SIZE];
static uint8_t dap_resp[DAP_PACKET_SIZE];
static uint32_t dap_packets;

static void dap_handle_packet(void)
{
    uint32_t len = USBD_GET_PAYLOAD_LEN(EP3);
    if (len > DAP_PACKET_SIZE)
        len = DAP_PACKET_SIZE;

    USBD_MemCopy(dap_req, (uint8_t *)(USBD_BUF_BASE + EP3_BUF_BASE), len);

    for (uint32_t i = 0; i < DAP_PACKET_SIZE; i++)
        dap_resp[i] = 0;

    uint32_t n = dap_process(dap_req, dap_resp);
    if (n > DAP_PACKET_SIZE)
        n = DAP_PACKET_SIZE;

    /* CMSIS-DAP v1 的回應一律是整包 64 byte，不足補 0。 */
    USBD_MemCopy((uint8_t *)(USBD_BUF_BASE + EP2_BUF_BASE), dap_resp, DAP_PACKET_SIZE);
    USBD_SET_PAYLOAD_LEN(EP2, DAP_PACKET_SIZE);

    (void)n;
    dap_packets++;

    USBD_SET_PAYLOAD_LEN(EP3, EP3_BUF_LEN);   /* 準備收下一包 */
}

/*
 * 事件分派 —— 原廠範例是寫在 USBD_IRQHandler 裡的，這裡改成輪詢。
 * 內容一樣：把 INTSTS 的旗標轉成對 BSP 函式的呼叫。
 */
static void usb_poll(void)
{
    uint32_t sts = USBD_GET_INT_FLAG();
    uint32_t bus = USBD_GET_BUS_STATE();

    if (sts & USBD_INTSTS_FLDET) {
        USBD_CLR_INT_FLAG(USBD_INTSTS_FLDET);
        if (USBD_IS_ATTACHED())
            USBD_ENABLE_USB();
        else
            USBD_DISABLE_USB();
    }

    if (sts & USBD_INTSTS_BUS) {
        USBD_CLR_INT_FLAG(USBD_INTSTS_BUS);
        if (bus & USBD_STATE_USBRST) {
            USBD_ENABLE_USB();
            USBD_SwReset();             /* 匯流排重置：位址歸零、端點狀態清掉 */
        }
        if (bus & USBD_STATE_SUSPEND)
            USBD_DISABLE_PHY();
        if (bus & USBD_STATE_RESUME)
            USBD_ENABLE_USB();
    }

    if (sts & USBD_INTSTS_USB) {
        if (sts & USBD_INTSTS_SETUP) {
            USBD_CLR_INT_FLAG(USBD_INTSTS_SETUP);
            USBD_CLR_EP_STALL(EP0);
            USBD_CLR_EP_STALL(EP1);
            USBD_ProcessSetupPacket();
        }
        if (sts & USBD_INTSTS_EP0) {
            USBD_CLR_INT_FLAG(USBD_INTSTS_EP0);
            USBD_CtrlIn();
        }
        if (sts & USBD_INTSTS_EP1) {
            USBD_CLR_INT_FLAG(USBD_INTSTS_EP1);
            USBD_CtrlOut();
        }
        if (sts & USBD_INTSTS_EP2)
            USBD_CLR_INT_FLAG(USBD_INTSTS_EP2);     /* IN 送完了，沒事要做 */

        if (sts & USBD_INTSTS_EP3) {
            USBD_CLR_INT_FLAG(USBD_INTSTS_EP3);
            dap_handle_packet();
        }
    }
}

int main(void)
{
    SYS_UnlockReg();
    CLK_EnableModuleClock(GPIO_MODULE);

    lcd_init();
    swd_pins_init();

    lcd_goto(0, 0);
    lcd_puts("DAP USB  PLL... ");

    CLK_EnablePLL(CLK_PLLCTL_PLL_SRC_HIRC, 48000000);
    CLK_EnableModuleClock(USBD_MODULE);
    CLK_SetModuleClock(USBD_MODULE, 0, CLK_USB_CLK_DIVIDER(1));

    /* report 描述元長度要填回組態描述元裡的 HID 描述元 */
    cfg_desc[HID_DESC_IDX + 7] = (uint8_t)(sizeof(hid_report) & 0xFF);
    cfg_desc[HID_DESC_IDX + 8] = (uint8_t)(sizeof(hid_report) >> 8);

    usb_init();
    SYS_LockReg();

    lcd_goto(0, 0);
    lcd_puts("NANO130 CMSISDAP");

    uint32_t tick = 0;

    for (;;) {
        usb_poll();

        /* LCD 更新放慢，免得佔住 CPU 讓 USB 來不及回應 —— 控制傳輸有逾時。 */
        if (++tick >= 20000u) {
            tick = 0;
            /* ⚠ 一行只有 16 格，標籤一律縮到最短。
             * 2026-09-15 同一個錯誤犯了三次（flash_isp 的 "prep err:"、
             * uart_echo、這裡），每次症狀都是「值的低位悄悄消失」——
             * **不會報錯、看起來只是數字怪怪的**。
             * 規則：先數格子再寫，標籤 + 值不得超過 16。 */
            lcd_goto(1, 0);
            if (g_usbd_UsbConfig == 0) {
                /* 還沒列舉完:看 VBUS 與 CFG。"V:Y CFG:0 P:0000" = 16 格。 */
                lcd_puts("V:");
                lcd_puts(USBD_IS_ATTACHED() ? "Y" : "n");
                lcd_puts(" CFG:0 P:");
                for (int i = 12; i >= 0; i -= 4)
                    lcd_putc("0123456789ABCDEF"[(dap_packets >> i) & 0xF]);
            } else {
                /* 列舉完成之後 V 與 CFG 都沒有新資訊了(P 會動就代表兩者都成立),
                 * 把格子讓給錯誤計數。
                 * "P:0060 E:4 N:0002" 太長,壓成 "P:0060 E:4 N:02" = 15 格。
                 *
                 * E = 最後一個非 OK 的傳輸結果(4=FAULT,8=ERROR/無回應)
                 * N = 累計錯誤次數(只取低 8 bit)
                 *
                 * **這兩個欄位是為了分辨「掛住」和「一直在報錯但還活著」。**
                 * 2026-09-17 之前這兩種情況在 LCD 上長得一模一樣。
                 * 只要 P 還在動,probe 就是活的。 */
                lcd_puts("P:");
                for (int i = 12; i >= 0; i -= 4)
                    lcd_putc("0123456789ABCDEF"[(dap_packets >> i) & 0xF]);
                lcd_puts(" E:");
                lcd_putc("0123456789ABCDEF"[dap_last_err & 0xF]);
                lcd_puts(" N:");
                lcd_putc("0123456789ABCDEF"[(dap_err_count >> 4) & 0xF]);
                lcd_putc("0123456789ABCDEF"[dap_err_count & 0xF]);
                lcd_puts(" ");
            }
        }
    }
}
