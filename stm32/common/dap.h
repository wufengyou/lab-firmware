/*
 * common/dap.h —— CMSIS-DAP 指令層（Stage 4a）
 *
 * 把一包 CMSIS-DAP 命令解析成 SWD 動作，再把結果組成回應包。
 *
 * ── 為什麼這一層要跟傳輸層分開 ──
 *
 * 真正的 CMSIS-DAP probe 是 USB HID 裝置：主機送 64 byte 的 HID report，
 * probe 回 64 byte。但**「解析 DAP 包」跟「包怎麼送到」是兩件無關的事**。
 *
 * 拆開的好處是可以先做完、先驗完指令層，完全不碰 USB：
 *   Stage 4a —— 這個檔案 + dap_uart/，走現有的 UART，host 端用 Python 餵包。
 *   Stage 4b —— 同一個 dap_process() 接到 OTG_FS 的 HID 端點上。
 *
 * 不這樣拆的話，USB 列舉失敗時你分不出是描述元寫錯還是 DAP 解析錯 ——
 * 又回到「一次動兩個變數」的老問題。
 *
 * ── 範圍 ──
 *
 * 只做 SWD，不做 JTAG（我們的 swd.h 本來就只有 SWD）。實作的命令是
 * OpenOCD / pyOCD 連線與基本讀寫會用到的那些；其餘回 0xFF（invalid command），
 * 這是 CMSIS-DAP 規定的作法，主機看得懂。
 *
 * **沒有實作的東西刻意留成「明確拒絕」而不是「假裝成功」** ——
 * 假裝成功會讓主機以為設定生效了，之後在別的地方以難懂的方式失敗。
 */
#ifndef DAP_H
#define DAP_H

#include <stdint.h>
#include "swd.h"

#define DAP_PACKET_SIZE   64u

/* ── 命令碼（CMSIS-DAP v1.2）── */
#define ID_DAP_Info               0x00u
#define ID_DAP_HostStatus         0x01u
#define ID_DAP_Connect            0x02u
#define ID_DAP_Disconnect         0x03u
#define ID_DAP_TransferConfigure  0x04u
#define ID_DAP_Transfer           0x05u
#define ID_DAP_TransferBlock      0x06u
#define ID_DAP_TransferAbort      0x07u
#define ID_DAP_WriteABORT         0x08u
#define ID_DAP_Delay              0x09u
#define ID_DAP_ResetTarget        0x0Au
#define ID_DAP_SWJ_Pins           0x10u
#define ID_DAP_SWJ_Clock          0x11u
#define ID_DAP_SWJ_Sequence       0x12u
#define ID_DAP_SWD_Configure      0x13u

#define DAP_OK                    0x00u
#define DAP_ERROR                 0xFFu

/* DAP_Transfer 的回應位元組：低 3 bit 是 SWD 的 ACK，bit3 是 protocol error。 */
#define DAP_XFER_OK               0x01u
#define DAP_XFER_WAIT             0x02u
#define DAP_XFER_FAULT            0x04u
#define DAP_XFER_ERROR            0x08u

static uint8_t dap_idle_cycles;          /* DAP_TransferConfigure 設定的 */
static uint16_t dap_retry_count = 100;
static uint16_t dap_match_retry;

static inline uint32_t dap__rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void dap__wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint32_t dap__str(uint8_t *dst, const char *s)
{
    uint32_t n = 0;
    while (s[n]) {
        dst[n] = (uint8_t)s[n];
        n++;
    }
    dst[n++] = 0;                        /* CMSIS-DAP 的字串含結尾 0 */
    return n;
}

/* ── 把 swd_xfer 的回傳值翻成 DAP 的回應位元組 ──
 * swd.h 的慣例：1=OK、2=WAIT、4=FAULT、7/0=無回應、-1=parity 錯。
 * DAP 的慣例：低 3 bit 放 ACK，parity/protocol 錯另外用 bit3。
 * **兩套編碼長得很像但不一樣**，這種地方最容易寫成直接回傳了事。 */
