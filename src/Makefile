# mbproxy - Modbus TCP -> RTU gateway for the pCOWeb card
#
#   make            card binary build/mbproxy (freestanding, OABI-patched) + host binary
#   make test       C unit tests + gateway tests against the simulated pCO
#   make hello      build/hello: "hello world" for the card, to check toolchain and upload first
#   make clean

CORE      = mb_core.c mbproxy.c
HEADERS   = mb_core.h plat.h
BUILD     = build

HOSTCC    ?= cc
HOSTFLAGS = -std=c99 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -O2 -g

ARMCC     = arm-none-eabi-gcc
# ARM720T = ARMv4T, ARM mode. No libc: only libgcc for integer division.
ARMFLAGS  = -mcpu=arm7tdmi -marm -Os -ffreestanding -fno-builtin -fno-pic -fno-stack-protector \
            -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
            -Wall -Wextra
ARMLINK   = -nostdlib -static -Wl,--gc-sections -Wl,--build-id=none -lgcc

all: $(BUILD)/mbproxy $(BUILD)/mbproxy-host

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/mbproxy: $(CORE) plat_pcoweb.c $(HEADERS) fixelf.py | $(BUILD)
	$(ARMCC) $(ARMFLAGS) -o $@ $(CORE) plat_pcoweb.c $(ARMLINK)
	python3 -I fixelf.py $@
	arm-none-eabi-size $@

$(BUILD)/hello: hello.c fixelf.py | $(BUILD)
	$(ARMCC) $(ARMFLAGS) -o $@ hello.c $(ARMLINK)
	python3 -I fixelf.py $@

hello: $(BUILD)/hello

$(BUILD)/mbproxy-host: $(CORE) plat_posix.c $(HEADERS) | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -o $@ $(CORE) plat_posix.c

$(BUILD)/test_core: test/test_core.c mb_core.c mb_core.h | $(BUILD)
	$(HOSTCC) $(HOSTFLAGS) -I. -o $@ test/test_core.c mb_core.c

test: $(BUILD)/test_core $(BUILD)/mbproxy-host
	$(BUILD)/test_core
	python3 test/test_gateway.py $(BUILD)/mbproxy-host

clean:
	rm -rf $(BUILD)

.PHONY: all test clean hello
