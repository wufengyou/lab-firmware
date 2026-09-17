/*
 * common/usb_otg.h —— STM32F446 OTG_FS 裝置端最小驅動（Stage 4b）
 *
 * 手刻，不用 ST HAL / CMSIS device 標頭，風格跟 swd.h / uart.h 一致。
 * 只做「device 模式、内建 FS PHY、control + 1 組 interrupt IN/OUT」——
 * 剛好夠掛一個 HID 端點餵 dap_process()，其餘（host 模式、DMA、ISO）不碰。
 *
 * ── 這一版特別依賴的板子事實（README「Stage 4b 需要的硬體」一節）──
 *
 *   - VBUS 那條線（紅）刻意不接，所以必須關掉 VBUS 偵測（GCCFG.NOVBUSSENS），
 *     否則核心會以為沒插電腦、永遠不列舉。
 *   - master 走的是內建 FS serial transceiver，不是 ULPI，所以 GUSBCFG.PHYSEL
 *     一定要設。
 *   - HSI 16 MHz 沒辦法直接餵出 USB PHY 要的 48 MHz，所以開 PLL 只為了 PLLQ
 *     輸出 48 MHz；SYSCLK **不切過去**，AHB 還是 16 MHz —— 這樣改動最小，
 *     但也是這份驅動裡唯一一個「查手冊查到、沒上機驗過」的地方：TRDT 用的是
 *     RM0390 turnaround-time 表裡 AHB=16-17MHz 對應的 0xE。**如果列舉卡住，
 *     先懷疑這一格。**
 *
 * ── 端點配置 ──
 *   EP0：control，64 byte（列舉用）
 *   EP1 IN / EP1 OUT：interrupt，64 byte（CMSIS-DAP 的 report）
 */
#pragma once
#include "stm32f446.h"
#include "delay.h"   /* delay_ms()：FDMOD 之後要等 50ms，見 otg_device_init() */

/* ---- RCC：PLL（只為了 48 MHz USB 時脈）、OTG_FS 時脈 ---- */
#define RCC_CR        REG32(0x40023800)   /* bit24 PLLON, bit25 PLLRDY */
#define RCC_PLLCFGR   REG32(0x40023804)
#define RCC_AHB2ENR   REG32(0x40023834)   /* bit7 OTGFSEN */

static inline void usb_clock_init(void)
{
    /* PLLM=8 (16MHz/8=2MHz VCO 輸入), PLLN=96 (VCO=192MHz), PLLQ=4 (192/4=48MHz)。
     * PLLP 留預設（不用，SYSCLK 不切過去）。 */
    RCC_PLLCFGR = (8u << 0) | (96u << 6) | (4u << 24) | (1u << 22); /* PLLSRC=HSI(bit22=0)... */
    RCC_PLLCFGR &= ~(1u << 22);   /* PLLSRC = 0 = HSI，寫清楚一點 */
    RCC_CR |= (1u << 24);         /* PLLON */
    while (!(RCC_CR & (1u << 25)))/* PLLRDY */
        ;
    RCC_AHB2ENR |= (1u << 7);     /* OTGFSEN */
}

/* ---- OTG_FS 暫存器（base 0x50000000，RM0390 ch.22）---- */
#define OTG  0x50000000u
#define OTG_GOTGCTL   REG32(OTG + 0x000)
#define OTG_GAHBCFG   REG32(OTG + 0x008)   /* bit0 GINTMSK */
#define OTG_GUSBCFG   REG32(OTG + 0x00C)
#define OTG_GRSTCTL   REG32(OTG + 0x010)
#define OTG_GINTSTS   REG32(OTG + 0x014)
#define OTG_GINTMSK   REG32(OTG + 0x018)
#define OTG_GRXSTSR   REG32(OTG + 0x01C)   /* 讀不 pop */
#define OTG_GRXSTSP   REG32(OTG + 0x020)   /* 讀會 pop */
#define OTG_GRXFSIZ   REG32(OTG + 0x024)
#define OTG_DIEPTXF0  REG32(OTG + 0x028)   /* EP0 TX FIFO（device 模式） */
#define OTG_GCCFG     REG32(OTG + 0x038)

#define OTG_DCFG      REG32(OTG + 0x800)
#define OTG_DCTL      REG32(OTG + 0x804)
#define OTG_DSTS      REG32(OTG + 0x808)
#define OTG_DIEPMSK   REG32(OTG + 0x810)
#define OTG_DOEPMSK   REG32(OTG + 0x814)
#define OTG_DAINT     REG32(OTG + 0x818)
#define OTG_DAINTMSK  REG32(OTG + 0x81C)

