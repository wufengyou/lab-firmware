# stm32 —— NUCLEO 板對板的自製 SWD debugger（骨架）

`nano130/` 那套（客製版當 debugger 讀另一塊）搬到 STM32 M4。**SWD/DAP 協定碼
一字不改**（在 `common/swd.h`），只換了腳位存取層與工具鏈。

## 硬體：不是兩片一樣的板子

實際使用的是 **NUCLEO-F446RE ×1 + STM32L476G-DISCO ×1**。器材與角色分配另見
<https://lab.da-acadmy.com/stm32/repro/>。
本檔案先前寫「NUCLEO-F446RE ×2」是錯的，已更正。角色分配：

| 角色 | 板子 | 為什麼 |
|---|---|---|
| **master**（燒這份韌體） | NUCLEO-F446RE | `core/` 的 linker、`stm32f446.h`、USART2->ST-LINK VCP 的接法都已經是為它寫的；Arduino 排針上有現成的空閒腳位 |
| **target**（被讀的那片） | STM32L476G-DISCO | target 只需要 PA13 / PA14 / GND 三條線，而這三支腳在兩顆晶片上位置相同 |

L476G-DISCO 當 master 會麻煩很多：要另寫 `core/stm32l476.ld`（flash 1 MB、
SRAM1 96K @ `0x20000000` 與 SRAM2 32K @ `0x10000000` 是分開的兩塊）與
`stm32l476.h`，而且它的擴充座腳位大多被 LCD / MEMS / audio 佔掉。所以
**把 L476 放在 target 側，現有的 C 程式一行都不用改。**

## 為什麼是純學習

這兩片本來就各有能用的 ST-LINK，board-to-board 純粹是把 `nano130/` 的
Stage 1-3 在 M4 上重跑一遍。

不過換成異質的兩顆之後多了一件真事：**你的 debugger 面對的是一顆記憶體佈局、
周邊、ID 都跟自己不同的晶片**，這正是真實 debugger 要處理的問題（見下面
「為什麼要讀 DBGMCU_IDCODE」）。M4 當 target 的額外收穫：DWT CYCCNT、
真正的 data watchpoint、（加 SWO 擷取的話）ITM trace —— M0 全都沒有。

## 回家要準備

| | |
|---|---|
| 硬體 | F446RE + L476G-DISCO + 兩條 USB 線（**注意接頭不同**：NUCLEO-64 是 mini-B，DISCO 是 mini-B/micro，出門前確認）+ **母-母杜邦線 ×3~4** |
| 軟體 | `arm-none-eabi-gcc` + `make` + 序列終端機（PuTTY / tio，115200 8N1） |
| 燒錄 | **不用額外工具** —— NUCLEO 板載 ST-LINK 會掛成 USB 磁碟，把 `.bin` 丟進去即燒 |

## 接線

| master（F446RE，Arduino 排針） | | target（L476G-DISCO）的 SWD |
|---|---|---|
| PA9  (D8)  SWCLK | ──────► | PA14 (SWCLK) = **CN3 pin 2** |
| PA8  (D7)  SWDIO | ◄─────► | PA13 (SWDIO) = **CN3 pin 4** |
| GND | ─────── | GND（共地必接） |

CN3 的腳號是 2026-09-11 量出來的（見下面「四支腳的指紋」）。**pin 2 和 pin 4
不相鄰**，中間隔著 pin 3。

- **master 自己的 PA13/PA14 不要動到** —— 那是 master 被板載 ST-LINK 燒錄的線。
- **master（NUCLEO）的 CN2 兩個跳線帽要保持 ON，不要拔。** 兩片的處置是相反的：

  | 板子 | 角色 | 跳線帽 |
  |---|---|---|
  | NUCLEO-F446RE | master | **CN2 保持 ON** —— 它的 ST-LINK 要負責燒錄 master、還要提供看輸出用的虛擬 COM port |
  | L476G-DISCO | target | **CN3 拔掉** —— 它的 ST-LINK 要讓出 SWD 匯流排 |

  一句話：**誰要被我們的 master 讀，誰就拔；master 自己不拔。**
  拔掉 NUCLEO 的 CN2 之後 NUCLEO 磁碟還是會出現（ST-LINK 本身還活著），但 `.bin`
  燒不到 F446 上、序列埠也是死的 —— 症狀是「拖進去好像成功了，但終端機一片空白」，
  很容易誤判成程式寫壞了。
- **target 要放開 SWD 匯流排，否則兩個 master 搶同一條線，`swd_probe` 會印 `no ACK`。**
  NUCLEO-64 是拔 CN2；**L476G-DISCO（MB1184）是拔 CN3 的兩個跳線帽** —— 見下一節。
- L476 的 PA13 / PA14 不必去 P1 / P2 排針上找，**CN3 的 MCU 側（pin 2 / pin 4）就是**。
- **`swd_probe` 和 `cpu_ctrl` 共用這一組線，不用改接。** halt / step / run 全部走
  SWD 的 MEM-AP 寫 target 的除錯暫存器，沒有用到 nRESET —— 所以從頭到尾就是三條線。

## L476G-DISCO 端：拔掉 CN3 的兩個跳線帽

依據 **UM1879**（*Discovery kit with STM32L476VG MCU*，DocID027676）§7.1.3 / §7.1.4、
Table 2（Jumper states）、Figure 6 / Figure 7：

| CN3 兩個跳線帽 | 效果 |
|---|---|
| **ON**（出廠預設） | 板載 ST-LINK/V2-1 接到板上的 L476 —— 這時匯流排是它的，我們搶不到 |
| **OFF** | ST-LINK 改接到 CN4 外部除錯座，**板上的 L476 之 SWD 被放開** |

所以要做的就是**把 CN3 那兩個跳線帽拔掉**。等價於 NUCLEO-64 拔 CN2，只是編號不同。

### 接哪兩支腳：就接 CN3 的 MCU 側

MB1184 電路圖上 CN3 是 4 pin：兩支走 ST-LINK 側（`T_JTCK` / `T_JTMS`），
兩支走 MCU 側（`STM_JTCK` / `STM_JTMS`，也就是 L476 的 PA14 / PA13）。
**跳線帽拔掉之後，MCU 側那兩支就是現成的 SWCLK / SWDIO 測試點**，不必去
P1 / P2 排針上翻 PA13 / PA14。

> ✅ **已定案（2026-09-11 實測）：CN3 的 pin 2 = MCU 側 SWCLK、pin 4 = MCU 側 SWDIO。**
> 直接看下面「四支腳的指紋」那張表，不用再量。以下兩節保留的是**方法**
> —— 換一片板子、或哪天讀數對不上時要重走一次的流程。

### 怎麼確定 CN3 哪兩支是 MCU 側（用電表，不必碰晶片接腳）

CN3 是 4 pin，兩支通 ST-LINK、兩支通 MCU。跳線帽是橫跨中間把兩側接起來，
所以配對是 **(1,2)** 和 **(3,4)**。要分辨哪一側是哪一側，不必去 LQFP100 上
找 PA13/PA14 —— **借 CN4 來當已知端**：

CN4 是板載 ST-LINK 的對外輸出，它的 pin 2 = SWCLK、pin 4 = SWDIO，
**接的正是 CN3 的 ST-LINK 側**。所以：

1. 跳線帽拔掉、板子斷電，電表切「通斷」檔
2. 一支表棒點 **CN4 pin 2（SWCLK）**，另一支依序點 CN3 的四支腳
   → 嗶的那一支 = **CN3 的 ST-LINK 側 SWCLK**
3. 同一支腳的**配對腳**（1↔2 或 3↔4）就是 **MCU 側的 SWCLK**，接 master 的 PA9
4. 用 **CN4 pin 4（SWDIO）**重複一次 → 找出 **MCU 側的 SWDIO**，接 master 的 PA8

> **懶人法也可以**：直接接，如果 `no ACK` 就把兩條訊號線對調再試一次。
> SWCLK / SWDIO 接反只是讀不到，不會燒東西（兩邊都是 3.3V CMOS、有串聯電阻）。
> 但量過的話心裡有底，而且結果可以寫進文件，下次不用再猜。

