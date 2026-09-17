# STM32F446RE 韌體建置（NUCLEO-F446RE）—— arm-none-eabi-gcc，無 HAL、無 CMSIS device 標頭
#
#   make                     編譯 swd_probe -> build/swd_probe/swd_probe.bin
#   make APP=xxx              編譯別的 app
#   make flash NODE=/e        把 .bin 複製到 NUCLEO 的 USB 磁碟（drag-drop 燒錄）
#   make clean
#
# 工具鏈：預設吃 PATH 上的 arm-none-eabi-*。不在 PATH 就覆蓋 TOOLCHAIN，例如：
#   make TOOLCHAIN="C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin/arm-none-eabi-"

TOOLCHAIN ?= arm-none-eabi-
CC      := $(TOOLCHAIN)gcc
OBJCOPY := $(TOOLCHAIN)objcopy
SIZE    := $(TOOLCHAIN)size

APP     ?= swd_probe
B       := build/$(APP)

CPU     := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
INC     := -Icommon -Icore -I$(APP)
# -MMD -MP：產生 .d 相依檔並在下面 include 進來。沒有這個的話改 common/*.h
# 不會觸發重編（只看 .c 的時間戳），make 會說 "Nothing to be done"，然後你燒進去
# 的是舊的 binary —— 2026-09-11 踩過，症狀是「改了沒生效」，最難查的那一種。
CFLAGS  := $(CPU) $(INC) -Os -g3 -ffreestanding -ffunction-sections -fdata-sections -Wall -std=gnu11 -MMD -MP
LDSCRIPT:= core/stm32f446.ld
LDFLAGS := $(CPU) -T"$(LDSCRIPT)" -nostartfiles -Wl,--gc-sections -Wl,-Map=$(B)/$(APP).map \
           --specs=nano.specs --specs=nosys.specs

SRC := $(APP)/main.c core/startup.c
OBJ := $(patsubst %,$(B)/%.o,$(notdir $(basename $(SRC))))

vpath %.c $(sort $(dir $(SRC)))

all: $(B)/$(APP).bin

$(B)/%.o: %.c | $(B)
	"$(CC)" $(CFLAGS) -c $< -o $@

$(B)/$(APP).elf: $(OBJ)
	"$(CC)" $(LDFLAGS) $^ -o $@
	"$(SIZE)" $@

$(B)/$(APP).bin: $(B)/$(APP).elf
	"$(OBJCOPY)" -O binary $< $@

$(B):
	mkdir -p $(B)

# NUCLEO 板載 ST-LINK 會掛成一顆 USB 磁碟；把 .bin 丟進去就燒好了。
# NODE 給磁碟路徑（Git Bash 下 E: 是 /e）。
flash: $(B)/$(APP).bin
	@test -n "$(NODE)" || (echo "用法: make flash NODE=/e   （NUCLEO 磁碟路徑）"; exit 1)
	cp "$(B)/$(APP).bin" "$(NODE)/"
	@echo "已複製到 $(NODE) —— ST-LINK 會自動燒錄，燈閃完即可。"

clean:
	rm -rf $(B)

-include $(OBJ:.o=.d)

.PHONY: all flash clean
