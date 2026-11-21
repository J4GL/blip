# blip — `./build.sh` runs this in the blip-build images; `./install.sh` runs it natively.
CC      ?= gcc
WARN    := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wconversion \
           -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Wvla -Werror
REL     := -O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE
TESTF   := -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
SRC     := src/config.c src/util.c src/procfs.c src/sampler.c src/colstore.c src/detect.c \
           src/nginxlog.c src/probe.c src/report.c src/health.c
HDR     := $(wildcard src/*.h)
TESTS   := $(wildcard tests/*.c)

.PHONY: all test test-ubsan clean
all: dist/blip

dist/blip: $(SRC) src/main.c $(HDR)
	@mkdir -p dist
	$(CC) $(WARN) $(REL) -static-pie -o $@ $(SRC) src/main.c -l:libzstd.a
	strip $@

.build/run: $(SRC) $(HDR) $(TESTS) tests/t.h
	@mkdir -p .build
	$(CC) $(WARN) $(TESTF) -Isrc -o $@ $(SRC) $(TESTS) -lzstd

test: .build/run
	./.build/run $(FILTER)

# x86_64 under Rosetta: AddressSanitizer cannot run there, UBSan can.
.build/run-ubsan: $(SRC) $(HDR) $(TESTS) tests/t.h
	@mkdir -p .build
	$(CC) $(WARN) -O1 -g -fsanitize=undefined -fno-sanitize-recover=all -Isrc -o $@ $(SRC) $(TESTS) -lzstd

test-ubsan: .build/run-ubsan
	./.build/run-ubsan $(FILTER)

# Same release build for the host CPU (budget tests without the x86 emulator)
.build/blip-native: $(SRC) src/main.c $(HDR)
	@mkdir -p .build
	$(CC) $(WARN) $(REL) -static-pie -o $@ $(SRC) src/main.c -l:libzstd.a

clean:
	rm -rf .build dist