> 這個通斷法沒有實際用到 —— 下面那個「量指紋」的方法更快（不必找 CN4 的腳位、
> 不必斷電），而且一次就把四支腳全部標定完。留著是因為它不需要 target 通電，
> 板子拿不到電的時候還是得靠它。

### ⚠ 接 CN3 也可能接錯側（2026-09-11 實際踩到）

線接在 CN3 上是對的，但 **CN3 有兩側**，接到 ST-LINK 側一樣不會通。這次就是
這樣：兩條線都接在 ST-LINK 側，`ack` 一直是 7。

**判斷方法（用 CN4 當已知端，不必碰晶片接腳）：**

跳線帽拔掉之後，**只有 CN3 的 ST-LINK 側會通到 CN4**（ST-LINK 被改接過去了），
MCU 側不通。所以：

| 你接的那支 CN3 腳，對 CN4 量通斷 | 結論 |
|---|---|
| **嗶（通）** | 接在 **ST-LINK 側** —— 錯的，往配對腳挪一格 |
| **不嗶（不通）** | 接在 **MCU 側** —— 對的 |

CN3 是 4 pin、跳線帽橫跨配對，所以**每支 ST-LINK 側腳的配對腳就是對應的
MCU 側同名訊號**。不確定哪兩支是一組的話，把跳線帽裝回去看它蓋住哪兩支。

**電壓怎麼判讀（⚠ 有個反直覺的地方）：**

`swd.h` 每次傳輸失敗收尾都會 `swd__drive(); swd__dio_lo();` —— 所以 **master
停在 `no ACK` 之後，PA8 一直維持「輸出、低電位」**。因此：

> **只要線是通的，不管接對接錯，SWDIO 那條線都該量到 ~0V。**
> master 的輸出會壓過 target 的內部上拉。

（本文件先前寫「接對的話 SWDIO 會是 3.3V」，**那是錯的**，會讓人把好的接法
誤判成壞的。留著這行標記，不要再推導一次。）

實測到的三種狀態：

| SWDIO 對 GND | 意思 |
|---|---|
| **~0V** | 線是通的。**分不出接對接錯** —— 要看 `ack` 值 |
| **~1.7V**（半個電源電壓） | **浮接：那個點跟 master 的 D7 是斷的**。線壞了或沒插到底 |
| ~3.3V | master 沒在驅動（沒燒對韌體？沒 RESET？） |

SWCLK 是單向的（master 驅動、target 只聽），接哪一側都是 3.3V ——
**光看 SWCLK 什麼都判斷不出來。**

至於接到 ST-LINK 側會怎樣：拔掉 CN3 跳線帽**並不會讓 ST-LINK 消失**，只是把它
改接到 CN4，所以接 ST-LINK 側等於跟另一個 debugger 的輸出互頂。
（3.3V CMOS 對頂、兩邊有串聯電阻，短時間不會壞，但發現了就改掉。）

**最可靠的辨識法：把線全部拔掉，量 CN3 四支腳的「指紋」**

DISCO 通電、跳線帽拔掉、訊號線全部拔掉，對 DISCO 的 GND 量四支腳：

| 讀到 | 那支是 |
|---|---|
| ~3.3V | **MCU 側 SWDIO**（L476 的 PA13，內部**上拉**） |
| ~0V | **MCU 側 SWCLK**（L476 的 PA14，內部**下拉**） |

STM32 的 SWD 腳位上電後的預設提升／下拉就是指紋。**✅ 2026-09-11 量出來的結果
（這張表就是結論，不用再量）：**

| CN3 腳 | 1 | 2 | 3 | 4 |
|---|---|---|---|---|
| 電壓 | ~1V | **0V** | ~1V | **3V** |
| 是誰 | ST-LINK 側 SWCLK | **MCU 側 SWCLK**（PA14） | ST-LINK 側 SWDIO | **MCU 側 SWDIO**（PA13） |

所以接線是 **CN3 pin 2 → master D8（PA9）**、**CN3 pin 4 → master D7（PA8）**。
**這兩支不相鄰**（中間隔著 pin 3），插的時候容易差一格。

判讀要點：

- **只有 0V 和 3V 是真訊號。** 那是 L476 活著的腳位給出的上拉／下拉，電位明確。
- **兩支 ~1V 是浮接**，不是「1V 這個電位」—— 高阻抗腳被電表輸入阻抗與漏電流
  拉到電源一半上下。ST-LINK 側被跳線帽拔掉後就懸空了，本來就該是這樣。
  **看到半個電源電壓要想到浮接，不要試圖解釋那個數字。**
- 3V 而不是 3.3V 也是電表分壓造成的，不影響判讀。
- **自我檢查**：MCU 側落在 2、4，ST-LINK 側落在 1、3 —— 跨配對交錯，符合
  「每組 (1,2)、(3,4) 裡各一支」。如果 pin 1 數反方向，整張表會鏡射成 MCU 側
  在 1、3，但因為認的是 0V / 3V 這兩個特徵值本身，**數錯方向不影響結論**。

（已知：CN3 是**一排 4 支**，跳線帽蓋 **1-2** 和 **3-4**，所以配對是 (1,2) 和 (3,4)，
每組裡一支 ST-LINK 側、一支 MCU 側。）

### ⚠ 不要把 master 接到 CN4

CN4 是板載 ST-LINK 的**對外輸出**（原廠用途是拿這片去燒別人的板子），方向跟
我們要的相反。它的正確用途只有兩個：原廠那個用途，以及**當量測的已知參考端**
（絲印可信、UM1879 Table 3 有腳位表）。上面那個判斷方法就是在用它。

### 供電：CN3 拔掉之後 L476 還是有電（✅ 2026-09-11 實測確認）

UM1879 §7.4（Power supply）列出 ST-LINK 供電的條件是 **JP6 在 `3V3`、JP3 閉合、
JP5 在 `ON`** —— 電源路徑走的是 JP5 / JP6，CN3 只掛 `T_JTCK` / `T_JTMS` 兩條訊號線。

**所以我們要的組態是：CN3 兩個跳線帽拔掉，但 JP6 = `3V3`、JP5 = `ON` 保持不動**，
L476 照樣從自己的 CN1（Mini-B USB）吃電。

> 注意 §7.1.4 叫你設 **JP5.OFF** —— 那是「拿這片的 ST-LINK 去燒別人的板子」的情境，
> 目的正是讓板上的 L476 不要跑。**我們的情境相反，JP5 要留在 ON。**
>
> **實測（2026-09-11）：CN3 兩個跳線帽拔掉後，L476 仍然有電。** 上面那段原本是從
> 手冊電源章節與電路圖網路名稱推出來的推論，現已由實機證實。

## 指令（這台機器上實測過的寫法）

`make` 和 `arm-none-eabi-gcc` **都不在 PATH 上**，而且有兩個坑：

- MSYS2 的 `make` 在 **Git Bash 底下跑會炸**（`cygheap base mismatch` —— 兩套
  cygwin DLL 打架）。**要在 PowerShell 或 cmd 裡跑。**
- 光把 `make.exe` 叫起來還不夠：Makefile 用到 `mkdir -p` / `cp`，
  **`C:\msys64\usr\bin` 必須進 PATH**，否則會停在
  `make: mkdir: No such file or directory`。

所以在 **PowerShell** 裡（以下是實際跑通、產出兩支 `.bin` 的指令）：

```powershell
cd <你 clone 的位置>/stm32
$env:PATH = "C:\msys64\usr\bin;" + $env:PATH
& C:\msys64\usr\bin\make.exe                  # -> build/swd_probe/swd_probe.bin  (2340 B)
& C:\msys64\usr\bin\make.exe APP=cpu_ctrl     # -> build/cpu_ctrl/cpu_ctrl.bin    (3392 B)
```

`arm-none-eabi-gcc` 在
`C:\Program Files (x86)\Arm GNU Toolchain arm-none-eabi\12.2 mpacbti-rel1\bin`，
這台機器上已經在 PATH 裡；換機器加 `TOOLCHAIN="C:/.../bin/arm-none-eabi-"`。

