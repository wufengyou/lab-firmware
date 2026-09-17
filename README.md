# lab-firmware

<https://lab.da-acadmy.com> 那些實驗的程式碼。

教材在網站上，**程式碼在這裡**。兩邊分開的理由很簡單：教材是拿來讀的，
程式碼是拿來 clone、build、改壞再改回來的。

## 有什麼

| 目錄 | 是什麼 | 教材 |
|---|---|---|
| [`stm32/`](stm32/) | 用一塊 NUCLEO-F446RE 當 SWD 除錯器去讀另一塊 STM32。無 HAL、無 CMSIS device 標頭，SWD/DAP 協定自己刻。最後由 pyOCD 獨立驗證。 | [軌跡](https://lab.da-acadmy.com/stm32/) · [自己做一次](https://lab.da-acadmy.com/stm32/repro/) |
| [`nano130/`](nano130/) | 把 Nuvoton NANO130 客製板做成燒錄器，讓它燒另一塊同型板。PC 端只剩一支 PowerShell。 | [軌跡](https://lab.da-acadmy.com/nano130/) · [自己做一次](https://lab.da-acadmy.com/nano130/repro/) |
| [`xbee/`](xbee/) | XBee 雙向鏈路的量測工具。主題是「收到了」跟「收對了」不是同一件事。 | [軌跡](https://lab.da-acadmy.com/xbee/) · [自己做一次](https://lab.da-acadmy.com/xbee/repro/) |

**先讀各目錄的 `README.md`**，裡面有工具鏈、腳位、以及踩過的坑。

## 這不是教學範例碼

這是實驗筆記本裡的程式碼，保留了當時的判斷過程：
註解會寫「為什麼是這個數字」，錯誤的假設會被留下並標記為錯而不是刪掉。
有些地方看起來囉唆，那是刻意的——當初卡住的就是那裡。

幾個例子：

- `nano130/isp_send.ps1` 的參數收 `[long]` 而不是 `[int]`，因為 PowerShell 5.1
  大於 `0x7FFFFFFF` 的十六進位常值會溢位成負數
- `stm32/common/swd.h` 的回傳慣例在註解裡寫明了，因為曾經有兩套慣例混用卡了一輪
- `nano130/flash_wr/main.c` 的驗證階梯把「不改變 target」與「會寫 flash」明確切開

## 建置

兩個 Makefile 的工具鏈路徑都是 `?=` 預設值，可以從命令列覆蓋：

```sh
make TOOLCHAIN="D:/gcc-arm/bin"          # nano130
make TOOLCHAIN="…/bin/arm-none-eabi-"    # stm32（是前綴不是目錄）
```

## 安全

`nano130/flash_wr` 與 `stm32/flash_wr` **會永久改變 target 的 flash**。
動它們之前先讀對應 README 的保險條款與備份步驟。
用 pyOCD 直接寫入會繞過韌體內的保險——那時候完整備份是唯一的還原路徑。

## 授權

程式碼 MIT（見 [LICENSE](LICENSE)）。
`nano130/vendor/` 是第三方的，各自保留原授權，見 [THIRD-PARTY.md](THIRD-PARTY.md)。