#define OTG_DIEPCTL(i)  REG32(OTG + 0x900 + (i) * 0x20)
#define OTG_DIEPINT(i)  REG32(OTG + 0x908 + (i) * 0x20)
#define OTG_DIEPTSIZ(i) REG32(OTG + 0x910 + (i) * 0x20)
#define OTG_DTXFSTS(i)  REG32(OTG + 0x918 + (i) * 0x20)

#define OTG_DOEPCTL(i)  REG32(OTG + 0xB00 + (i) * 0x20)
#define OTG_DOEPINT(i)  REG32(OTG + 0xB08 + (i) * 0x20)
#define OTG_DOEPTSIZ(i) REG32(OTG + 0xB10 + (i) * 0x20)

#define OTG_DIEPTXF(i)  REG32(OTG + 0x104 + ((i) - 1) * 0x04)  /* i=1..3 */

#define OTG_FIFO(i)     ((volatile uint32_t *)(OTG + 0x1000 + (i) * 0x1000))

/* GINTSTS / GINTMSK 位元 */
#define OTG_INT_RXFLVL   (1u << 4)
#define OTG_INT_USBRST   (1u << 12)
#define OTG_INT_ENUMDNE  (1u << 13)
#define OTG_INT_IEPINT   (1u << 18)
#define OTG_INT_OEPINT   (1u << 19)

/* GRXSTSP 的 PKTSTS 值 */
#define RXSTS_SETUP_COMP   4u
#define RXSTS_SETUP_RECV   6u
#define RXSTS_OUT_RECV     2u
#define RXSTS_OUT_COMP     3u

#define EP0_MPS   64u
#define EP1_MPS   64u

/* DIEPCTL/DOEPCTL 共用位元 */
#define EPCTL_EPENA   (1u << 31)
#define EPCTL_EPDIS   (1u << 30)
#define EPCTL_SD0PID  (1u << 28)   /* device IN: SD0PID/SODDFRM 同一個 bit */
#define EPCTL_SNAK    (1u << 27)
#define EPCTL_CNAK    (1u << 26)
#define EPCTL_USBAEP  (1u << 15)
#define EPCTL_EPTYP_CTRL  (0u << 18)
#define EPCTL_EPTYP_INT   (3u << 18)

static uint8_t s_setup_pkt[8];

/* 核心重置：GRSTCTL.CSRST，等 AHBIDL 再等 CSRST 自己清掉 */
static inline void otg_core_reset(void)
{
    while (!(OTG_GRSTCTL & (1u << 31)))   /* AHBIDL */
        ;
    OTG_GRSTCTL |= 1u;                    /* CSRST */
    while (OTG_GRSTCTL & 1u)
        ;
}

static inline void otg_flush_rxfifo(void)
{
    OTG_GRSTCTL = (1u << 4);              /* RXFFLSH */
    while (OTG_GRSTCTL & (1u << 4))
        ;
}
static inline void otg_flush_txfifo(uint32_t ep)
{
    OTG_GRSTCTL = (1u << 5) | (ep << 6);  /* TXFFLSH | FIFO num */
    while (OTG_GRSTCTL & (1u << 5))
        ;
}

/* 初始化到「等 USB reset」為止：時脈、PHY、device 模式、全域中斷開關。
 * 端點本身要等 USBRST / ENUMDNE 兩個中斷發生才設（見 usb_hid.c 的狀態機），
 * 因為 reset 之前設的東西 reset 一來全部歸零，白工。 */