> **⚠ 另一台機器（2026-09-14 遇到）：`C:\msys64` 整個不存在。**
> 上面那段路徑是寫這份文件那台機器的事實，不是通用的。這台改用
> `%LOCALAPPDATA%\Microsoft\WinGet\Links\make.exe`（winget 裝的
> GNU Make）+ `Arm GNU Toolchain 14.2 rel1`。這支 make 底層用 `cmd.exe` 執行recipe，
> 不是 `sh`，所以 Makefile 裡的 `mkdir -p $(B)` 在 `cmd` 下會報「命令語法不正確」——
> `cmd` 的 `mkdir` 不吃 `-p`。**這台機器上第一次編某個新 `APP` 之前要手動
> `New-Item -ItemType Directory -Force -Path build\<APP>`**，之後 make 發現目錄已存在
> 就不會再跑那條 recipe。換機器先確認 `make.exe` 在哪、底層 shell 是什麼，
> 不要預設走 CLAUDE.md 那條 `C:\msys64\usr\bin` 路徑。

燒錄不必用 `make flash` —— **直接把 `.bin` 拖進檔案總管裡那顆 NUCLEO 磁碟**，
ST-LINK 會自動燒，燈閃完就好。比記 `NODE=` 參數可靠。

## 這台機器上的 COM 埠與燒錄磁碟

兩片都是 ST-LINK/V2-1（`VID_0483&PID_374B`），**光看 COM 編號分不出誰是誰**。
用 USB 序號把 VCP 和燒錄磁碟對起來之後（`serial.ps1 -List` 會自動做這件事）：

| 板子 | 角色 | COM 埠 | 燒錄磁碟 | ST-LINK 序號 |
|---|---|---|---|---|
| **NUCLEO-F446RE** | **master** | **COM7** | **E:** `NODE_F446RE` | `066…`（你的會不同） |
| STM32L476G-DISCO | target | COM6 | D: `DIS_L476VG` | `067…`（你的會不同） |

**`.bin` 拖到 E:，終端機開 COM7。** target 不燒任何東西。

> COM 編號會變（換 USB 孔就會變），**磁碟標籤不會**。所以每次先跑
> `.\serial.ps1 -List` 確認，不要背 COM7。

## 終端機：`serial.ps1`

`arduino-cli monitor` 這個 repo 不用（互動式），PuTTY 要另外裝，而先前那段
裡那段給 Mega 用的 PowerShell 是**唯讀**的 —— `cpu_ctrl` 需要一邊看輸出一邊按鍵
下指令，所以自己寫了一支。

```powershell
cd <你 clone 的位置>/stm32
.\serial.ps1 -List          # 只列出接著哪些板子，不開埠
.\serial.ps1                # 開 COM7（master）
.\serial.ps1 -Port COM6     # 開別的埠
```

打進去的字元直接送給 master，這就是 `cpu_ctrl` 的 `h` / `r` / `s` / `d` / `m` / `?` / `c`。

- **離開按 `Ctrl+]`，不要按 `Ctrl+C`。** Ctrl+C 會讓埠來不及關，下次開會說「存取被拒」。
- **開埠之後要按板子上的黑色 RESET 鈕。** NUCLEO 的 USART2 是普通 UART，
  DTR 不會重置 F446（**跟 AVR Mega 不一樣** —— 那邊開埠就會觸發重置，接了馬達的話車會自己跑）。
  好處是不會有意外動作，代價是開機那行標題在你開埠前早就印完了，要按 RESET 重跑。

### 存檔編碼：UTF-8 **with BOM**（踩過）

Windows PowerShell 5.1 讀 `.ps1` 預設當 ANSI（這台是 CP950）。存成無 BOM 的
UTF-8 會讓中文註解變亂碼，**而亂碼裡的位元組會把引號和大括號拆掉**，錯誤訊息是
一長串莫名其妙的 `Missing closing '}'` —— 看起來像語法寫錯，其實是編碼。
改這支檔案之後如果它突然不能跑，先確認 BOM 還在。

## 看結果

master 的 USART2（PA2/PA3）已橋到板載 ST-LINK 的虛擬 COM port。PC 開終端機
（115200 8N1）。**✅ 2026-09-11 實測逐字輸出（以下不是示意，是真的跑出來的）：**

```
=== STM32 SWD probe ===
DPIDR  = 0x2BA01477
CPUID  = 0x410FC241
IDCODE = 0x10076415  dev_id=0x00000415  -> STM32L475/L476/L486
FLASH  = 1024 KB
SP[0]  = 0x20018000  (target 初始 SP = RAM 頂端)
```

對照（每一行為什麼是這個值，見下面兩節）：

| 行 | 值 | 意義 |
|---|---|---|
| `DPIDR` | `0x2BA01477` | ARM SW-DP, DPv1 —— 兩顆晶片相同，**證明不了接對** |
| `CPUID` | `0x410FC241` | Cortex-M4 r0p1 —— 同上 |
| `dev_id` | `0x415` | **STM32L476 —— 這才是讀到 target 的證據** |
| `FLASH` | 1024 KB | L476VG 是 1 MB，對 |
| `SP[0]` | `0x20018000` | SRAM1 96K 的頂端（`0x20000000 + 0x18000`）。**F446 會是 `0x20020000`** |

### 為什麼要讀 DBGMCU_IDCODE（`0xE0042000`）

**前兩行證明不了任何事。** F446 和 L476 都是 Cortex-M4 r0p1、都掛 ARM SW-DP DPv1，
所以 `DPIDR` 和 `CPUID` 在兩顆上完全相同 —— 接線接錯、甚至意外讀到 master 自己，
也會印出一模一樣的數字。

`DBGMCU_IDCODE` 低 12 bit 是 ST 的 device ID，這才是唯一能區分的證據：

| dev_id | 晶片 |
|---|---|
| `0x421` | STM32F446 |
| `0x415` | STM32L475 / L476 / L486 |
| `0x435` | STM32L43x / L44x |

`FLASH` 那行同理，但它的暫存器位址**每個系列不一樣**（F4 在 `0x1FFF7A22`、
L4 在 `0x1FFF75E0`），所以程式裡是先讀出 dev_id、再決定去讀哪個位址 —— 這是
debugger 必然會長出來的第一段「晶片資料庫」。

`SP[0]` 讀的是 `0x08000000`（flash 實體位址）而不是 `0x00000000`：後者是 BOOT
腳位決定的別名區，target 的 BOOT 設定不同就會讀到別的東西。這個值等於 target
主 RAM 的頂端，順便驗證 RAM 佈局 —— F446 是 `0x20020000`（128K），
L476 的 SRAM1 只有 96K，會落在 `0x20018000` 附近。

## Stage 3：`cpu_ctrl`

**接線和 `swd_probe` 一模一樣，一條都不用加。** nano130 那版是 keypad + LCD1602 當
介面，F446RE 上沒有那些週邊，所以改用 USART2 的虛擬 COM port —— 同一個終端機視窗
既看輸出也打指令。反而比 16x2 的 LCD 好用：暫存器可以一次全部列出來，不必按鍵一個
一個翻。

指令直接按鍵，**不用按 Enter**：

| 鍵 | 動作 |
|---|---|
| `h` | halt —— 停住 target 的 CPU |
| `r` | run —— 放它繼續跑 |
| `s` | step —— 單步一條指令（`C_MASKINTS=1`，會遮中斷，不會踩進 ISR） |
| `d` | dump —— 列出 R0-R12 / SP / LR / PC / xPSR |
| `m` | memory —— 讀一個位址（接著打 8 位 hex，Enter 送出，Esc 取消） |
| `?` | 重讀 DHCSR + PC |
| `c` | 重新 connect（線鬆掉、target 斷電後用這個） |

```
=== STM32 cpu_ctrl（Stage 3）===

  h halt   r run    s step   d dump regs
  m mem    ? status c connect

  DPIDR = 0x2BA01477
  RUN     DHCSR=0x00030000  PC=--------
> h
  HALT    DHCSR=0x00030003  PC=0x0800063C
> s
  HALT    DHCSR=0x00030003  PC=0x0800063E
> d
  R0  = 0x00000000    R1  = 0x20017F00
  ...
  PC  = 0x0800063E    xPSR = 0x61000000
> r
  RUN     DHCSR=0x00030001  PC=--------
```

