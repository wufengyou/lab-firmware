/*
 * stm32/dap_usb —— Stage 4b：CMSIS-DAP 指令層掛到 OTG_FS 的 HID 端點
 *
 * common/dap.h 的 dap_process() 跟 Stage 4a 完全共用、一個字沒改。換掉的只有
 * 傳輸層：Stage 4a 是 UART 加自己框的封包，這一支是真正的 USB HID interrupt
 * transfer。接線不變（SWD 三條線）；新增一條剪開的 USB 線到 PA11/PA12（見
 * README「Stage 4b 需要的硬體」）。
 *
 * USART2（PA2/PA3）沒有被 Stage 4b 用掉，所以拿來印列舉除錯訊息——
 * 這條線這時候不是「傳輸層」了，純粹是給人看的 log，跟 Stage 4a 反過來。
 * 開 `.\serial.ps1` 看這些訊息、Windows 裝置管理員看 USB 列舉結果，兩邊對照查。
 *
 * ── 目前狀態：編得過、沒上機測過 ──
 * OTG_FS 的暫存器操作是照 RM0390 手冊查出來的，這份跟前面三個 Stage 不一樣的
 * 地方：**沒有已知 good 的版本可以切一刀比對**（USB 是全新的子系統）。
 * 上機第一件事：Windows 裝置管理員底下出不出現一個 HID 裝置。出現了再驗
 * dap_process 有沒有正確跑；沒出現就照 README 的除錯順序查。
 */
#include "stm32f446.h"
#include "delay.h"
#include "uart.h"
#include "swd.h"
#include "dap.h"
#include "usb_otg.h"
#include "usb_desc.h"

static uint8_t s_ep1_out[DAP_PACKET_SIZE];
static uint8_t s_ep1_in[DAP_PACKET_SIZE];
static volatile uint32_t s_ep1_out_len = 0;
static volatile uint32_t s_ep1_out_ready = 0;

/* 除錯訊息要短。uart_puts 是阻塞的，115200 baud 一個字元 ~87us，而列舉的時序
 * 很緊（SET_ADDRESS 之後主機最快 2ms 就問下一題）。原本用 uart_puthex 一個值
 * 就吃掉 10 個字元，一行 60 字元 = 5ms 空白期，等於在時序關鍵路徑上睡覺。 */
static void dbg_hex2(uint32_t v)
{
    const char *h = "0123456789ABCDEF";
    uart_putc(h[(v >> 4) & 0xF]);
    uart_putc(h[v & 0xF]);
}

/* 控制傳輸的資料階段：只送得出「一個封包裝得下」的描述元（見 usb_desc.h 註解），
 * 所以不必處理跨封包續傳。長度依 wLength 截短。 */
static void ep0_send(const uint8_t *data, uint32_t len, uint32_t wlength)
{
    if (len > wlength)
        len = wlength;
    if (len > EP0_MPS)
        len = EP0_MPS;      /* 這份骨架的描述元全部 <=41 byte，實際不會走到這裡 */
    otg_write_in(0, data, len);     /* 先把硬體餵飽，再印 log */
    uart_puts(" ->"); dbg_hex2(len); uart_puts("\r\n");
}

static void ep0_status_in(void)
{
    otg_write_in(0, 0, 0);
}