static inline void otg_device_init(void)
{
    usb_clock_init();

    RCC_AHB1ENR |= (1u << PORTA);
    gpio_mode(PORTA, 11, 2); gpio_af(PORTA, 11, 10);   /* PA11 = OTG_FS_DM, AF10 */
    gpio_mode(PORTA, 12, 2); gpio_af(PORTA, 12, 10);   /* PA12 = OTG_FS_DP, AF10 */

    /* 以下順序照 ST HAL 的 USB_CoreInit / USB_SetCurrentMode / USB_DevInit
     * 三支函式實測過的順序重排（2026-09-14 上機才發現順序真的重要，不是
     * 隨便排都通）：PHYSEL 要在 core reset **之前**設，FDMOD 設完要等
     * 50ms 才能碰其他暫存器——這條規定在 RM0390 GUSBCFG.FDMOD 那格有寫，
     * 但沒等的話症狀不是報錯，是「core init done 照樣印得出來、D+ 卻永遠
     * 量不到 3.3V 上拉」，非常容易被誤判成接線問題（這裡就真的誤判過一輪）。 */
    OTG_GUSBCFG |= (1u << 6);              /* PHYSEL：用內建 FS 收發器（要在 reset 之前） */
    otg_core_reset();
    OTG_GCCFG = (1u << 16);                /* PWRDWN，VBDEN 故意不設（見下面 VBUS 那段） */

    OTG_GUSBCFG &= ~((1u << 29) | (1u << 30));  /* 先清 FHMOD/FDMOD */
    OTG_GUSBCFG |= (1u << 30);                  /* FDMOD：強制 device 模式 */
    delay_ms(50);                               /* ⚠ 沒等這 50ms，D+ 拉不起來 */

    /* 沒接 VBUS：關掉 VBUS 偵測，否則核心永遠以為沒插電腦。
     *
     * F446 跟其他 F4 系列的 GCCFG 佈局不一樣：其他 F4（F405/F407…）是
     * 「NOVBUSSENS=1 代表關掉偵測」；F446（連同 F469/F412）在同一個 bit
     * 位置放的是 VBDEN，語意剛好相反——**設成 1 是打開 VBUS 偵測，不是
     * 關掉**。F446 正確做法是 VBDEN 保持清空，改用 GOTGCTL 的 override bit
     * 騙核心「B-session 有效」。 */
    OTG_GOTGCTL |= (1u << 6) | (1u << 7);  /* BVALOEN | BVALOVAL */

    OTG_DCFG = (OTG_DCFG & ~3u) | 3u;      /* DSPD = 11：Full speed（內建 FS PHY） */

    otg_flush_txfifo(0x10);                /* 0x10 = 全部 TX FIFO */
    otg_flush_rxfifo();

    OTG_DIEPMSK = 0; OTG_DOEPMSK = 0; OTG_DAINTMSK = 0;
    OTG_DAINT = 0xFFFFFFFFu;
    OTG_GINTSTS = 0xFFFFFFFFu;              /* 清掉重排過程中可能纍積的假中斷 */

    OTG_GINTMSK = OTG_INT_USBRST | OTG_INT_ENUMDNE | OTG_INT_RXFLVL
                | OTG_INT_IEPINT | OTG_INT_OEPINT;
    OTG_GAHBCFG |= 1u;                     /* GINTMSK：全域中斷開關（我們是 polling GINTSTS，不用 NVIC） */

    /* ⚠ 2026-09-14 上機才發現：光是上面這些設定完，D+ 還是不會被拉高。
     * ST HAL 把「連接」拆成獨立一步（USB_DevConnect），清掉 DCTL.SDIS
     * （soft disconnect）才會真的驅動 D+ 上拉——之前這裡整個沒寫過 DCTL，
     * 等於韌體從頭到尾都停在「軟體斷線」狀態，電腦當然什麼都偵測不到。 */
    OTG_DCTL &= ~(1u << 1);                /* 清 SDIS：真正對電腦「掛上去」 */
    delay_ms(3);
}

/* USBRST 中斷處理：把所有端點打回重置後的樣子 */
static inline void otg_on_reset(void)
{
    /* ⚠ USB reset 之後裝置一定要回到位址 0——這是 USB 規格，不是選配。
     * 核心不會自己清 DCFG.DAD，ST HAL 在 USBRST 明確呼叫 USB_SetDevAddress(0)。
     * 不清的話，主機重試一輪時會在位址 0 跟我們講話，而我們還停在上一輪拿到的
     * 位址上，於是整個列舉卡在「SET_ADDRESS 成功之後就沒下文」的迴圈裡
     * （2026-09-14 踩到，症狀就是 USBRST/ENUMDNE/SET_ADDRESS 無限重複）。 */
    OTG_DCFG &= ~(0x7Fu << 4);

    for (uint32_t i = 0; i < 4; i++) {
        OTG_DIEPCTL(i) = 0;
        OTG_DOEPCTL(i) = 0;
        OTG_DIEPINT(i) = 0xFF;
        OTG_DOEPINT(i) = 0xFF;
    }
    OTG_DAINTMSK = (1u << 0) | (1u << 16);     /* IN EP0 + OUT EP0 */
    OTG_DOEPMSK  = (1u << 0) | (1u << 3);      /* XFRC | STUP */
    OTG_DIEPMSK  = (1u << 0);                  /* XFRC */

    OTG_GRXFSIZ = 64u;                          /* RX FIFO：64 word，餵 EP0+EP1 OUT 都夠 */
    OTG_DIEPTXF0 = (64u << 16) | 64u;           /* EP0 TX FIFO：offset=64, size=64 word */
    OTG_DIEPTXF(1) = (128u << 16) | 64u;        /* EP1 TX FIFO：offset=128, size=64 word */
}

