/*
 * common/usb_desc.h —— Stage 4b 的 USB 描述元
 *
 * HID、vendor-defined usage page（CMSIS-DAP 標準做法：64 byte 進、64 byte 出，
 * 內容不假裝成滑鼠鍵盤，主機端靠 report descriptor 認得這是 raw HID）。
 * idVendor/idProduct 用 pid.codes 的測試區段（0x1209），本地開發用，
 * 沒有要上架就不用申請正式 VID/PID。
 */
#pragma once
#include <stdint.h>

static const uint8_t usb_dev_desc[18] = {
    18, 0x01,               /* bLength, DEVICE */
    0x00, 0x02,              /* bcdUSB 2.00 */
    0x00, 0x00, 0x00,        /* class/subclass/protocol：由 interface 決定 */
    64,                       /* bMaxPacketSize0 */
    0x09, 0x12,               /* idVendor  0x1209 (pid.codes test) */
    0x01, 0xDA,               /* idProduct 0xDA01 */
    0x00, 0x01,               /* bcdDevice 1.00 */
    1, 2, 0,                  /* iManufacturer, iProduct, iSerialNumber(無) */
    1,                        /* bNumConfigurations */
};

/* CMSIS-DAP 慣用的 vendor-defined report：64 byte in、64 byte out，opaque。
 *
 * ⚠ 陣列長度一定要寫 `[]` 讓編譯器自己數，**不要手動填數字**。
 * 2026-09-14 踩過：這裡原本寫成 `[33]` 但內容只有 27 byte，C 會安靜地
 * 在後面補 6 個 0x00，而 0x00 在 report descriptor 裡是無效項目。於是
 * sizeof 回報 33、HID 描述元也宣告 33、我們真的送了 33 byte 出去，
 * Windows 的 HID 解析器判定描述元非法 → 裝置管理員代碼 10，
 * ProblemStatus = 0xC000001D（= invalid HID report descriptor）。
 * 症狀極具誤導性：列舉全程看起來完全正常（SET_CONFIGURATION 都過了），
 * 只是最後主機收完描述元就默默把匯流排掛起，log 沒有任何錯誤。
 * 「初始值比陣列短」是合法 C，編譯器連警告都不會給。 */
static const uint8_t usb_hid_report_desc[] = {
    0x06, 0x00, 0xFF,   /* Usage Page (Vendor Defined 0xFF00) */
    0x09, 0x01,         /* Usage (Vendor Usage 1) */
    0xA1, 0x01,         /* Collection (Application) */
    0x15, 0x00,         /*   Logical Minimum 0 */
    0x26, 0xFF, 0x00,   /*   Logical Maximum 255 */
    0x75, 0x08,         /*   Report Size 8 */
    0x95, 0x40,         /*   Report Count 64 */
    0x09, 0x01,         /*   Usage (Vendor Usage 1) */
    0x81, 0x02,         /*   Input (Data,Var,Abs) */
    0x95, 0x40,         /*   Report Count 64 */
    0x09, 0x01,         /*   Usage (Vendor Usage 1) */
    0x91, 0x02,         /*   Output (Data,Var,Abs) */
    0xC0,               /* End Collection */
};

/* config(9) + interface(9) + HID(9) + ep IN(7) + ep OUT(7) = 41 byte */
static const uint8_t usb_cfg_desc[41] = {
    9, 0x02, 41, 0x00, 1, 1, 0, 0x80, 50,      /* CONFIGURATION：bus-powered, 100mA */

    9, 0x04, 0, 0, 2, 0x03, 0x00, 0x00, 0,      /* INTERFACE：class=HID */

    9, 0x21, 0x11, 0x01, 0x00, 1, 0x22,          /* HID：bcdHID=1.11, 1 個 report desc */
    sizeof(usb_hid_report_desc) & 0xFF, sizeof(usb_hid_report_desc) >> 8,

    7, 0x05, 0x81, 0x03, 64, 0x00, 1,            /* ENDPOINT：EP1 IN, interrupt, 64B, 1ms */
    7, 0x05, 0x01, 0x03, 64, 0x00, 1,            /* ENDPOINT：EP1 OUT, interrupt, 64B, 1ms */
};

static const uint8_t usb_str_lang[4]  = { 4, 0x03, 0x09, 0x04 };   /* langid 0x0409 */

/* UTF-16LE，bLength 包含表頭 2 byte */
static const uint8_t usb_str_mfg[20] = {
    20, 0x03,
    'b',0,'o',0,'a',0,'r',0,'d',0,'-',0,'l',0,'a',0,'b',0,
};
static const uint8_t usb_str_prod[34] = {
    34, 0x03,
    'F',0,'4',0,'4',0,'6',0,'R',0,'E',0,' ',0,
    'C',0,'M',0,'S',0,'I',0,'S',0,'-',0,'D',0,'A',0,'P',0,
};

/* 描述元最惡劣的錯誤是「宣告的長度」跟「實際位元組數」對不起來——裝置照樣
 * 列舉、主機照樣收下，然後在很後面以看不懂的方式失敗（見上面 report descriptor
 * 那段的慘案）。這幾條把一致性交給編譯器，不要靠人眼數。
 * 只能檢查 sizeof：讀陣列元素在 C 裡不算常數運算式，所以描述元裡寫死的長度欄位
 * 還是得跟這裡的數字一起維護——但至少「少寫幾個 byte 被靜靜補 0」不會再發生。 */
_Static_assert(sizeof(usb_dev_desc) == 18, "device descriptor 長度不對，bLength 那格也要跟著改");
_Static_assert(sizeof(usb_cfg_desc) == 41, "config descriptor 長度不對，wTotalLength 那格也要跟著改");
_Static_assert(sizeof(usb_str_lang) == 4,  "langid 長度不對");
_Static_assert(sizeof(usb_str_mfg)  == 20, "廠商字串長度不對，第一個 byte 也要跟著改");
_Static_assert(sizeof(usb_str_prod) == 34, "產品字串長度不對，第一個 byte 也要跟著改");
