/*
 * nano130/keypad_lcd —— 4x4 keypad 打字，顯示到 LCD1602，可清零循環
 *
 * 目標板：WENG NANO 客製版。腳位速查：../../iot_lab_research/nano130_firmware_resource_map.md
 * 本檔用到的腳位：
 *
 *   LCD1602（8-bit 並列，RW 接腳但韌體固定拉低＝只寫）
 *     DB0..DB7 = PD.0 .. PD.7      （整個 port D 低位元組）
 *     RS       = PC.15
 *     RW       = PC.14   （固定 0）
 *     E        = PB.15
 *     VO 對比  = 板上 VR1 10K 電位器，跟 MCU 無關
 *
 *   KEYPAD K1（4x4 矩陣，板上無上拉 → 用 MCU 內建 PUEN 上拉）
 *     列 R1..R4 = PC.0 .. PC.3     （推挽輸出，掃描時逐一拉低）
 *     行 C1..C4 = PC.4, PC.5, PD.15, PD.14   （輸入 + 內建上拉，讀到低＝該鍵按下）
 *
 * 操作：
 *   0-9 A B C D *  → append 到第二行
 *   #              → 清螢幕、回到起點（這就是「清零／循環」）
 *   第二行滿 16 字 → 下一鍵自動先清一次再收
 *
 * 時脈：重置後預設 HIRC 12 MHz，delay 都是抓大概，LCD/keypad 容忍度夠。
 */
#include "Nano100Series.h"

/* ---- 粗略延時（HIRC 12 MHz、-Os）。實測 ×3 太短，改 ×10 才接近真 us ---- */
static void delay_us(uint32_t us)
{
    volatile uint32_t n = us * 10u;
    while (n--) __NOP();
}
static void delay_ms(uint32_t ms)
{
    while (ms--) delay_us(1000);
}

/* ---- LCD1602 ---- */
#define LCD_RS_H()   (PC->DOUT |=  (1u << 15))
#define LCD_RS_L()   (PC->DOUT &= ~(1u << 15))
#define LCD_E_H()    (PB->DOUT |=  (1u << 15))
#define LCD_E_L()    (PB->DOUT &= ~(1u << 15))

static void lcd_put_bus(uint8_t v)
{
    PD->DOUT = (PD->DOUT & 0xFF00u) | v;      /* PD.0..7 */
}

static void lcd_strobe(void)
{
    LCD_E_H();
    delay_us(2);
    LCD_E_L();
    delay_us(50);
}

static void lcd_cmd(uint8_t c)
{
    LCD_RS_L();
    lcd_put_bus(c);
    lcd_strobe();
}

static void lcd_data(uint8_t d)
{
    LCD_RS_H();
    lcd_put_bus(d);
    lcd_strobe();
}

static void lcd_goto(uint32_t row, uint32_t col)
{
    lcd_cmd(0x80u | (col + (row ? 0x40u : 0x00u)));
}

static void lcd_puts(const char *s)
{
    while (*s) lcd_data((uint8_t)*s++);
}

static void lcd_clear(void)
{
    lcd_cmd(0x01);
    delay_ms(2);
}

static void lcd_init(void)
{
    delay_ms(50);                 /* 上電等待 */
    lcd_cmd(0x30); delay_ms(5);   /* 8-bit 喚醒三次 */
    lcd_cmd(0x30); delay_us(200);
    lcd_cmd(0x30); delay_us(200);
    lcd_cmd(0x38);                /* function set: 8-bit, 2 line, 5x8 */
    lcd_cmd(0x08);                /* display off */
    lcd_clear();
    lcd_cmd(0x06);                /* entry mode: 位址遞增、畫面不捲動 */
    lcd_cmd(0x0C);                /* display on, cursor/blink off */
}

/* ---- 4x4 keypad ---- */
static const char KEYMAP[4][4] = {
    { '1', '2', '3', 'A' },
    { '4', '5', '6', 'B' },
    { '7', '8', '9', 'C' },
    { '*', '0', '#', 'D' },
};

/* 行輸入讀值：0 = 被拉低（按下） */
static uint32_t col_read(uint32_t c)
{
    switch (c) {
    case 0:  return (PC->PIN >> 4)  & 1u;
    case 1:  return (PC->PIN >> 5)  & 1u;
    case 2:  return (PD->PIN >> 15) & 1u;
    default: return (PD->PIN >> 14) & 1u;
    }
}

/* 單次掃描：回傳第一個偵測到的鍵，沒有回 0。掃完把四列都拉回高。 */
static char kp_raw(void)
{
    char found = 0;

    for (uint32_t r = 0; r < 4 && !found; r++) {
        PC->DOUT = (PC->DOUT | 0x0Fu) & ~(1u << r);   /* 只有這一列低 */
        delay_us(300);                                /* 等線路穩定（弱上拉，要給夠時間） */
        for (uint32_t c = 0; c < 4; c++)
            if (col_read(c) == 0) {
                found = KEYMAP[r][c];
                break;
            }
    }
    PC->DOUT |= 0x0Fu;                                /* 掃描結束四列回高 */
    return found;
}

/* 去彈跳：要連續兩次（相隔 5 ms）讀到同一個鍵才算數 */
static char kp_scan(void)
{
    char a = kp_raw();
    if (!a)
        return 0;
    delay_ms(5);
    return (kp_raw() == a) ? a : 0;
}

/* ---- pin setup ---- */
static void pins_init(void)
{
    SYS_UnlockReg();
    CLK_EnableModuleClock(GPIO_MODULE);
    SYS_LockReg();

    /* LCD 資料 PD.0-7、控制 PC.14/15、PB.15：推挽輸出 */
    GPIO_SetMode(PD, 0x00FFu, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PC, 0xC000u, GPIO_PMD_OUTPUT);
    GPIO_SetMode(PB, 0x8000u, GPIO_PMD_OUTPUT);
    PC->DOUT &= ~(1u << 14);            /* RW 固定拉低（只寫） */
    LCD_E_L();

    /* keypad 列 PC.0-3：推挽輸出 */
    GPIO_SetMode(PC, 0x000Fu, GPIO_PMD_OUTPUT);
    PC->DOUT |= 0x0Fu;

    /* keypad 行 PC.4/5 與 PD.14/15：輸入 + 內建上拉 */
    GPIO_SetMode(PC, 0x0030u, GPIO_PMD_INPUT);
    GPIO_SetMode(PD, 0xC000u, GPIO_PMD_INPUT);
    PC->PUEN |= 0x0030u;
    PD->PUEN |= 0xC000u;
}

static void banner(void)
{
    lcd_clear();
    lcd_goto(0, 0);
    lcd_puts("KEYPAD DEMO");
    lcd_goto(1, 0);
}

int main(void)
{
    pins_init();
    lcd_init();
    banner();

    uint32_t n = 0;
    char held = 0;                       /* 目前壓著的鍵；0 = 沒有 */

    for (;;) {
        char k = kp_scan();

        if (k && !held) {                /* 只在「先前沒壓任何鍵」時才收 —— */
            held = k;                    /* 放鍵過程若冒出鬼鍵，held 還沒歸 0，會被擋掉 */

            if (k == '#') {
                banner();                /* 清零／循環 */
                n = 0;
            } else {
                if (n >= 16) {           /* 第二行滿了，先自動清一次 */
                    banner();
                    n = 0;
                }
                lcd_data((uint8_t)k);
                n++;
            }
        } else if (!k) {
            held = 0;                    /* 完全放開才解鎖，下一鍵才收得到 */
        }

        delay_ms(10);
    }
}
