
export FW_VER := 1.9

PROJ = FF_OSD
VER := v$(FW_VER)

SUBDIRS += src

.PHONY: all clean dist flash flash2 dfu start serial

ifneq ($(RULES_MK),y)

export ROOT := $(CURDIR)

all:
	$(MAKE) -C src -f $(ROOT)/Rules.mk $(PROJ).elf $(PROJ).bin $(PROJ).hex
debug:
	debug=y $(MAKE) -C src -f $(ROOT)/Rules.mk $(PROJ).elf $(PROJ).bin $(PROJ).hex
clean:
	rm -rf $(PROJ)-$(VER)*
	$(MAKE) -f $(ROOT)/Rules.mk $@

dist: all
	rm -rf $(PROJ)-$(VER)*
	mkdir -p $(PROJ)-$(VER)
	cp -a src/$(PROJ).elf $(PROJ)-$(VER)/$(PROJ)-$(VER).elf
	cp -a src/$(PROJ).bin $(PROJ)-$(VER)/$(PROJ)-$(VER).bin
	cp -a src/$(PROJ).hex $(PROJ)-$(VER)/$(PROJ)-$(VER).hex
	cp -a COPYING $(PROJ)-$(VER)/
	cp -a README.md $(PROJ)-$(VER)/
	cp -a RELEASE_NOTES $(PROJ)-$(VER)/
	zip -r $(PROJ)-$(VER).zip $(PROJ)-$(VER)
	rm -rf $(PROJ)-$(VER)

endif

BAUD=921600
DEV?=/dev/ttyUSB0
CUBE_CLI  ?= /usr/local/STMCubeProgrammer/bin/STM32_Programmer_CLI
ST_UTIL   ?= st-util
ST_FLASH  ?= st-flash

flash2: all
	sudo stm32flash -b $(BAUD) \
	-vw src/$(PROJ).hex $(DEV)

flash: all
	$(ST_FLASH) --reset write src/FF_OSD.bin 0x08000000

erase:
	$(ST_FLASH) --connect-under-reset erase

stlink:
	$(ST_UTIL) $(ST_ARGS) --no-reset

gdb:
	gdb -q -x .gdbinit src/$(PROJ).elf

dfu: all
	@echo using DEV=$(DEV) to program via serial DFU
	sudo $(CUBE_CLI) -c port=$(DEV) br=115200 -v -w src/$(PROJ).bin 0x08000000
	sudo $(CUBE_CLI) -c port=$(DEV) br=115200 -g 0x08000000
#	sudo $(CUBE_CLI) -c port=$(DEV) br=115200 -v -w src/$(PROJ).bin 0x08000000 -g 0x08000000

start:
	sudo stm32flash -b $(BAUD) -g 0 $(DEV)

serial:
	sudo miniterm.py $(DEV) 115200
