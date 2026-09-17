/*
 * stm32/swd_probe —— Stage 1+2（STM32 版）：master 讀 target 的身分與記憶體
 *
 * master = NUCLEO-F446RE，燒這支。target = 另一片 Cortex-M（家裡是 STM32L476G-DISCO）。
 * 接線見 common/swd.h 檔頭。輸出走 USART2 -> 板載 ST-LINK 的虛擬 COM port，
 * PC 端開終端機（115200 8N1）就看得到。
 *
 * 為什麼要讀 DBGMCU_IDCODE：
 *   DPIDR 和 CPUID 在兩顆 M4 上長得一模一樣（都是 ARM SW-DP DPv1 + Cortex-M4 r0p1），
 *   所以那兩個值「對」並不能證明我連到的是哪顆晶片 —— 接錯線讀到自己也會是那個數字。
 *   DBGMCU_IDCODE 的低 12 bit 是 ST 的 device ID，這才是唯一能區分 F446 / L476 的證據。
 */
#include "stm32f446.h"
#include "delay.h"
#include "uart.h"
#include "swd.h"

/* DBGMCU_IDCODE：F4 與 L4 都在這個位址（Cortex-M 的外部 PPB 區） */
#define DBGMCU_IDCODE   0xE0042000u

/* Flash size register：每個系列位置不同，所以要先知道 dev_id 才能讀。
 * 單位是 KB，16-bit。swd_mem_read 是 32-bit 對齊的，F4 那個位址不對齊，
 * 所以讀下面那個 word 再取高半字。 */
#define FLASHSZ_F4_WORD 0x1FFF7A20u   /* 真正的暫存器在 0x1FFF7A22 -> 取 bits[31:16] */
#define FLASHSZ_L4_WORD 0x1FFF75E0u   /* 已對齊 -> 取 bits[15:0] */

static void put_dec(uint32_t v)
{
    char buf[10];
    int n = 0;
    if (v == 0) { uart_putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) uart_putc(buf[n]);
}

static void die(const char *msg)
{
    uart_puts(msg);
    for (;;)
        ;
}

int main(void)
{
    dwt_init();
    uart_init();
    swd_pins_init();

    uart_puts("\r\n\r\n=== STM32 SWD probe ===\r\n");

    uint32_t id = 0;
    int ack = -9;
    for (int t = 0; t < 4; t++) {
        ack = swd_read_idcode(&id);
        if (ack == 1)
            break;
        delay_ms(50);
    }

    if (ack != 1) {
        uart_puts("no ACK (");
        uart_putc((char)('0' + (ack & 7)));
        die(") — 檢查接線 / 共地 / target 的 ST-LINK 隔離跳線\r\n");
    }

    uart_puts("DPIDR  = ");
    uart_puthex(id);
    uart_puts("\r\n");

    if (swd_connect() != 0)
        die("connect fail — DP 上電或 AP 選擇失敗\r\n");

    uint32_t v;

    if (swd_mem_read(0xE000ED00u, &v) == 1) {   /* SCB CPUID */
        uart_puts("CPUID  = ");
        uart_puthex(v);
        uart_puts("\r\n");
    }

    /* 這一段才是「我到底連到誰」的判據 */
    uint32_t devid = 0;
    if (swd_mem_read(DBGMCU_IDCODE, &v) == 1) {
        devid = v & 0xFFFu;
        uart_puts("IDCODE = ");
        uart_puthex(v);
        uart_puts("  dev_id=");
        uart_puthex(devid);
        uart_puts("  -> ");
        switch (devid) {
        case 0x421u: uart_puts("STM32F446\r\n");            break;
        case 0x415u: uart_puts("STM32L475/L476/L486\r\n");  break;
        case 0x435u: uart_puts("STM32L43x/L44x\r\n");       break;
        case 0x431u: uart_puts("STM32F411\r\n");            break;
        case 0x413u: uart_puts("STM32F405/407/415/417\r\n");break;
        default:     uart_puts("不認得 —— 查該系列 RM 的 DBGMCU_IDCODE 表\r\n"); break;
        }
    } else {
        uart_puts("IDCODE 讀不到\r\n");
    }

    /* Flash 容量：位址依系列而異，所以擺在 dev_id 之後 */
    if (devid == 0x421u || devid == 0x431u || devid == 0x413u) {
        if (swd_mem_read(FLASHSZ_F4_WORD, &v) == 1) {
            uart_puts("FLASH  = ");
            put_dec((v >> 16) & 0xFFFFu);
            uart_puts(" KB\r\n");
        }
    } else if (devid == 0x415u || devid == 0x435u) {
        if (swd_mem_read(FLASHSZ_L4_WORD, &v) == 1) {
            uart_puts("FLASH  = ");
            put_dec(v & 0xFFFFu);
            uart_puts(" KB\r\n");
        }
    }

    /* 向量表第 0 項 = 初始 SP，通常等於該晶片主 RAM 的頂端，順便驗證 RAM 佈局。
     * 讀 0x08000000（flash 實體位址）而不是 0x00000000 —— 後者是 BOOT 腳位決定的
     * 別名區，target 的 BOOT 設定不同就會讀到別的東西。 */
    if (swd_mem_read(0x08000000u, &v) == 1) {
        uart_puts("SP[0]  = ");
        uart_puthex(v);
        uart_puts("  (target 初始 SP = RAM 頂端)\r\n");
    }

    uart_puts("--- done ---\r\n");
    for (;;)
        ;
}
