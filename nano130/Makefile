# nano130 韌體建置 —— arm-none-eabi-gcc + 精簡版 Nano100B BSP（vendor/）
#
# 工具鏈預設值是這台機器上唯一那份的路徑（省得每次重新推導）。
# 但它是預設值不是寫死：換機器或別人要重現時，從命令列覆蓋即可，不必改檔。
#
#   make TOOLCHAIN=/usr/bin            (工具鏈在 PATH 上時)
#   make TOOLCHAIN="D:/gcc-arm/bin"
#
#   make            編譯，產出 build/blink.bin
#   make flash      編譯後用 NuLink 燒進 APROM 並驗證
#   make clean

TOOLCHAIN ?= C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/14.2 rel1/bin
CC      := $(TOOLCHAIN)/arm-none-eabi-gcc
OBJCOPY := $(TOOLCHAIN)/arm-none-eabi-objcopy
SIZE    := $(TOOLCHAIN)/arm-none-eabi-size

NULINK  ?= C:/Program Files (x86)/Nuvoton Tools/NuLink Command Tool/NuLink.exe

# 要建哪一支：make APP=keypad_lcd flash   （預設 blink）
APP     ?= blink
V       := vendor
B       := build/$(APP)

CPU     := -mcpu=cortex-m0 -mthumb
INC     := -I$(V)/CMSIS/Include -I$(V)/Device/Include -I$(V)/StdDriver/inc -Icommon -I$(APP)
# -MMD -MP：產生 .d 相依檔並在檔尾 include 進來。沒有這個的話改 common/*.h
# 不會觸發重編（make 只看 .c 的時間戳），它會說 "Nothing to be done"，然後你
# 燒進去的是舊的 binary —— 症狀是「改了沒生效」，最難查的那一種。
# 2026-09-14 補上：stm32/ 那邊 2026-09-11 就加了，這裡一直漏著。
CFLAGS  := $(CPU) $(INC) -Os -g3 -ffunction-sections -fdata-sections -Wall -std=gnu11 -MMD -MP
ASFLAGS := $(CPU) -g3
LDSCRIPT:= $(V)/Device/Source/GCC/gcc_arm.ld
LDFLAGS := $(CPU) -T"$(LDSCRIPT)" -Wl,--gc-sections -Wl,-Map=$(B)/$(APP).map \
           --specs=nano.specs --specs=nosys.specs

SRC := $(APP)/main.c \
       $(V)/Device/Source/system_Nano100Series.c \
       $(V)/StdDriver/src/gpio.c \
       $(V)/StdDriver/src/clk.c \
       $(V)/StdDriver/src/sys.c \
       $(V)/StdDriver/src/uart.c \
       $(V)/StdDriver/src/usbd.c
ASM := $(V)/Device/Source/GCC/startup_Nano100Series.S

OBJ := $(patsubst %,$(B)/%.o,$(notdir $(basename $(SRC)))) $(B)/startup_Nano100Series.o

vpath %.c $(sort $(dir $(SRC)))
vpath %.S $(dir $(ASM))

all: $(B)/$(APP).bin

$(B)/%.o: %.c | $(B)
	"$(CC)" $(CFLAGS) -c $< -o $@

$(B)/%.o: %.S | $(B)
	"$(CC)" $(ASFLAGS) -c $< -o $@

$(B)/$(APP).elf: $(OBJ)
	"$(CC)" $(LDFLAGS) $^ -o $@
	"$(SIZE)" $@

$(B)/$(APP).bin: $(B)/$(APP).elf
	"$(OBJCOPY)" -O binary $< $@

$(B):
	mkdir -p $(B)

# NuLink 會把傳入路徑轉大寫再找檔，相對路徑會失敗 —— 一律給絕對路徑。
flash: $(B)/$(APP).bin
	"$(NULINK)" -e APROM
	"$(NULINK)" -w APROM "$(CURDIR)/$(B)/$(APP).bin"
	"$(NULINK)" -v APROM "$(CURDIR)/$(B)/$(APP).bin"
	"$(NULINK)" -reset

clean:
	rm -rf $(B)

.PHONY: all flash clean

-include $(OBJ:.o=.d)