static inline uint8_t dap__ack_to_resp(int ack)
{
    if (ack == 1) return DAP_XFER_OK;
    if (ack == 2) return DAP_XFER_WAIT;
    if (ack == 4) return DAP_XFER_FAULT;
    if (ack < 0)  return DAP_XFER_ERROR;          /* parity */
    return DAP_XFER_ERROR;                        /* 7 或 0：線上沒人回應 */
}

/* ── DAP_Info ── */
static inline uint32_t dap__info(const uint8_t *req, uint8_t *resp)
{
    uint8_t id = req[1];
    uint32_t n = 0;

    resp[0] = ID_DAP_Info;

    switch (id) {
    case 0x01: n = dap__str(&resp[2], "board-lab"); break;
    case 0x02: n = dap__str(&resp[2], "F446RE SWD probe"); break;
    case 0x03: n = dap__str(&resp[2], "0001"); break;
    case 0x04: n = dap__str(&resp[2], "1.2.0"); break;   /* 宣稱的 CMSIS-DAP 版本 */
    case 0x05: n = dap__str(&resp[2], ""); break;        /* target device vendor */
    case 0x06: n = dap__str(&resp[2], ""); break;        /* target device name */

    case 0xF0:                                            /* capabilities */
        /* bit0 = SWD、bit1 = JTAG。我們只有 SWD —— **不要順手把 JTAG 也宣稱了**，
         * 主機會據此決定要不要跟你講 JTAG。 */
        resp[2] = 0x01;
        n = 1;
        break;

    case 0xFE:                                            /* packet count */
        resp[2] = 1;
        n = 1;
        break;

    case 0xFF:                                            /* packet size */
        resp[2] = (uint8_t)DAP_PACKET_SIZE;
        resp[3] = (uint8_t)(DAP_PACKET_SIZE >> 8);
        n = 2;
        break;

    default:
        n = 0;
        break;
    }

    resp[1] = (uint8_t)n;
    return 2 + n;
}

/* ── DAP_Transfer ──
 * 請求：[0]=cmd [1]=DAP index [2]=transfer 數，之後每筆是
 *       request byte（bit0 APnDP、bit1 RnW、bit2-3 暫存器位址 A[3:2]），
 *       寫入的話再跟 4 byte 資料。
 * 回應：[0]=cmd [1]=實際做完幾筆 [2]=最後一筆的回應，之後是讀到的資料。
 *
 * **失敗就停**：CMSIS-DAP 規定一旦某筆不是 OK 就中止，回報做到第幾筆。
 * 主機靠這個數字知道哪一筆壞了。 */
/* 帶 WAIT 重試的單筆傳輸。WAIT 是 target 忙，不是錯誤。 */
static inline int dap__xfer_retry(uint32_t apndp, uint32_t rnw, uint32_t reg, uint32_t *data)
{
    int ack;
    for (uint32_t r = 0; ; r++) {
        ack = swd_xfer(apndp, rnw, reg, data);
        if (ack != 2 || r >= dap_retry_count)
            return ack;
    }
}

#define DAP_DP_RDBUFF  0x0Cu

