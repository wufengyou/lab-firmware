
這些是 `?=` 預設值，不是寫死：`make TOOLCHAIN="D:/gcc-arm/bin"` 就能換掉，
不必改檔。要讓別人重現這個實驗，這件事是必要的。
# nano130 —— NANO130KE3BN 韌體開發

Nuvoton NANO130KE3BN（Cortex-M0、42 MHz、123 KB APROM、16 KB SRAM）的
從原始碼建置 / 燒錄環境。器材與接線見 <https://lab.da-acadmy.com/nano130/repro/>。

## 為什麼有這個目錄

原本那批客製版（WENG NANO 2016）是搭一支不合規的 J-Link clone 在燒。
改用 NuTiny 板載的 **Nu-Link-Me** 經 `ICEJP8` 拉 3 線（3V3 / SWDAT / SWCLK，
無 nRESET，地另接）就能燒 —— 2026-09-10 驗證成立。

**寫韌體看腳位速查表：[`../iot_lab_research/nano130_firmware_resource_map.md`](../iot_lab_research/nano130_firmware_resource_map.md)**

## 互動教材

可直接開啟 [`web/index.html`](web/index.html) 查看 N2 互動教材：從 PC → UART0 →
master → SWD → target → LED 的證據鏈，包含四層心智模型、六個核心知識點、
實測接線、512-byte 封包、Stage a–e 驗證梯、阻塞／非阻塞時序對照、三板互燒
與上機檢查卡。這是純靜態教材，不需要安裝額外套件；內容以 2026-09-15 的上機
結果與現行 `kp_poll()` 實作為事實基線。

## 工具鏈（Makefile 的預設值，可從命令列覆蓋）

| 用途 | 路徑 |
|---|---|
| 編譯 | `C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\14.2 rel1\bin\` |
| 燒錄 | `C:\Program Files (x86)\Nuvoton Tools\NuLink Command Tool\NuLink.exe`（基礎那支，非 M2351/M460 分支） |
| BSP | `vendor/`（Nano100B_BSP 的精簡子集，已入版控，不依賴系統上的 Temp 解壓目錄） |

`vendor/` 只放編得起來需要的：CMSIS core（cm0）、Device（header + startup + linker
+ system）、StdDriver 全套 src/inc。來源是 Nuvoton Nano100B BSP。

這些是 Makefile 的 `?=` 預設值，不是寫死。換機器或別人要重現時從命令列覆蓋即可：

    make TOOLCHAIN="D:/gcc-arm/bin"
    make NULINK="D:/Nuvoton/NuLink.exe" flash

要讓這個實驗能被別人重做，這件事是必要的。

## 指令

```sh
make                          # 編譯 blink -> build/blink/blink.bin
make APP=keypad_lcd           # 編譯別的 app
make APP=keypad_lcd flash     # 編譯 + 抹除 + 燒 APROM + 驗證 + reset（晶片隨即開跑）
make APP=keypad_lcd clean
```

`APP` 預設 `blink`。每個 app 一個目錄（`blink/`、`keypad_lcd/`…），
build 產物在 `build/$(APP)/`，互不干擾。

- **Nu-Link 會把傳入路徑轉大寫再找檔，相對路徑會失敗** —— Makefile 一律給
  絕對路徑（`$(CURDIR)/...`）。手動下 `NuLink.exe -w APROM xxx` 也要注意這點。
- `NuLink.exe -p` / `-r UID` 只印螢幕，任何 cwd 都能跑。
- 讀 UID 建檔：各板的 UID 指紋要自己建表核對（讀 UID 的方式見 `mem_peek/`）。

## 已知的坑

- **客製版的 LCD 是 LCD1602 字元模組（HD44780），8-bit 並列走 GPIO，不是
  NANO130 的段式 LCD 控制器** —— BSP 那些 `LCD_*` 段式範例在這片板子上不會動。
  接腳 DB0-7=PD.0-7、RS=PC.15、RW=PC.14、E=PB.15，RW 韌體固定拉低只寫。
- **客製版的 4x4 keypad 是裸矩陣，板上沒有上拉電阻** —— 讀取那一側要開
  MCU 內建的 `PC->PUEN` / `PD->PUEN`。列=PC.0-3、行=PC.4/PC.5/PD.15/PD.14。
- **`delay_us` 的迴圈校正**：`while(n--) __NOP()` 在 HIRC 12 MHz + `-Os` 下，
  ×3 只有標稱的約 1/4。keypad 換列後 settle 不夠會讀到鄰腳（PC.3→PC.4）耦合
  的假訊號。現在用 ×10 + 300 µs settle + 相隔 5 ms 雙讀去彈跳。
- **keypad 邊緣判斷別用「這次≠上次就收」** —— 放鍵瞬間會冒鬼鍵。改成「按住」
  模型：只有先前沒壓任何鍵時才收新鍵，完全放開才解鎖。
- 客製版 `J4` 沒有 nRESET 腳。韌體開頭別做會鎖死 SWD 的事（把 SWD 腳改用途、
  進深睡、緊迴圈），否則沒有 connect-under-reset 可救。
- `make flash` 的 `-e APROM` 一定會先抹掉整個 APROM。中途失敗晶片會是空的，
  重跑 `make flash` 即可。

## 目前內容

- `blink/` —— 第一支韌體。閃 LED2（RGB）的 PA.13。用來驗證
  「改碼 → make → flash → 晶片開跑」整條鏈。2026-09-10 於 C1 板實測通過。
- `keypad_lcd/` —— 4x4 keypad 打字顯示到 LCD1602；`#` 清螢幕循環。
  2026-09-10 於 C1 板實測通過（含去彈跳、鬼鍵處理）。

