# dcmon - lightweight Linux system monitor
CC       ?= gcc
PREFIX   ?= /usr/local
CPPFLAGS += -D_GNU_SOURCE -Isrc
STD      := -std=c17
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wno-format-nonliteral

SRC      := $(wildcard src/*.c)
HDR      := $(wildcard src/*.h)
LIB_SRC  := $(filter-out src/main.c,$(SRC))

# Size-optimised release build: LTO, dead-section GC, no unwind tables,
# stripped.  Stack protector and FORTIFY stay on for stability.
RELEASE_CFLAGS  := $(STD) -Os -flto -s $(WARN) -D_FORTIFY_SOURCE=2 \
                   -ffunction-sections -fdata-sections \
                   -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-ident
RELEASE_LDFLAGS := -flto -s -Wl,--gc-sections -Wl,-O1 -Wl,--as-needed -Wl,--hash-style=gnu \
                   -Wl,--build-id=none

DEBUG_CFLAGS    := $(STD) -O0 -g3 $(WARN) -fsanitize=address,undefined -fno-omit-frame-pointer
DEBUG_LDFLAGS   := -fsanitize=address,undefined

.PHONY: all release debug test bench clean install uninstall

all: release

release: dcmon

dcmon: $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(RELEASE_CFLAGS) $(SRC) -o $@ $(RELEASE_LDFLAGS)

debug: dcmon-debug

dcmon-debug: $(SRC) $(HDR)
	$(CC) $(CPPFLAGS) $(DEBUG_CFLAGS) $(SRC) -o $@ $(DEBUG_LDFLAGS)

build/tests: $(LIB_SRC) $(HDR) tests/test.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(DEBUG_CFLAGS) $(LIB_SRC) tests/test.c -o $@ $(DEBUG_LDFLAGS)

test: build/tests dcmon-debug
	ASAN_OPTIONS=detect_leaks=1 ./build/tests ./dcmon-debug

build/sccount: tests/sccount.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(STD) -O2 $(WARN) tests/sccount.c -o $@

bench: dcmon build/sccount
	sh tests/bench.sh ./dcmon

install: dcmon
	install -D -m 0755 dcmon $(DESTDIR)$(PREFIX)/bin/dcmon

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dcmon

clean:
	rm -rf dcmon dcmon-debug build