### 三個踩過的坑（都寫進程式註解了）

- **DHCSR 讀寫不對稱。** 讀回來的高半字是 `S_*` 狀態位元，寫進去的高半字必須是
  `DBGKEY`（`0xA05F`）。把讀到的值原封不動寫回去，整包會被硬體丟掉 ——
  症狀是「寫了沒反應，也不報錯」，很難查。
- **`step` 之前一定要先 `halt`。** 對著跑著的 CPU 寫 `C_STEP`，ARMv7-M B1.5.15 說
  行為是 unpredictable。程式裡先讀 `S_HALT` 擋掉了。
- **`C_MASKINTS` 不能跟 `C_HALT` 的變化寫在同一次** —— 見下一節，這個是上機才抓到的。

### ⚠ `C_MASKINTS` 必須單獨寫一次（2026-09-11 上機才抓到）

ARMv7-M B1.5.15 有一條容易漏掉的規定：

> **對 `C_MASKINTS` 的寫入會被忽略，除非該次寫入同時維持 `C_HALT = 1`。**

而 `step`（要 `C_HALT=0` 跑一條）和 `run`（要 `C_HALT=0` 一直跑）**都必須清掉
`C_HALT`**，所以這兩個動作都不可能順便設定 `C_MASKINTS`。原本 `swd.h` 兩個都是
一次寫完，於是：

| 原本的寫法 | 意圖 | 實際結果 |
|---|---|---|
| `swd_step()` 寫 `0xD` | `C_STEP` + 遮中斷 | **`C_MASKINTS` 沒設進去** —— 單步時中斷沒被遮，會踩進 ISR |
| `swd_run()` 寫 `0x1` | 跑，順便解除遮蔽 | **`C_MASKINTS` 也清不掉** —— step 過一次之後就永遠卡在 1 |

第二個比第一個嚴重：**target 是在中斷被遮的狀態下跑的**，SysTick 不來、週邊中斷
不進 ISR，看起來像 target 當掉，但 `DHCSR` 顯示 `RUN`、一切「正常」。

修法是兩個都拆成兩次寫，第一次趁 `C_HALT` 還是 1 把 `C_MASKINTS` 設成想要的值：

```c
swd_step():  DBGKEY|0xB  (DEBUGEN|HALT|MASKINTS)  ->  DBGKEY|0xD  (DEBUGEN|STEP|MASKINTS)
swd_run():   DBGKEY|0x3  (DEBUGEN|HALT)           ->  DBGKEY|0x1  (DEBUGEN)
```

**怎麼自己驗證（看 `DHCSR` 的 bit 3）：**

| 狀態 | 修好之後 | 沒修的話 |
|---|---|---|
| `s` 之後 | `0x0103000F` | `0x01030007` |
| `r` 之後 | `0x01010001` | `0x01010009`（step 過才會出現） |

**最有說服力的證據不是 bit 3，是 `PC` 的分佈**：修好之後每次 `h` 落在完全不同的
位址（實測 `0x08000246` / `0x0800539C` / `0x080026F4`）、`SP` 也跟著變（呼叫深度不同）
—— target 真的在整個程式裡跑。中斷被遮的時候，`h` 每次都落在同一小段裡打轉。

> 這一整條是**上機才可能發現的**：程式編得過、`step` 看起來也正常（`PC` 有前進），
> 只有把讀回來的 `DHCSR` 逐位元對照才看得出來。這就是「編得過」和「對」的差別。

`PC` 只在 halt 時顯示，跑著的時候印 `--------`：CPU 還在跑的時候讀核心暫存器，
讀到的是某個瞬間的值，下一個 cycle 就變了，印出來只會誤導。

## Stage 3.5：`flash_wr` —— 燒 target 的 flash

**✅ 2026-09-11 實測通過。接線跟前面完全相同，一條都不用加。**

到 Stage 3 為止 debugger 只會讀與控制 CPU。這一支跨過那道門檻：解鎖 target 的
flash 控制器、寫入、抹除、讀回驗證 —— 這正是 ST-LINK 燒錄時在做的事。

### 三道保險（每一道都是刻意的）

前面三個 Stage 全是唯讀或只動除錯暫存器（斷電就恢復），這一支會**不可逆地改變
target 的 flash**。所以：

| 保險 | 為什麼 |
|---|---|
| 只碰 **bank2 最後一頁**（`0x080FF800`） | 原廠 demo 在 bank1 前段。實測那頁本來就是全 `FF`，沒被用到 |
| **絕不寫 `FLASH_OPTKEYR`（`0x4002200C`）與 option bytes** | 抹掉一頁 flash 可以再燒回去；把 RDP 讀保護等級寫壞可能救不回來。程式從頭到尾只寫 `KEYR`/`SR`/`CR` |
| **動手前先驗 `dev_id == 0x415`** | F4 的 flash 控制器在 `0x40023C00`、L4 在 `0x40022000`。接錯板子照 L4 的位址亂寫，等於對 F446 的隨機週邊下令 |

最後一條是 Stage 2 那條「晶片資料庫」的延伸：**debugger 每多一個功能，就要多認得
一點晶片**。

### 驗證順序：階梯式，前兩階不改變 target

這個順序是設計過的，不要跳。每一階失敗都能停在無害的地方：

| 階 | 按鍵 | 驗證了什麼 | 會改變 target 嗎 |
|---|---|---|---|
| 1 | `v` | `FLASH_CR` 讀到 **`0xC0000000`** = base 位址對了 | **不會** |
| 2 | `u` → `?` | `CR` 變 **`0x40000000`**（LOCK 清掉）= KEYR 位址與 KEY1/KEY2 都對 | **不會**（`k` 可鎖回去） |
| 3 | `w` | 寫入路徑 | 會，但只動 8 byte |
| 4 | `e` → `v` | 抹除路徑 + PNB/BKER 算對了 | 會，一整頁 |

**第 3 階排在第 4 階前面是刻意的**：那頁本來就是空的，所以不必先抹。先寫的好處有兩個
—— 寫入只動 8 個 byte，風險比頁級抹除小；而且**抹除之後才有東西可以驗**（看得到簽章
消失變回全 `FF`）。抹一頁本來就空的頁，什麼都證明不了。

### 實測輸出（2026-09-11）

```
  目標頁 0x080FF800：
    +00000000 = 0xFFFFFFFF     <- 本來就是空的
  哨兵 0x08000000 = 0x20018000 0x08005485   （基準已記錄）
  FLASH_CR = 0xC0000000        <- LOCK=1 + OPTLOCK=1，重置後的正常值
> u
  ✓ 已解鎖（LOCK=0）
  FLASH_CR = 0x40000000        <- OPTLOCK 還在，那個我們永遠不碰
> w
  讀回：0x5A5AF446 0x20260911   ✓ 相符
> e
  哨兵未變 ✓（bank1 沒被動到）
> v
    +00000000 = 0xFFFFFFFF     <- 簽章消失，抹除確實有效
  哨兵 0x08000000 = 0x20018000 0x08005485   ✓ 沒被動到
```

哨兵 `0x20018000` 就是 Stage 2 讀到的 `SP[0]`，兩邊互相印證。

### L4 flash 的四個特性（跟 F4 都不一樣）

- **只能寫雙字（64-bit）**，位址要 8-byte 對齊。寫單字硬體報 `SIZERR`。
- **抹除以頁為單位，一頁 2 KB**（F4 是大小不一的 sector）。
- **1 MB 的 L476 是雙 bank**：`PNB` 是 **bank 內**的頁號（0-255），另外用 `BKER` 選 bank。
  算成全域頁號就會抹到別的地方 —— 這正是「哨兵」要防的事。