- `flash_wr/` —— **N1：把客製版變成燒錄器**。master 用 keypad 驅動五階驗證梯
  （A/B/C = Stage 1-3 唯讀，`0` 解保險後 `1`/`2` = Stage 4/5 寫入）。
  2026-09-15 於 master C3(`0615`) → target C1(`062C`) 實測**五階全通**。
  暫存器地圖、安全規則與每一階在驗什麼寫在 `flash_wr/main.c` 的檔頭。

- `uart_echo/` —— N2 Stage a。UART0 最小收發，**RX 計數直接顯示在 LCD 上，
  不靠回傳**，所以兩個方向能分開判定。2026-09-15 用它確認 `TX=PB.1 / RX=PB.0`
  （資源表原本描錯一格），並確認板上**沒有** USB-UART 橋接晶片，要外接 USB-TTL。
- `flash_isp/` + `isp_send.ps1` —— **N2：完整的燒錄器**。payload 由 PC 走 UART0
  送進 master，master 經 SWD 抹除／寫入／逐 word 回讀驗證／重置 target。
  2026-09-15 把 `blink` 燒進 C2 並實測會閃，**全程無 Nuvoton 工具**。
  協定、錯誤碼、安全規則寫在 `flash_isp/main.c` 檔頭。

## 用 flash_isp 燒一支韌體（取代 `make flash`）

```sh
make APP=blink                                   # 產生 build/blink/blink.bin
# master 上按 keypad 的 0 切到 ARMED（reset 後會回到 dryrun）
powershell -File isp_send.ps1 -Port COM13 -File build/blink/blink.bin
powershell -File isp_send.ps1 -Port COM13 -Reset  # ⚠ 是 -Reset 不是 -Run
```

**`-Run` 只是「從停住的地方繼續」**，而那個位址屬於已經被換掉的舊程式；
新韌體要從向量表跑起來得重置（走 `AIRCR` 的 `SYSRESETREQ`，不需要 nRESET 腳）。

**燒之前先備份**（`*.bin` 已在 `.gitignore` 裡，不會進版控）：

```sh
powershell -File isp_send.ps1 -Port COM13 -Dump backup.bin           # 整個 APROM（123 KB，約 2-3 分）
powershell -File isp_send.ps1 -Port COM13 -Dump head.bin -Length 936 # 只讀前面一段
```

讀取刻意**不限位址**（`-Addr` 可給任意值）—— 讀不改變任何東西，安全規則管的是寫。
所以它也能當 peek 用：把 CONFIG（`0x00300000`）讀出來存檔、讀週邊暫存器除錯。

**換韌體時加 `-EraseAll`**：寫入只會抹它自己要寫的那幾頁，所以用小韌體覆蓋
大韌體會在後面留下上一手的程式碼（2026-09-15 由 dump 抓到，寫入那一側完全
看不出來）。殘骸可能含上一手的資料——這個 repo 就有過內含明文 WiFi 帳密的韌體。

```sh
powershell -File isp_send.ps1 -Port COM13 -EraseAll -File build/blink/blink.bin
```

## 互燒：兩塊板子互為燒錄器（不需要 Nu-Link）

2026-09-15 實測成立。`flash_isp` 燒進兩塊板子之後，任一塊都能把另一塊救回來，
**自舉不再依賴 Nu-Link**。要反向時：

1. **先把 Nu-Link 的線從 target 的 `J4` 整組拔掉** —— 那正是 Nu-Link-Me 接的孔，
   不拔的話兩個 master 會在同一條 SWD 匯流排上對打。
2. target 原本若是靠 Nu-Link 供電，改用自己的 USB。
3. SWD 三條線反向：新 master 的 `J17` pin 3/4/8 → 新 target 的 `J4` pin 3/2/4。
4. USB-TTL 移到新 master 的 `PB.0/PB.1/GND`。
5. 按新 master 的 `*`，看到 `DP:0x0BB11477` 才往下做。

**動手抹除之前先確認身分**：三塊外觀完全一樣，插錯不會有任何提示。
Nu-Link 不在線上時用 `-Dump` 讀前 1 KB 跟已知的 `.bin` 比對即可。

keypad 現已用非阻塞 `kp_poll()` 每次只取樣一列，**不必再按住約 1 秒**。
按鍵狀態每輪都掃，但重連 SWD／重畫 LCD 等慢動作只在 UART 靜置約 50 ms 後執行；
這是把「便宜的偵測」和「昂貴的動作」分開（見 `flash_isp/main.c` 主迴圈註解）。