static inline uint32_t dap__transfer(const uint8_t *req, uint8_t *resp)
{
    uint32_t count = req[2];
    const uint8_t *p = &req[3];
    uint8_t *out = &resp[3];
    uint32_t done = 0;
    uint8_t last = DAP_XFER_OK;
    uint32_t post_read = 0;      /* 有一筆 AP 讀取已經送出、結果還沒收回來 */

    resp[0] = ID_DAP_Transfer;

    /* ── ⚠ AP 的讀取是「延後的」(posted) ──
     *
     * 在 SWD 上送出一筆 AP read，線上回來的資料是**上一次** AP read 的結果；
     * 這一次的結果要等下一筆 AP read，或是讀 DP 的 RDBUFF(0x0C) 才拿得到。
     *
     * `swd.h` 的 `swd__ap_rd()` 自己補了 RDBUFF 那一筆，所以 Stage 1-3.5 都對。
     * 但這裡不能用它 —— DAP_Transfer 必須忠實執行主機給的每一筆，
     * 而 **CMSIS-DAP 規格把「處理 posted read」這件事規定成韌體的責任**
     * （ARM 官方 DAP.c 的 DAP_SWD_Transfer 就是這個狀態機）。
     *
     * 2026-09-14 踩到：原本這裡直接呼叫原始的 swd_xfer()，AP 讀回來的
     * 永遠是過期資料。Stage 4a 的 dap.ps1 測不出來，因為它是在**主機端**
     * 自己補 RDBUFF；pyOCD 照規格假設韌體會做，於是讀 AP IDR 讀到垃圾，
     * 報 `Invalid AP address (#0)`。
     *
     * 規則整理（下面的程式碼就是這三條）：
     *   - 讀 AP：如果還沒有 posted 的讀取，先送一筆把它 post 出去（結果丟掉）；
     *     之後每一筆 AP read 收回來的都是「上一筆」的結果。
     *   - 讀 DP 或任何寫入之前：如果有 posted 的讀取沒收，先讀 RDBUFF 收回來。
     *   - 迴圈結束時還有 posted 的：補一筆 RDBUFF。
     */
    for (uint32_t i = 0; i < count; i++) {
        uint8_t rq = *p++;
        uint32_t apndp = rq & 1u;
        uint32_t rnw = (rq >> 1) & 1u;
        uint32_t reg = (uint32_t)(rq & 0x0Cu);      /* A[3:2] 已經在 bit2-3 上 */
        uint32_t data = 0;
        int ack;

        /* value match / match mask（bit4/bit5）沒有實作。
         * **明確拒絕，不要假裝成功** —— 主機用它做 polling，假裝成功會讓它
         * 以為條件已達成而繼續往下跑。 */
        if (rq & 0x30u) {
            last = DAP_XFER_ERROR;
            break;
        }

        if (rnw) {
            if (post_read) {
                /* 上一筆 posted 的 AP 讀取還沒收。這一筆如果也是 AP 讀取，
                 * 就順便用它把上一筆收回來（同時把這一筆 post 出去）；
                 * 否則得用 RDBUFF 收，收完就不再是 posted 狀態。 */
                if (apndp) {
                    ack = dap__xfer_retry(1, 1, reg, &data);
                } else {
                    ack = dap__xfer_retry(0, 1, DAP_DP_RDBUFF, &data);
                    post_read = 0;
                }
                last = dap__ack_to_resp(ack);
                if (last != DAP_XFER_OK)
                    break;
                dap__wr32(out, data);       /* 存的是**上一筆**的結果 */
                out += 4;
            }

            if (apndp) {
                if (!post_read) {
                    /* 把這一筆 AP 讀取 post 出去，回來的資料是舊的，丟掉。 */
                    uint32_t dummy = 0;
                    ack = dap__xfer_retry(1, 1, reg, &dummy);
                    last = dap__ack_to_resp(ack);
                    if (last != DAP_XFER_OK)
                        break;
                    post_read = 1;
                }
            } else {
                /* DP 的讀取不是 posted，當場就拿得到值。 */
                ack = dap__xfer_retry(0, 1, reg, &data);
                last = dap__ack_to_resp(ack);
                if (last != DAP_XFER_OK)
                    break;
                dap__wr32(out, data);
                out += 4;
            }
        } else {
            if (post_read) {
                /* 寫入之前一定要先把欠著的讀取結果收回來，否則它會被沖掉。 */
                ack = dap__xfer_retry(0, 1, DAP_DP_RDBUFF, &data);
                last = dap__ack_to_resp(ack);
                if (last != DAP_XFER_OK)
                    break;
                dap__wr32(out, data);
                out += 4;
                post_read = 0;
            }

            data = dap__rd32(p);
            p += 4;
            ack = dap__xfer_retry(apndp, 0, reg, &data);
            last = dap__ack_to_resp(ack);
            if (last != DAP_XFER_OK)
                break;
        }

        done++;
    }

    /* 收尾：還欠一筆沒收的話補讀 RDBUFF。 */
    if (last == DAP_XFER_OK && post_read) {
        uint32_t data = 0;
        int ack = dap__xfer_retry(0, 1, DAP_DP_RDBUFF, &data);
        last = dap__ack_to_resp(ack);
        if (last == DAP_XFER_OK) {
            dap__wr32(out, data);
            out += 4;
        }
    }

    resp[1] = (uint8_t)done;
    resp[2] = last;
    return (uint32_t)(out - resp);
}