- **`EOP` 只有 `EOPIE=1` 時才會被設**（F4 是無條件設）。實測寫入與抹除都成功，但
  `EOP` 全程是 0。我們**不開 `EOPIE`** —— 那會讓 target 的 flash 中斷線變成可觸發狀態，
  而 target 的 NVIC 是原廠韌體在管的，為一行提示訊息去動它不划算。
  **真正的驗證一律是讀回來比對**，`EOP` 只是附帶資訊。

### ⚠ `swd.h` 有兩套回傳慣例（2026-09-11 踩過）

寫這一支時卡了一輪「接線明明沒動卻連不上」：

| 函式 | 成功時回傳 |
|---|---|
| `swd_read_idcode` / `swd_mem_read` / `swd_mem_write` | **`1`**（直接回 SWD 的 ACK 值） |
| `swd_connect` | **`0`**，負值才是錯誤碼 |

寫成 `swd_connect() != 1` 的話，**連線成功會被判定為失敗**。順序也固定：先
`swd_read_idcode`（它做 line reset + JTAG-to-SWD 切換），再 `swd_connect`。

這個症狀特別惡劣，因為**它會把人推去查硬體**。把方向導正的是「燒回 `swd_probe`」
—— 已知good 的韌體讀得到，就證明問題在新程式碼，不在線路。
**改動之後遇到怪事，先用已知good 的版本切一刀**，這比任何猜測都便宜。

## Stage 4a：`dap_uart` —— CMSIS-DAP 指令層（走 UART）

**✅ 2026-09-11 實測通過。接線一樣是那三條。**

Stage 4 的目標是讓 master 變成 PC 認得的標準 CMSIS-DAP probe（可以用 OpenOCD /
pyOCD 接）。但那需要在 F446 上手刻整套 USB device stack，不是一次做得完的，所以拆兩半：

| | 內容 | 需要 USB 線嗎 |
|---|---|---|
| **4a（已完成）** | `common/dap.h` 解析 DAP 命令 + `dap_uart/` 走 UART 傳輸 | **不用** |
| 4b（待做） | OTG_FS 的 USB device stack + HID 端點 | 要 |

**拆開的理由跟前面所有 Stage 一樣：一次只動一個變數。** 不拆的話，USB 列舉失敗時
你分不出是描述元寫錯還是 DAP 解析錯。拆開之後 4b 只要把同一個 `dap_process()`
接到 HID 端點上，**指令層一個字都不用改**。

### 這一輪跟前面三個 Stage 的差別

讀到的數字完全一樣，但**誰在主導協定變了**：

| | Stage 1-3.5 | Stage 4a |
|---|---|---|
| 決定協定順序的是 | 韌體自己（`swd_connect()` 一手包辦） | **主機** —— line reset、電源握手、AP 選擇全部由 PC 下令 |
| 韌體的角色 | 完整的 debugger | **只是執行 DAP 命令的機器** |

這正是 CMSIS-DAP probe 的本質：**probe 不懂 ARM 除錯架構，它只會照著主機的指示
在線上打訊號**。所有的智慧都在主機端。

### 主機端：`dap.ps1`

沒用 Python 是因為這台機器沒有 pyserial，裝套件只為了跑一支測試腳本不划算；
`System.IO.Ports.SerialPort` 本來就在 .NET 裡，`serial.ps1` 已經用同一套。

```powershell
cd <你 clone 的位置>/stm32
.\dap.ps1              # 對 COM7 跑一整輪自我測試
```

**先把 `serial.ps1` 關掉**（同一個埠不能兩個程式開）。**不用按 RESET** ——
`dap_uart` 開機不印任何文字（UART 現在是二進位通道，歡迎訊息會被當成框架垃圾）。

### 框架格式

UART 是位元組流、沒有封包邊界，所以要自己框：

```
主機 -> MCU:  0xA5  len  payload[len]  xor
MCU  -> 主機:  0x5A  len  payload[len]  xor
```

`xor` **不是為了糾錯**（這段路很可靠），而是為了在框架跑掉時立刻發現 ——
否則症狀會變成「回應內容莫名其妙」，比直接報錯難查十倍。收到不合法的框架就
整包丟掉、回去等下一個 `0xA5`，不試圖修復。

### 實測輸出（2026-09-11）

```
  廠商        board-lab
  產品        F446RE SWD probe
  capabilities 0x01  （bit0=SWD bit1=JTAG）
  封包大小    64 byte
  DAP_Connect  -> SWD
  SWJ_Sequence -> line reset + JTAG-to-SWD
  DPIDR                  0x2BA01477  ✓
  CTRL/STAT              0xF0000040  ✓ 已上電
  CPUID                  0x410FC241  ✓
  DBGMCU_IDCODE          0x10076415  ✓
  SP[0]                  0x20018000  ✓
```

### 「沒實作」要明確拒絕，不要假裝成功

`dap.h` 裡沒做的功能一律回 `DAP_ERROR`（value match、match mask、非預設的
turnaround）。**假裝成功會讓主機以為設定生效了，之後在別的地方以難懂的方式失敗。**

唯一的例外是 `DAP_SWJ_Clock`：主機用它指定 SWCLK 頻率，而我們的 `SWD_DLY()` 是寫死的
迴圈延遲，調不了。這裡**回 OK 但實際上沒照做** —— 因為 CMSIS-DAP 沒有「我只有固定
速度」這個回答，回 ERROR 會讓主機直接放棄連線。代價是主機以為自己設定的頻率生效了。
要真的支援就得把 `SWD_DLY()` 改成可變的，記在「還沒做」。

### ⚠ PowerShell 5.1：大於 `0x7FFFFFFF` 的十六進位常值會變成負數

寫 `dap.ps1` 時**連炸兩次**，都是同一個原因：

```
0xA0000000  ->  -1610612736      （Int32 溢位）
0xE000ED00  ->  -536810240
```

- 拿去跟 `-band` 的結果比較**永遠不相等** —— `CTRL/STAT` 明明是 `0xF0000040`、
  兩個 ACK 都亮了，卻判成「電源握手失敗」
- 轉 `[uint32]` 會直接拋「數值對 UInt32 而言太大或太小」
- **`[uint32]0xA0000000` 不能用來修** —— 轉型發生在常值已經是負數之後

解法有兩個：加 `L` 後綴（`0xA0000000L`），或**在函式邊界收斂**（參數收 `[long]`，
進來先 `-band 0xFFFFFFFFL`）。處理 MCU 暫存器時整批常值都會中獎，
**優先用後者** —— 在每個常值後面撒 `L`，漏一個就再炸一次。

> 這一輪三次失敗**全部在主機端**，韌體的 `dap.h` / `dap_uart` 一個字都沒改過。
> `DPIDR` 讀得到就證明 DAP 解析、SWJ 序列、`swd_xfer` 的參數對應都是對的 ——
> **能讀到第一個正確的值，嫌疑範圍就小了一大半。**

### Stage 4b 需要的硬體（回家先找）

NUCLEO-64 **沒有**幫 F446 拉出任何 USB 接頭（板上那顆 mini-B 是 ST-LINK 的）。
要讓 master 變成 USB 裝置，需要一個插進 PC 的 **USB Type-A 公頭**：

| 線色（慣例） | 訊號 | 接到 |
|---|---|---|
| 綠 | D+ | **PA12** |
| 白 | D− | **PA11** |
| 黑 | GND | GND |
| 紅 | VBUS | **不接** —— 板子由 ST-LINK 那條供電，韌體裡把 VBUS 偵測關掉 |

最省事是剪一條不要的 USB 線，保留 A 公頭那端。⚠ **線色只是慣例，要用電表量**：
A 公頭有金屬片那面看過去，**pin1=VBUS、pin2=D−、pin3=D+、pin4=GND**。
D+/D− 接反的症狀是「完全沒有列舉事件」，跟線沒接好長得一模一樣
（在一片 NodeMCU 上遇過同樣症狀，很難查）。

## Stage 4b：`dap_usb` —— OTG_FS USB device stack + HID 端點

**✅ 2026-09-14 實測通過：USB 列舉成功、pyOCD 認得這支 probe 並能跟它對話。**

```
> pyocd list
  #   Probe/Board                  Unique ID                          Target
  0   board-lab F446RE CMSIS-DAP   HXQKFMFC7V24PE6D3AZCGWDIG3GKTF4S   n/a
```

