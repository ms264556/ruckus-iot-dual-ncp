# AP-side programs, cross-built as static ARMv7 binaries.
#
#   make CPC_INC=<cpc-daemon>/lib CPC_LIB=<build>/libcpc.a     iotd, ncpflash, ezsp_probe
#   make test                                                  ASH codec unit test (native gcc)
#
# scripts/build-ap.sh builds cpc-daemon first and passes the paths.

CROSS        ?= arm-linux-gnueabi-
CC            = $(CROSS)gcc
HOSTCC       ?= gcc
CPC_INC      ?= ../third_party/cpc-daemon/lib
CPC_LIB      ?= ../build/cpc-daemon/libcpc.a
CFLAGS       ?= -O2 -Wall -Wextra -Wno-unused-parameter
EXTRA_CFLAGS ?=

IOTD_SRCS = iotd/main.c iotd/log.c iotd/config.c iotd/net.c iotd/cpcd.c iotd/link.c \
            iotd/ble.c iotd/zb.c iotd/ash_codec.c
IOTD_HDRS = iotd/iotd.h iotd/ash_codec.h

all: iotd/iotd tools/ncpflash tools/ezsp_probe

iotd/iotd: $(IOTD_SRCS) $(IOTD_HDRS)
	$(CC) -static $(CFLAGS) $(EXTRA_CFLAGS) -I$(CPC_INC) -o $@ $(IOTD_SRCS) $(CPC_LIB) -lpthread

tools/ncpflash: tools/ncpflash.c
	$(CC) -static $(CFLAGS) $(EXTRA_CFLAGS) -o $@ $<

tools/ezsp_probe: tools/ezsp_probe.c
	$(CC) -static $(CFLAGS) $(EXTRA_CFLAGS) -I$(CPC_INC) -o $@ $< $(CPC_LIB) -lpthread

test: test/ash_codec_test
	./test/ash_codec_test

test/ash_codec_test: test/ash_codec_test.c iotd/ash_codec.c iotd/ash_codec.h
	$(HOSTCC) $(CFLAGS) -Iiotd -o $@ test/ash_codec_test.c iotd/ash_codec.c

clean:
	rm -f iotd/iotd tools/ncpflash tools/ezsp_probe test/ash_codec_test

.PHONY: all test clean