static void handle_setup(const uint8_t *s)
{
    uint8_t  bmRequestType = s[0];
    uint8_t  bRequest      = s[1];
    uint16_t wValue        = (uint16_t)(s[2] | (s[3] << 8));
    uint16_t wLength       = (uint16_t)(s[6] | (s[7] << 8));

    /* Stage 4b 除錯用（列舉穩定之後可以拿掉）。格式：S <bmRT> <bReq> <wVal> <wLen> */
    uart_puts("S ");
    dbg_hex2(bmRequestType); uart_putc(' ');
    dbg_hex2(bRequest);      uart_putc(' ');
    dbg_hex2(wValue >> 8); dbg_hex2(wValue & 0xFF); uart_putc(' ');
    dbg_hex2(wLength);

    if (bmRequestType == 0x80 && bRequest == 0x06) {          /* GET_DESCRIPTOR，標準、device->host */
        switch (wValue >> 8) {
        case 0x01: ep0_send(usb_dev_desc, sizeof usb_dev_desc, wLength); return;
        case 0x02: ep0_send(usb_cfg_desc, sizeof usb_cfg_desc, wLength); return;
        case 0x03:
            switch (wValue & 0xFF) {
            case 0: ep0_send(usb_str_lang, sizeof usb_str_lang, wLength); return;
            case 1: ep0_send(usb_str_mfg,  sizeof usb_str_mfg,  wLength); return;
            case 2: ep0_send(usb_str_prod, sizeof usb_str_prod, wLength); return;
            }
            break;
        }
        otg_ep0_stall();
        return;
    }

    if (bmRequestType == 0x81 && bRequest == 0x06 && (wValue >> 8) == 0x22) {  /* HID GET_DESCRIPTOR(REPORT) */
        ep0_send(usb_hid_report_desc, sizeof usb_hid_report_desc, wLength);
        return;
    }

    if (bmRequestType == 0x00 && bRequest == 0x05) {          /* SET_ADDRESS */
        /* ⚠ 位址要**當下立刻**寫進 DCFG，不要等狀態階段送完才寫。
         * USB 規格說位址在狀態階段之後才生效，但 dwc2 核心自己會處理這件事
         * （ST HAL 與 TinyUSB 都是收到 SETUP 就寫）。原本延後到 XFRC 才寫是
         * 競態：主機可能已經改用新位址發問，而我們還在用舊位址聽。 */
        OTG_DCFG = (OTG_DCFG & ~(0x7Fu << 4)) | ((uint32_t)(wValue & 0x7Fu) << 4);
        ep0_status_in();
        uart_puts(" ->addr\r\n");
        return;
    }

    if (bmRequestType == 0x00 && bRequest == 0x09) {          /* SET_CONFIGURATION */
        ep0_status_in();
        otg_ep1_enable();
        uart_puts(" ->CONFIG, EP1 on\r\n");
        return;
    }

    if (bmRequestType == 0xA1 && bRequest == 0x01) {          /* HID GET_REPORT */
        /* 我們沒有「目前的報告內容」這種狀態（DAP 是一問一答，不是感測器），
         * 但 Windows 的 HID 驅動啟動時可能會問一次。回一包 0 比回 STALL 安全：
         * STALL 在某些 Windows 版本會讓驅動啟動失敗（代碼 10）。 */
        static const uint8_t zeros[DAP_PACKET_SIZE] = { 0 };
        ep0_send(zeros, sizeof zeros, wLength);
        return;
    }

    if (bmRequestType == 0x21 && (bRequest == 0x0A || bRequest == 0x0B)) {  /* HID SET_IDLE / SET_PROTOCOL */
        ep0_status_in();                                       /* 沒有東西好設定，回 ACK 就好 */
        uart_puts(" ->ack\r\n");
        return;
    }

    /* 沒實作的一律 stall，明確拒絕，不要假裝成功（跟 dap.h 的原則一樣）。 */
    otg_ep0_stall();
    uart_puts(" ->STALL\r\n");
}

static void poll_usb(void)
{
    uint32_t gintsts = OTG_GINTSTS;

    if (gintsts & OTG_INT_USBRST) {
        OTG_GINTSTS = OTG_INT_USBRST;
        otg_on_reset();        /* 裡面會把 DCFG.DAD 清回 0 */
        uart_puts("[usb] USBRST\r\n");
    }

    if (gintsts & OTG_INT_ENUMDNE) {
        OTG_GINTSTS = OTG_INT_ENUMDNE;
        otg_on_enum_done();
        uart_puts("[usb] ENUMDNE\r\n");
    }

    if (gintsts & OTG_INT_RXFLVL) {
        uint32_t sts = OTG_GRXSTSP;             /* pop */
        uint32_t epnum  = sts & 0xFu;
        uint32_t bcnt   = (sts >> 4) & 0x7FFu;
        uint32_t pktsts = (sts >> 17) & 0xFu;

        /* ⚠ 2026-09-14 上機抓到的 bug：**收到的每一包都必須從 FIFO 讀乾淨**，
         * 不管我們認不認得它。原本只在「EP0 的 SETUP」與「EP1 的 OUT」兩種
         * 情況下讀資料，其他情況只 pop 掉狀態項目就算了——但狀態項目跟資料
         * 是分開的兩件事，資料沒讀走就會永遠卡在 RX FIFO 裡，於是
         * GINTSTS.RXFLVL 一直是 1、**後面所有封包（包括 SETUP）全被堵在它
         * 後面**，症狀是列舉走到一半整個靜止，看起來像主機不講話了。
         *
         * 所以下面一律 otg_read_fifo()，只是「要不要留下來」才分情況。 */
        if (pktsts == RXSTS_SETUP_RECV && epnum == 0 && bcnt == sizeof s_setup_pkt) {
            otg_read_fifo(s_setup_pkt, bcnt);
        } else if (pktsts == RXSTS_OUT_RECV && epnum == 1 && bcnt <= DAP_PACKET_SIZE) {
            otg_read_fifo(s_ep1_out, bcnt);
            s_ep1_out_len = bcnt;
            s_ep1_out_ready = 1;
        } else {
            /* 認不得的資料（例如 EP0 控制傳輸的 OUT 資料階段）：讀掉丟棄，
             * 目的只是把 FIFO 清空。不能因為「用不到」就不讀。 */
            otg_drain_fifo(bcnt);
        }
        /* SETUP_COMP / bcnt=0 的狀態項目：pop 掉就好，本來就沒有資料要讀。 */
    }

    if (gintsts & OTG_INT_OEPINT) {
        uint32_t daint = OTG_DAINT;

        if (daint & (1u << 16)) {               /* OUT EP0 */
            uint32_t oi = OTG_DOEPINT(0);
            if (oi & (1u << 3)) {                /* STUP：SETUP 資料已經在 s_setup_pkt 裡 */
                handle_setup(s_setup_pkt);
            }
            OTG_DOEPINT(0) = oi;
            /* 不管是 STUP 還是狀態階段的 OUT ZLP，都重新掛下一個 SETUP。 */
            OTG_DOEPTSIZ(0) = (3u << 29) | (1u << 19) | EP0_MPS;
            OTG_DOEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
        }
        if (daint & (1u << 17)) {                /* OUT EP1 */
            uint32_t oi = OTG_DOEPINT(1);
            OTG_DOEPINT(1) = oi;
        }
    }

    if (gintsts & OTG_INT_IEPINT) {
        uint32_t daint = OTG_DAINT;
        if (daint & (1u << 0)) {
            OTG_DIEPINT(0) = OTG_DIEPINT(0);   /* 清掉就好；位址已在 SETUP 當下寫入 */
        }
        if (daint & (1u << 1)) {
            uint32_t ii = OTG_DIEPINT(1);
            OTG_DIEPINT(1) = ii;
        }
    }
}