`board-lab` / `F446RE CMSIS-DAP` 這兩個字串是 **pyOCD 透過 EP1 送 `DAP_Info`
命令問出來的**，不是 USB 描述元裡的字串——所以這一行同時證明了三件事：
列舉成功、HID 端點通、`dap_process()` 在 USB 上跑起來了。

**而且它真的能當 debugger 用。** 用標準工具鏈把 Stage 2 當年手刻讀出來的值
重現一次，四個全中：

```
> pyocd cmd -t cortex_m -O connect_mode=attach -c "read32 0xE0042000" ...
e0042000:  10076415     dev_id=0x415 -> STM32L476
1fff75e0:  ffff0400     低半字 0x400 = 1024 KB flash
08000000:  20018000     target 初始 SP（SRAM1 96K 頂端）
e000ed00:  410fc241     Cortex-M4 r0p1
```

halt / 讀核心暫存器 / run 也通（Stage 3 手刻做過的事，現在由 pyOCD 代勞）：

```
> pyocd cmd -t cortex_m -O connect_mode=attach -c "halt" -c "reg" -c "go"
Successfully halted device
      pc: 0x080026ee          sp: 0x20017f6c
      lr: 0x0800272d        xpsr: 0x21000000
Successfully resumed device
```

`pc` 落在 L476 原廠 demo 的 flash 範圍內，跟 Stage 3 抓到的 `0x080026F4`
幾乎是同一段程式碼——兩個世代的實作互相印證。

跟前面三個 Stage 不一樣的地方：USB 是全新的子系統，**沒有已知 good 的版本
可以切一刀比對**——所以這一輪抓 bug 全靠「把狀態暫存器印出來」，
見〈五個上機才抓到的 bug〉。

### 做了什麼

- `common/usb_otg.h`：手刻的 OTG_FS 裝置端驅動（時脈/PLL、PHY、控制傳輸的
  FIFO 收送、端點開關），照 RM0390 第 22 章的暫存器位置刻的，風格跟 `swd.h`
  一樣（沒有 HAL、沒有 CMSIS device 標頭）。
- `common/usb_desc.h`：描述元。HID class，report descriptor 是 CMSIS-DAP 慣用的
  vendor-defined 64 byte in / 64 byte out（不假裝滑鼠鍵盤）。VID/PID 用
  `0x1209`/`0xDA01`（pid.codes 測試區段，本地開發用，沒有要上架不用申請正式碼）。
- `dap_usb/main.c`：控制傳輸的標準請求（GET_DESCRIPTOR / SET_ADDRESS /
  SET_CONFIGURATION / HID SET_IDLE-SET_PROTOCOL）+ EP1 收 HID OUT report、丟給
  `dap_process()`（跟 Stage 4a **一字不改**）、把回應從 EP1 IN 送出去。輪詢
  `GINTSTS`，沒用 NVIC——跟這個專案其他韌體一樣，`core/startup.c` 只掛了 16 個
  系統向量，不開外部中斷。

### 硬體（✅ 已驗證）

- 剪開的 USB 線：綠 D+ → **PA12 = CN10 pin 12**、白 D− → **PA11 = CN10 pin 14**、
  黑 GND → **CN10 pin 20**、**紅 VBUS 不接**（不接是刻意的，見下面 bug #1）
- **PA11/PA12 只在內側的 Morpho 排針（CN10）上**，外側的 Arduino 排針沒有引出
  這兩支腳。腳位表在 ST 的 **UM1724**（*STM32 Nucleo-64 boards (MB1136)*）。
- **電表量不出 PA11/PA12 在哪**：這兩支腳預設浮接，沒有像 DISCO CN3 那種
  上拉／下拉指紋可以認。電表在這裡只有兩個用途：確認 USB 線的線色對應
  （把 A 頭插上電腦，量到 **VBUS 對 GND = 5V** 就代表線色假設正確），
  以及拿「已知好的 SWD GND 線」當參考確認新拉的 GND 有接到同一個網路。
- SWD 三條線維持 Stage 1-4a 那組接法不變，一條都不用改。
- ST-LINK 那條 USB 線繼續插著——`dap_usb` 的除錯 log 走 USART2，
  跟新接的那條 USB 線是兩回事，不衝突。**兩條線可以同時插。**

### 六個上機才抓到的 bug（每一個都花掉一輪）

這一節是這個 Stage 最值錢的部分。下面表格是 USB 那五個，第六個在 `dap.h`
（見本節後段〈bug #6〉，那個最有教育意義）。共同點是：**五個裡面有四個的
症狀完全相同（「看起來一切正常，但主機沒反應」），而且六個沒有任何一個
會在編譯時報錯。**

| # | bug | 症狀 | 為什麼難查 |
|---|---|---|---|
| 1 | **F446 的 `GCCFG` 佈局跟其他 F4 不一樣**：別的 F4 是 `NOVBUSSENS=1` 關閉 VBUS 偵測，F446 同一格是 `VBDEN`，**設 1 是打開**。照別的型號抄 = 手滑把偵測打開，而我們沒接 VBUS | D+ 永遠量不到 3.3V 上拉，插電腦毫無反應 | 網路上絕大多數 STM32 USB 範例都是 F407 的 |
| 2 | **設 `GUSBCFG.FDMOD` 之後要等 50ms** 才能碰其他暫存器（ST HAL 的 `USB_SetCurrentMode` 裡就是 `HAL_Delay(50)`）。另外 `PHYSEL` 要在 core reset **之前**設 | 同上，完全沒反應 | 不等也不會報錯，就只是後面的設定悄悄不生效 |
| 3 | **光初始化完不會連上**：要另外清 `DCTL.SDIS`（soft disconnect）才會真的驅動 D+ 上拉。ST HAL 把這步拆成獨立的 `USB_DevConnect()` | 同上，完全沒反應 | 「初始化」跟「接上去」是兩件事，不看 HAL 的呼叫順序不會想到 |
| 4 | **USB reset 之後沒把 `DCFG.DAD` 清回 0**；另外 `SET_ADDRESS` 的位址要在收到 SETUP 當下立刻寫，不要等狀態階段送完才寫（會有競態） | `USBRST → ENUMDNE → SET_ADDRESS` 無限重複，每輪位址加一 | 看起來像主機一直重試，很容易往硬體方向查 |
| 5 | **`usb_hid_report_desc[33]` 但內容只有 27 byte**，C 安靜地補 6 個 `0x00`，而 `0x00` 在 report descriptor 裡是無效項目 | **列舉全程正常、`SET_CONFIGURATION` 都過了**，主機收完報告描述元才默默把匯流排掛起 | 「初始值比陣列短」是合法 C，`-Wall` 也不警告。已加 `_Static_assert` 防再犯 |

### 怎麼查：把狀態暫存器印出來，不要猜

USB 沒有「燒回已知 good 版本切一刀」這招可用，所以這一輪是靠 `dap_usb/main.c`
裡那個每秒心跳破的案：

```
HB gints=44808C28 dsts=3B07 doep0=8000 diep1=004C
```

判讀要點（**這幾條比上面的 bug 清單更通用**）：

- **`DSTS` 中間那幾位是 frame number，主機活著就會每 1ms 遞增。**
  數字**凍結** = 主機停止送 SOF = 匯流排被掛起（主機放棄了）；
  數字**在跳** = 主機還在講話，那問題就在我們這端收不到。
  這一條把「主機不理我」和「我聽不到主機」分開，是整輪除錯的關鍵分岔點。
- `DSTS` 的 bit0 `SUSPSTS=1` 也代表掛起。**但裝置閒置時被主機選擇性掛起是正常的**
  ——列舉成功之後沒人開啟它，它本來就會進 suspend，不要誤判成失敗。
- **`GINTSTS` 的位元是累積的（W1C），我們只清 `USBRST` 和 `ENUMDNE`**，
  所以其他位元反映的是「歷史上發生過」，不是當下狀態。拿它判斷即時狀況會被騙。
  （查這輪時我就把 bit5 `NPTXFE` 誤讀成 bit4 `RXFLVL`，白繞一圈。
  **`RXFLVL` 是 bit4。**）