/* ── DAP_TransferBlock ──
 * 同一個暫存器連續讀或寫 N 次。這是 OpenOCD 下載韌體時的主力命令 ——
 * 每筆都用 DAP_Transfer 的話，光是 request byte 就浪費掉一半頻寬。 */
static inline uint32_t dap__transfer_block(const uint8_t *req, uint8_t *resp)
{
    uint32_t count = (uint32_t)req[2] | ((uint32_t)req[3] << 8);
    uint8_t rq = req[4];
    const uint8_t *p = &req[5];
    uint8_t *out = &resp[4];
    uint32_t apndp = rq & 1u;
    uint32_t rnw = (rq >> 1) & 1u;
    uint32_t reg = (uint32_t)(rq & 0x0Cu);
    uint32_t done = 0;
    uint8_t last = DAP_XFER_OK;

    resp[0] = ID_DAP_TransferBlock;

    /* AP 的讀取是 posted 的，理由跟 dap__transfer() 那一長段註解完全相同。
     * 區塊讀取的處理比較簡單，因為整段都是同一個暫存器：
     *   開頭先 post 一筆（結果丟掉），中間每一筆收到的都是前一筆的結果，
     *   **最後一筆改成讀 RDBUFF** 把最後的值收回來。 */
    if (rnw && apndp && count > 0) {
        uint32_t dummy = 0;
        int ack = dap__xfer_retry(1, 1, reg, &dummy);
        last = dap__ack_to_resp(ack);
        if (last != DAP_XFER_OK)
            count = 0;                  /* 連 post 都失敗，下面整個不用跑 */
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t data = 0;
        int ack;

        if (!rnw) {
            data = dap__rd32(p);
            p += 4;
            ack = dap__xfer_retry(apndp, 0, reg, &data);
        } else if (apndp && i == count - 1) {
            ack = dap__xfer_retry(0, 1, DAP_DP_RDBUFF, &data);   /* 最後一筆收尾 */
        } else {
            ack = dap__xfer_retry(apndp, 1, reg, &data);
        }

        last = dap__ack_to_resp(ack);
        if (last != DAP_XFER_OK)
            break;

        if (rnw) {
            dap__wr32(out, data);
            out += 4;
        }
        done++;
    }

    resp[1] = (uint8_t)done;
    resp[2] = (uint8_t)(done >> 8);
    resp[3] = last;
    return (uint32_t)(out - resp);
}

/* ── 主入口 ──
 * req/resp 都是至少 DAP_PACKET_SIZE 大小的緩衝區。回傳回應的長度。 */