int main(void)
{
    dwt_init();
    uart_init();
    swd_pins_init();

    uart_puts("\r\n=== STM32 dap_usb (Stage 4b) ===\r\n");
    otg_device_init();
    uart_puts("[usb] core init done, waiting for host...\r\n");

    /* 心跳：列舉停在某一步時，用來分辨「主機還在講話但我們收不到」還是
     * 「主機已經放棄不講了」。DSTS 的 bit0 SUSPSTS=1 代表匯流排被主機掛起
     * （= 主機放棄了），FNSOF 會一直跳代表主機還在送 SOF（= 匯流排還活著）。 */
    uint32_t hb = CM_DWT_CYCCNT;

    for (;;) {
        poll_usb();

        if ((uint32_t)(CM_DWT_CYCCNT - hb) > 16000000u) {   /* 16MHz -> 1 秒 */
            hb = CM_DWT_CYCCNT;
            uart_puts("HB gints="); dbg_hex2(OTG_GINTSTS >> 24); dbg_hex2(OTG_GINTSTS >> 16);
            dbg_hex2(OTG_GINTSTS >> 8); dbg_hex2(OTG_GINTSTS);
            uart_puts(" dsts=");     dbg_hex2(OTG_DSTS >> 8); dbg_hex2(OTG_DSTS);
            uart_puts(" doep0=");    dbg_hex2(OTG_DOEPCTL(0) >> 24); dbg_hex2(OTG_DOEPCTL(0) >> 16);
            uart_puts(" diep1=");    dbg_hex2(OTG_DIEPCTL(1) >> 24); dbg_hex2(OTG_DIEPCTL(1) >> 16);
            uart_puts("\r\n");
        }

        if (s_ep1_out_ready) {
            s_ep1_out_ready = 0;
            for (uint32_t i = s_ep1_out_len; i < DAP_PACKET_SIZE; i++)
                s_ep1_out[i] = 0;             /* 跟 dap_uart 一樣：殘留資料清乾淨 */

            uint32_t n = dap_process(s_ep1_out, s_ep1_in);
            if (n > DAP_PACKET_SIZE)
                n = DAP_PACKET_SIZE;

            /* ⚠ HID 的 interrupt report 是**固定長度**的，不是「想送多少就多少」。
             * dap_process() 回傳的長度是變動的（DAP_Info 可能只有幾個 byte），
             * 但主機是照描述元宣告的 64 byte 在讀。只送 n 個 byte 的話主機會
             * 讀到短封包，pyOCD 端的症狀是 "read error"。真正的 CMSIS-DAP 韌體
             * 一律送滿 64 byte、尾巴補 0——我們也照做。 */
            for (uint32_t i = n; i < DAP_PACKET_SIZE; i++)
                s_ep1_in[i] = 0;

            uart_puts("D "); dbg_hex2(s_ep1_out[0]);
            uart_puts(" n="); dbg_hex2(n); uart_puts("\r\n");

            otg_write_in(1, s_ep1_in, DAP_PACKET_SIZE);
            otg_ep1_prime_out();
        }
    }
}