- 除錯訊息要短。`uart_puts` 是阻塞的，115200 baud 下一行 60 字元 = **5ms**，
  而 `SET_ADDRESS` 之後主機最快 2ms 就問下一題——**印 log 本身會變成 bug**。
  所以 `dbg_hex2()` 只印兩位、格式壓到最短，而且**先餵硬體再印**。

### Windows 端的兩個查法

- **裝置管理員的「代碼 10」不只一種**。用 PowerShell 問出真正的錯誤碼：

  ```powershell
  Get-PnpDeviceProperty -InstanceId 'USB\VID_1209&PID_DA01\...' -KeyName DEVPKEY_Device_ProblemStatus
  ```

  這輪拿到 `3221225501` = `0xC000001D` = **invalid HID report descriptor**，
  直接指向 bug #5。沒有這個碼的話光看「代碼 10」什麼都查不到。
- **列舉成功的正面證據**：Windows 會**回頭再讀一次字串描述元**
  （log 裡連續出現 `S 80 06 0300/0301/0302`），並且建出第二個裝置節點
  `HID\VID_...`（「符合 HID 標準的廠商定義裝置」）。看到這兩件事才算真的過。

### 兩個刻意的簡化，寫下來是因為以後可能要改

- **所有描述元都 ≤64 byte**（config 描述元含 interface+HID+2 endpoint 是
  41 byte），所以 `ep0_send()` 沒寫「跨封包續傳」——GET_DESCRIPTOR 一次
  IN transaction 就送完。加新描述元讓它超過 64 byte 的話，這裡要補。
- SYSCLK **沒有**切到 PLL，還是 HSI 16 MHz；PLL 只用來生 USB PHY 要的
  48 MHz（走 PLLQ）。AHB 維持 16 MHz、`GUSBCFG.TRDT` 用查表值 `0xE`
  （16-17 MHz 那一格）—— **實測可用，這個查表值是對的**。

### HID 的固定長度：回應一定要補滿 64 byte

`dap_process()` 的回應長度是變動的，但 **HID 的 interrupt report 是固定長度**，
主機照描述元宣告的 64 byte 在讀。只送實際長度的話 pyOCD 端會是 `read error`。
真正的 CMSIS-DAP 韌體一律送滿 64 byte、尾巴補 0，我們也照做
（`dap_usb/main.c` 主迴圈裡那段補 0）。**這跟 Stage 4a 的 UART 框架相反**
——那邊是自己框長度，愛送多少送多少。

### bug #6：AP 的讀取是「延後的」（posted）——這一個最有教育意義

USB 全部打通之後，pyOCD 還是連不上：

```
0001212 E Error reading AP#0 IDR: Invalid AP address (#0) [discovery]
0001239 E Error while initing target: No cores were discovered!
```

**這已經不是 USB 的問題** —— pyOCD 是真的在透過我們的 probe 下 SWD 命令，
只是讀回來的值不對。根因在 `common/dap.h`：

> 在 SWD 上送出一筆 **AP read**，線上回來的資料是**上一次** AP read 的結果。
> 這一次的結果要等下一筆 AP read，或是讀 DP 的 `RDBUFF`（`0x0C`）才拿得到。

`swd.h` 的 `swd__ap_rd()` 本來就有補 `RDBUFF` 那一筆，所以 **Stage 1-3.5 全部
是對的**。但 `dap__transfer()` 不能用它——DAP_Transfer 必須忠實執行主機給的
每一筆，而 **CMSIS-DAP 規格把「處理 posted read」規定成韌體的責任**
（ARM 官方 `DAP.c` 的 `DAP_SWD_Transfer` 就是這個狀態機）。原本那版直接呼叫
原始的 `swd_xfer()`，於是 AP 讀回來的永遠是過期資料。

**為什麼 Stage 4a 測不出來**：`dap.ps1` 是我們自己寫的主機端，它在**主機這邊**
自己補了 RDBUFF 那一筆——等於兩邊都不做、或兩邊都做，剛好湊成對。pyOCD 照
規格假設韌體會做，就露餡了。**自己寫的測試只會測到自己想到的用法**，
這是這一輪最值得記住的一句。

現在 `dap__transfer()` 實作的三條規則（程式碼就是照這三條寫的）：

- **讀 AP**：還沒有 posted 的讀取就先送一筆把它 post 出去（結果丟掉）；
  之後每一筆 AP read 收回來的都是「上一筆」的結果。
- **讀 DP 或任何寫入之前**：有 posted 沒收的話，先讀 `RDBUFF` 收回來
  （不先收的話它會被沖掉）。
- **迴圈結束**：還欠著就補一筆 `RDBUFF`。

`DAP_TransferBlock` 同理但簡單些（整段都是同一個暫存器）：開頭 post 一筆，
**最後一筆改成讀 `RDBUFF`** 收尾。

### ✅ 最後一哩：用 pyOCD 透過這支 probe 燒 target 的 flash（2026-09-14）

讀、halt/step/run 都通之後，debugger 還差「燒錄」才算完整。**驗收標準沿用
Stage 3.5 手刻 `flash_wr` 那一套**，只是這次由標準工具鏈執行：

```
> .\pyocd.ps1 -Flash build\pyocd_sig.bin -At 0x080FF800
> .\pyocd.ps1 -Read 0x080FF800      -> 5a5af446     簽章寫進去了
> .\pyocd.ps1 -Read 0x080FF804      -> 20260914
> .\pyocd.ps1 -Read 0x08000000      -> 20018000     哨兵沒變，bank1 沒被動到
> pyocd cmd ... -c "halt" -c "reg pc" -c "go"  -> pc = 0x0800272c
```

`pc` 跟燒錄前抓到的 `0x080026ee` 同一段程式碼 —— 原廠 demo 沒受影響。

**這一步的保險（跟 `flash_wr` 的三道是同一個思路，但實作位置不同）：**

| 保險 | 在哪裡 |
|---|---|
| 動手前先做 **1MB 全備份**（`build/l476_factory_backup.bin`） | 唯一的還原路徑，**破壞性操作前一定要先有它** |
| 只燒 bank2 最後一頁 `0x080FF800` | 原廠 demo 用不到，燒錄前確認過是全 `FF` |
| **`--erase sector`，絕不 `chip`** | 寫死在 `pyocd.ps1` 裡，不開放從參數改 —— chip erase 會清掉整顆 |
| 目標型號 `stm32l475xg` | pyOCD 沒有 l476 的內建支援，但 **L475/L476/L486 是同一顆晶片**（`dev_id=0x415`，見上面的裝置 ID 表），flash 控制器與佈局相同 |

> ⚠ **走 pyOCD 這條路會繞過 `flash_wr` 韌體裡那三道保險**（只碰 bank2 最後一頁、
> 絕不寫 option bytes、動手前驗 `dev_id`）—— 那些是我們自己韌體裡的檢查，
> pyOCD 不知道它們存在。所以保險改成上面這四條，**由主機端與流程來保證**。
> 全備份那條特別重要：它把「不可逆」降級成「可還原」。

備份本身也是對 `DAP_TransferBlock` 的壓力測試：1 MB 讀出來只花 2.4 分鐘，
而且開頭兩個字跟 README 記錄的哨兵 `20018000 08005485` 完全相符
—— **大量讀取的路徑是正確的**，不只是單筆讀取。

## 上機順序（一次只動一個變數）

寫程式的部分到此為止，兩支 app 都是「編得過、沒跑過」。**下一步是上機，不是再加功能。**
順序是設計過的：每一步只引入一個新的失敗來源，出問題時才知道是誰的錯。

### 0. 桌面準備（斷電做）

- [ ] L476G-DISCO：**拔掉 CN3 兩個跳線帽**，收好（很小，會滾）
- [ ] 確認 JP6 在 `3V3`、JP5 在 `ON`、JP3 閉合 —— 這三個**不要動**
- [ ] 兩條 USB 線都插上，兩片各自從自己的 USB 供電
- [x] **確認 L476 的 LD2（紅色電源 LED）仍然亮** —— ✅ 2026-09-11 實測通過
- [ ] **NUCLEO 的 CN2 不要拔**（見「接線」一節的對照表）