static inline uint32_t dap_process(const uint8_t *req, uint8_t *resp)
{
    switch (req[0]) {

    case ID_DAP_Info:
        return dap__info(req, resp);

    case ID_DAP_HostStatus:
        /* 主機要 probe 亮 LED（connect / running）。我們沒有接 LED，
         * 但要回 OK —— 這個命令失敗會讓某些主機端放棄連線。 */
        resp[0] = ID_DAP_HostStatus;
        resp[1] = DAP_OK;
        return 2;

    case ID_DAP_Connect: {
        /* port：0=預設、1=SWD、2=JTAG。回傳實際用的 port，0 代表失敗。 */
        uint8_t port = req[1];
        resp[0] = ID_DAP_Connect;
        if (port == 0 || port == 1) {
            swd_pins_init();
            resp[1] = 1;                 /* SWD */
        } else {
            resp[1] = 0;                 /* JTAG：我們沒有 */
        }
        return 2;
    }

    case ID_DAP_Disconnect:
        resp[0] = ID_DAP_Disconnect;
        resp[1] = DAP_OK;
        return 2;

    case ID_DAP_TransferConfigure:
        dap_idle_cycles = req[1];
        dap_retry_count = (uint16_t)req[2] | ((uint16_t)req[3] << 8);
        dap_match_retry = (uint16_t)req[4] | ((uint16_t)req[5] << 8);
        if (dap_retry_count == 0)
            dap_retry_count = 1;
        resp[0] = ID_DAP_TransferConfigure;
        resp[1] = DAP_OK;
        return 2;

    case ID_DAP_Transfer:
        return dap__transfer(req, resp);

    case ID_DAP_TransferBlock:
        return dap__transfer_block(req, resp);

    case ID_DAP_WriteABORT: {
        uint32_t v = dap__rd32(&req[2]);
        int ack = swd_xfer(0, 0, 0x0, &v);       /* DP 的 ABORT 在 0x0（寫） */
        resp[0] = ID_DAP_WriteABORT;
        resp[1] = (ack == 1) ? DAP_OK : DAP_ERROR;
        return 2;
    }

    case ID_DAP_Delay: {
        uint32_t us = (uint32_t)req[1] | ((uint32_t)req[2] << 8);
        delay_us(us);
        resp[0] = ID_DAP_Delay;
        resp[1] = DAP_OK;
        return 2;
    }

    case ID_DAP_SWJ_Clock:
        /* 主機指定 SWCLK 頻率。我們的 SWD_DLY() 是寫死的迴圈延遲，調不了 ——
         * 所以這裡**回 OK 但實際上沒有照做**。這是唯一一個「假裝成功」的地方，
         * 理由是：CMSIS-DAP 沒有「我只有固定速度」這個回答，而回 ERROR 會讓
         * 主機直接放棄連線。代價是主機以為自己設定的頻率生效了。
         * 想真的支援就要把 SWD_DLY() 改成可變的 —— 記在 README 的「還沒做」。 */
        resp[0] = ID_DAP_SWJ_Clock;
        resp[1] = DAP_OK;
        return 2;

    case ID_DAP_SWJ_Sequence: {
        /* 主機送任意長度的位元序列（LSB first），probe 照著在 SWDIO 上打出來。
         * 主機用它做 line reset 與 JTAG-to-SWD 切換 —— 也就是說**接上之後
         * 主機會自己做這件事，不需要我們的 swd__line_reset_and_switch()**。 */
        uint32_t bits = req[1];
        if (bits == 0)
            bits = 256;
        swd__drive();
        for (uint32_t i = 0; i < bits; i++) {
            uint8_t byte = req[2 + (i >> 3)];
            swd__wr((byte >> (i & 7)) & 1u);
        }
        resp[0] = ID_DAP_SWJ_Sequence;
        resp[1] = DAP_OK;
        return 2;
    }

    case ID_DAP_SWJ_Pins: {
        /* bit0 SWCLK、bit1 SWDIO、bit7 nRESET。
         * 我們只實作 nRESET（PB10 有拉出來）—— 其餘位元讀回目前狀態。 */
        uint8_t value = req[1];
        uint8_t select = req[2];
        uint8_t state = 0;

        if (select & 0x80u) {
            if (value & 0x80u)
                gpio_set(SWD_NRST_PORT, SWD_NRST_PIN);
            else
                gpio_clr(SWD_NRST_PORT, SWD_NRST_PIN);
        }
        if (gpio_get(SWD_CLK_PORT, SWD_CLK_PIN)) state |= 0x01u;
        if (swd__dio_rd())                        state |= 0x02u;
        if (gpio_get(SWD_NRST_PORT, SWD_NRST_PIN)) state |= 0x80u;

        resp[0] = ID_DAP_SWJ_Pins;
        resp[1] = state;
        return 2;
    }

    case ID_DAP_SWD_Configure:
        /* turnaround 週期數與 data phase 設定。我們的 swd.h 寫死 1 個 turnaround、
         * 不做 data phase —— 這是絕大多數 target 的預設值。
         * 主機若要求別的值，**明確拒絕**，不要讓它以為設定生效了。 */
        resp[0] = ID_DAP_SWD_Configure;
        resp[1] = (req[1] == 0) ? DAP_OK : DAP_ERROR;
        return 2;

    case ID_DAP_ResetTarget:
        /* 「用硬體特有的方式重置 target」。我們沒有實作（nRESET 走 SWJ_Pins），
         * 照規定回 execute=0 表示沒做。 */
        resp[0] = ID_DAP_ResetTarget;
        resp[1] = DAP_OK;
        resp[2] = 0;
        return 3;

    default:
        /* CMSIS-DAP 規定不認得的命令回 0xFF。主機看得懂這個。 */
        resp[0] = DAP_ERROR;
        return 1;
    }
}

#endif /* DAP_H */
