.DEFAULT_GOAL := build
BOARD ?= jbl_go4/ws9652a/cputop
APP ?= app
BUILD_DIR ?= /tmp/cwbeat-go4
BUILD_ARGS ?=
FLASH_ARGS ?=--port /dev/cu.usbmodem1101

.PHONY: setup build flash
setup:
	@west topdir >/dev/null 2>&1 || west init -l .
	west update
	uv pip install $$(west packages pip)

build:
	west build -p auto -b "$(BOARD)" -d "$(BUILD_DIR)" "$(APP)" $(BUILD_ARGS)

flash:
	west flash --no-rebuild -r jbl-bl -d "$(BUILD_DIR)" $(FLASH_ARGS)