### 1. master 單獨會不會講話（還不接 target）

**`swd_probe` 是燒給 master（NUCLEO-F446RE）的。** 這一整套裡 target 從頭到尾
都不燒任何東西 —— 它跑什麼都行（原廠 demo 也可以），我們只是從外面讀它。

把 `build/swd_probe/swd_probe.bin` 拖進 **NUCLEO 的 E: 磁碟**（`NODE_F446RE`）、
**先不接那三條線**，然後：

```powershell
cd <你 clone 的位置>/stm32
.\serial.ps1               # COM7 = master
```

**開埠之後按 NUCLEO 上的黑色 RESET 鈕**（開埠本身不會重置它）。

**✅ 2026-09-11 實測通過**，逐字輸出：

```
=== STM32 SWD probe ===
no ACK (7) — 檢查接線 / 共地 / target 的 ST-LINK 隔離跳線
```

`serial.ps1` 的互動迴圈同時得到驗證（能開埠、能顯示、`Ctrl+]` 正常關埠）。
`ack=7` 就是 SWD 線上沒有任何裝置回應時讀到的全 1 —— 沒接線本來就該是這樣。

- 應該看到 `=== STM32 SWD probe ===` 然後 `no ACK`。
- **看到 `no ACK` 是對的** —— 沒接線本來就讀不到。重點是**前面那行字要出現**：
  它證明 F446 有跑、USART2 通了、虛擬 COM port 對了。
- 如果連那行字都沒有：問題在 master 自己（燒錄 / COM port / 鮑率），
  跟 SWD、跟 target 一點關係都沒有。**不要開始動接線。**

### 2. 接三條線，讀 target 身分 —— ✅ 2026-09-11 通過

接 **CN3 pin 2 → D8（PA9, SWCLK）**、**CN3 pin 4 → D7（PA8, SWDIO）**、GND→GND，
按 master 的黑色 RESET 鈕重跑。

- [x] `DPIDR = 0x2BA01477`
- [x] `CPUID = 0x410FC241`
- [x] **`dev_id=0x415 -> STM32L475/L476/L486`** ← 這一行才是真的證據
- [x] `FLASH = 1024 KB`
- [x] `SP[0] = 0x20018000`（L476 的 SRAM1 只有 96K，不是 F446 的 `0x20020000`）

**如果 `dev_id` 印出 `0x421`（=F446），代表你讀到的是 master 自己**，
線接錯了或根本沒接上。這正是加這一行的原因 —— 前兩行在這種情況下也會是「對的」。

還是 `no ACK` 的話，照這個順序查（由便宜到貴）：
1. CN3 跳線帽真的拔掉了嗎
2. GND 有沒有接（最常漏的一條）
3. PA8/PA9 有沒有插錯排針孔（D7/D8，數過去容易差一格）
4. 杜邦線本身通不通（用電表量，母-母線很常一頭是壞的）

### 3. cpu_ctrl —— ✅ 2026-09-11 通過

線完全不動，只換燒 `cpu_ctrl.bin`。

- [x] 開機看到狀態行（`RUN` 或 `HALT` + `DHCSR=`）
- [x] 按 `h` → 變 `HALT`，`PC=` 出現一個 `0x08...` 的值
- [x] 按 `s` 幾次 → `PC` 每次前進 2 或 4（Thumb 指令長度）
      **這是整個練習真正的證明點**：你的板子正在一條一條推另一顆 CPU 的指令。
- [x] 按 `d` → 17 個暫存器列出來
- [x] 按 `r` → 回到 `RUN`，`PC` 變 `--------`
- [x] **`DHCSR` 的 bit 3 在 `s` 時是 1、`r` 時是 0** ← 這一項是上機才加的，見上面
      「`C_MASKINTS` 必須單獨寫一次」

實測片段（修好 `C_MASKINTS` 之後）：

```
> s
  HALT    DHCSR=0x0103000F  PC=0x08002734
> r
  RUN     DHCSR=0x01010001  PC=--------
> h
  HALT    DHCSR=0x01030003  PC=0x08000246
> r
  RUN     DHCSR=0x01010001  PC=--------
> h
  HALT    DHCSR=0x01030003  PC=0x0800539C
```

`DHCSR` 高半字的 `0x0100` 是 `S_RETIRE_ST`（上次讀取後有指令退休過），正常狀態位元。

### 4. 把實測貼回來

終端機輸出整段複製給我，我補進這份 README、在 `HANDOVER.md` 最上面開一段、
必要時修文件裡推錯的地方。（Stage 1-2 已經走完這個流程一輪；剩 Stage 3。）

### 可能會遇到、但先不要預先解決的事

L476 跑原廠 demo 時如果進了 **STOP mode**，SWD 會斷（低功耗模式下除錯時脈被關掉），
症狀是本來讀得到、動一動 joystick 之後就 `no ACK`。

**真的遇到了再處理** —— 解法是接第四條線（master 的 PB10 → target 的 NRST）做
connect-under-reset，`common/swd.h` 已經留好腳位定義。現在先不接，因為我們還不
知道會不會發生，而多一條未驗證的線只會讓第 2 步的除錯變難。

## 目前內容

- `core/` —— 最小 startup + linker（**master 的** F446RE：512K flash @ `0x08000000`、128K RAM）
- `common/` —— `stm32f446.h`（手刻暫存器）、`delay.h`（DWT）、`uart.h`（USART2）、
  `swd.h`（nano130 SWD 引擎的 STM32 移植）
- `swd_probe/` —— Stage 1+2：DPIDR + CPUID + **DBGMCU_IDCODE + flash 容量** + 初始 SP
- `cpu_ctrl/` —— Stage 3：halt / step / run + 核心暫存器 + 讀記憶體（見下）
- `flash_wr/` —— Stage 3.5：解鎖 / 寫入 / 抹除 target 的 flash（見上）
- `dap_uart/` + `common/dap.h` —— Stage 4a：CMSIS-DAP 指令層，走 UART（見上）
- `dap.ps1` —— Stage 4a 的主機端自我測試
- `dap_usb/` + `common/usb_otg.h` + `common/usb_desc.h` —— Stage 4b：OTG_FS
  USB device stack + HID 端點，**✅ 2026-09-14 實測 pyOCD 認得（見上）**
- `pyocd.ps1` —— pyOCD 的包裝。pyocd.exe 藏在 VS Code 的 CMSIS Debugger 擴充
  套件裡、路徑長到每次打都會出錯；而且它把 `-t stm32l475xg` 與
  **`--erase sector`（絕不 chip）** 寫死，不開放從參數改掉

## 還沒做

- **`dap.ps1` 的自我測試要補上 posted read 的案例**——目前它是在主機端自己補
  `RDBUFF`，所以韌體端做不做都測得過（bug #6 就是這樣漏掉的）。
  在那之前，**真正的驗收標準是 pyOCD 能不能連上**，不是 `dap.ps1` 過不過。
- 把 `build/pyocd_sig.bin` 那頁抹掉、讓 target 回到全 `FF` 的乾淨狀態
  （`flash_wr` 的 `e` 或 `pyocd erase --sector 0x080FF800`）。不急，那一頁
  原廠 demo 本來就用不到，留著簽章也不影響任何東西。
- `dap_usb/main.c` 的除錯輸出（`S ...` / `HB ...` / `D ...`）可以拿掉了，
  但**建議留著**——那是唯一能看見 USB 內部狀態的窗口，而且 115200 的 log
  在正常運作時不影響功能（只在時序極限時要注意，見上面）。
- L476 當 **master** 的支援（要另一份 `core/` 與 `stm32l476.h`，見上面「硬體」一節）。
  這是獨立的一步，不要跟現在這版混在一起
- `SWD_DLY()` 改成可變的，讓 `DAP_SWJ_Clock` 能真的生效
- `flash_wr` 變成真正的燒錄器：從 UART 收 binary、連續寫、跨頁
