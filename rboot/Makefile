#
# Makefile for rBoot
# https://github.com/raburton/esp8266
#

RBOOT_BUILD_BASE ?= build
RBOOT_FW_BASE    ?= firmware

SDK_DIR=../sdk/esp_iot_sdk_v2.0.0

ifndef XTENSA_BINDIR
CC := xtensa-lx106-elf-gcc
LD := xtensa-lx106-elf-gcc
OBJDUMP := xtensa-lx106-elf-objdump
OBJCOPY := xtensa-lx106-elf-objcopy
else
CC := $(addprefix $(XTENSA_BINDIR)/,xtensa-lx106-elf-gcc)
LD := $(addprefix $(XTENSA_BINDIR)/,xtensa-lx106-elf-gcc)
OBJDUMP := $(addprefix $(XTENSA_BINDIR)/,xtensa-lx106-elf-objdump)
OBJCOPY := $(addprefix $(XTENSA_BINDIR)/,xtensa-lx106-elf-objcopy)
endif

ifeq ($(V),1)
Q :=
else
Q := @
endif

CFLAGS    = -Os -O3 -Wpointer-arith -Wundef -Werror -Wl,-EL -fno-inline-functions -nostdlib -mlongcalls -mtext-section-literals  -D__ets__ -DICACHE_FLASH
LDFLAGS   = -nostdlib -Wl,--no-check-sections -u call_user_start -Wl,-static -L$(SDK_DIR)/ld
LD_SCRIPT = eagle.app.v6.ld

ifeq ($(RBOOT_BIG_FLASH),1)
	CFLAGS += -DBOOT_BIG_FLASH
endif
ifneq ($(RBOOT_DELAY_MICROS),)
	CFLAGS += -DBOOT_DELAY_MICROS=$(RBOOT_DELAY_MICROS)
endif
ifeq ($(RBOOT_INTEGRATION),1)
	CFLAGS += -DRBOOT_INTEGRATION
endif
ifeq ($(RBOOT_RTC_ENABLED),1)
	CFLAGS += -DBOOT_RTC_ENABLED
endif
ifeq ($(RBOOT_CONFIG_CHKSUM),1)
	CFLAGS += -DBOOT_CONFIG_CHKSUM
endif
ifeq ($(RBOOT_GPIO_ENABLED),1)
	CFLAGS += -DBOOT_GPIO_ENABLED
endif
ifeq ($(RBOOT_GPIO_SKIP_ENABLED),1)
	CFLAGS += -DBOOT_GPIO_SKIP_ENABLED
endif
ifneq ($(RBOOT_GPIO_NUMBER),)
	CFLAGS += -DBOOT_GPIO_NUM=$(RBOOT_GPIO_NUMBER)
endif
ifeq ($(RBOOT_IROM_CHKSUM),1)
	CFLAGS += -DBOOT_IROM_CHKSUM
endif
ifneq ($(RBOOT_EXTRA_INCDIR),)
	CFLAGS += $(addprefix -I,$(RBOOT_EXTRA_INCDIR))
endif
CFLAGS += $(addprefix -I,.)

ifeq ($(SPI_SIZE), 256K)
	E2_OPTS += -256
else ifeq ($(SPI_SIZE), 512K)
	E2_OPTS += -512
else ifeq ($(SPI_SIZE), 1M)
	E2_OPTS += -1024
else ifeq ($(SPI_SIZE), 2M)
	E2_OPTS += -2048
else ifeq ($(SPI_SIZE), 4M)
	E2_OPTS += -4096
endif
ifeq ($(SPI_MODE), qio)
	E2_OPTS += -qio
else ifeq ($(SPI_MODE), dio)
	E2_OPTS += -dio
else ifeq ($(SPI_MODE), qout)
	E2_OPTS += -qout
else ifeq ($(SPI_MODE), dout)
	E2_OPTS += -dout
endif
ifeq ($(SPI_SPEED), 20)
	E2_OPTS += -20
else ifeq ($(SPI_SPEED), 26)
	E2_OPTS += -26.7
else ifeq ($(SPI_SPEED), 40)
	E2_OPTS += -40
else ifeq ($(SPI_SPEED), 80)
	E2_OPTS += -80
endif

.SECONDARY:

#all: $(RBOOT_BUILD_BASE) $(RBOOT_FW_BASE) $(RBOOT_FW_BASE)/rboot.bin $(RBOOT_FW_BASE)/testload1.bin $(RBOOT_FW_BASE)/testload2.bin
all: $(RBOOT_BUILD_BASE) $(RBOOT_FW_BASE) $(RBOOT_FW_BASE)/rboot.bin

$(RBOOT_BUILD_BASE):
	mkdir -p $@

$(RBOOT_FW_BASE):
	mkdir -p $@

$(RBOOT_BUILD_BASE)/rboot-stage2a.o: rboot-stage2a.c rboot-private.h rboot.h
	@echo "CC $<"
	$(Q) $(CC) $(CFLAGS) -c $< -o $@

$(RBOOT_BUILD_BASE)/rboot-stage2a.elf: $(RBOOT_BUILD_BASE)/rboot-stage2a.o
	@echo "LD $@"
	$(Q) $(LD) -Trboot-stage2a.ld $(LDFLAGS) -Wl,--start-group $^ -Wl,--end-group -o $@

$(RBOOT_BUILD_BASE)/rboot-hex2a.h: $(RBOOT_BUILD_BASE)/rboot-stage2a.elf
	@echo "HDR $@"
	$(Q) rm -f $@
	$(Q) $(OBJDUMP) -f $< | grep 'start address' | awk '{print "const uint32 entry_addr = " $$3 ";"}' >> $@
	$(Q) $(OBJDUMP) -j .text -h $< | grep ' .text ' | awk '{print "const uint32 _text_addr = 0x" $$4 ";"}' >> $@
	$(Q) $(OBJDUMP) -h $< | grep ' .text ' | awk '{print "const uint32 _text_len  = 0x" $$3 ";"}' >> $@
	$(Q) echo 'const uint8  _text_data[] = {' >> $@
	$(Q) $(OBJCOPY) --dump-section .text=/dev/stdout $< | xxd -i >> $@
	$(Q) echo '};' >> $@

$(RBOOT_BUILD_BASE)/rboot.o: rboot.c rboot-private.h rboot.h $(RBOOT_BUILD_BASE)/rboot-hex2a.h
	@echo "CC $<"
	$(Q) $(CC) $(CFLAGS) -I$(RBOOT_BUILD_BASE) -c $< -o $@

$(RBOOT_BUILD_BASE)/%.o: %.c %.h
	@echo "CC $<"
	$(Q) $(CC) $(CFLAGS) -c $< -o $@

$(RBOOT_BUILD_BASE)/%.elf: $(RBOOT_BUILD_BASE)/%.o
	@echo "LD $@"
	$(Q) $(LD) -T$(LD_SCRIPT) $(LDFLAGS) -Wl,--start-group $^ -Wl,--end-group -o $@

$(RBOOT_FW_BASE)/%.bin: $(RBOOT_BUILD_BASE)/%.elf
	@echo "ET $@"
	$(Q) esptool.py elf2image -o $@ -e 1 $<
	$(Q) mv $@0x00000.bin $@
	$(Q) rm -f $@*.bin

clean:
	@echo "RM $(RBOOT_BUILD_BASE) $(RBOOT_FW_BASE)"
	$(Q) rm -rf $(RBOOT_BUILD_BASE)
	$(Q) rm -rf $(RBOOT_FW_BASE)
