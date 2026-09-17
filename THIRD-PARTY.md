# 第三方元件

## `nano130/vendor/`

Nuvoton Nano100B BSP 的精簡子集，只保留編得起來需要的部分：
CMSIS core（cm0）、Device（header + startup + linker + system）、StdDriver 全套 src/inc。

隨原始碼附上而不是叫人自己去下載，理由是**版本鎖定**：
BSP 版本不同會編不過，或編過了但行為不同。

| 來源 | 授權 | 版權 |
|---|---|---|
| Nuvoton Nano100B BSP（`Device/`、`StdDriver/`） | Apache-2.0 | Copyright (C) 2014 Nuvoton Technology Corp. |
| ARM CMSIS（`CMSIS/Include/`） | BSD-3-Clause | Copyright (c) 2009–2015 ARM Limited |

原始授權標頭都保留在各檔案內，未經修改。