/* ENUMDNE 中斷處理：這時才知道列舉出來的速度、才能把 EP0 收 SETUP 的路徑打開 */
static inline void otg_on_enum_done(void)
{
    OTG_DIEPCTL(0) = (OTG_DIEPCTL(0) & ~0x3u) | 0x0u;  /* MPSIZ=0 -> 64 byte（EP0 專用編碼） */
    OTG_DOEPCTL(0) |= EPCTL_USBAEP;

    /* 16 MHz AHB：turnaround time 用 RM0390 表查到的 0xE（沒上機驗過，卡列舉先看這裡）*/
    OTG_GUSBCFG = (OTG_GUSBCFG & ~(0xFu << 10)) | (0xEu << 10);

    OTG_DOEPTSIZ(0) = (3u << 29) | (1u << 19) | EP0_MPS;   /* STUPCNT=3, PKTCNT=1, XFRSIZ=MPS */
    OTG_DOEPCTL(0) |= EPCTL_EPENA | EPCTL_CNAK;
}

/* 把一包 SETUP/OUT 資料從 RXFIFO 搬到記憶體（word 對齊，最後不足一個 word 也要整個讀掉） */
static inline void otg_read_fifo(uint8_t *dst, uint32_t nbytes)
{
    uint32_t nwords = (nbytes + 3u) / 4u;
    for (uint32_t i = 0; i < nwords; i++) {
        uint32_t w = *OTG_FIFO(0);
        for (uint32_t b = 0; b < 4 && (i * 4 + b) < nbytes; b++)
            dst[i * 4 + b] = (uint8_t)(w >> (b * 8));
    }
}

/* 把 nbytes 從 RX FIFO 讀掉丟棄。用在「認不得但還是得清乾淨」的封包上——
 * RX FIFO 的狀態項目（GRXSTSP）跟資料是分開的，只 pop 狀態不讀資料，資料會
 * 一直卡在那裡把後面的封包全堵住。 */
static inline void otg_drain_fifo(uint32_t nbytes)
{
    uint32_t nwords = (nbytes + 3u) / 4u;
    for (uint32_t i = 0; i < nwords; i++)
        (void)*OTG_FIFO(0);
}

/* 把 EP0 或 EP1 IN 的資料寫進對應的 TX FIFO 並讓硬體發出去 */
static inline void otg_write_in(uint32_t ep, const uint8_t *src, uint32_t nbytes)
{
    OTG_DIEPTSIZ(ep) = (1u << 19) | nbytes;   /* PKTCNT=1 */
    OTG_DIEPCTL(ep) |= EPCTL_EPENA | EPCTL_CNAK;

    uint32_t nwords = (nbytes + 3u) / 4u;
    for (uint32_t i = 0; i < nwords; i++) {
        uint32_t w = 0;
        for (uint32_t b = 0; b < 4 && (i * 4 + b) < nbytes; b++)
            w |= ((uint32_t)src[i * 4 + b]) << (b * 8);
        *OTG_FIFO(ep) = w;
    }
}

static inline void otg_ep0_stall(void)
{
    OTG_DIEPCTL(0) |= (1u << 21);   /* STALL */
    OTG_DOEPCTL(0) |= (1u << 21);
}

/* EP1（HID interrupt IN/OUT）打開：dap_usb/main.c 在 SET_CONFIGURATION 之後呼叫一次 */
static inline void otg_ep1_enable(void)
{
    OTG_DAINTMSK |= (1u << 1) | (1u << 17);     /* IN EP1 + OUT EP1 */
    OTG_DIEPCTL(1) = EPCTL_USBAEP | EPCTL_EPTYP_INT | EPCTL_SD0PID | (1u << 22) /* TXFNUM=1 */ | EP1_MPS;
    OTG_DOEPCTL(1) = EPCTL_USBAEP | EPCTL_EPTYP_INT | EPCTL_SD0PID | EP1_MPS;
    OTG_DOEPTSIZ(1) = (1u << 19) | EP1_MPS;     /* PKTCNT=1 */
    OTG_DOEPCTL(1) |= EPCTL_EPENA | EPCTL_CNAK;
}

static inline void otg_ep1_prime_out(void)
{
    OTG_DOEPTSIZ(1) = (1u << 19) | EP1_MPS;
    OTG_DOEPCTL(1) |= EPCTL_EPENA | EPCTL_CNAK;
}
