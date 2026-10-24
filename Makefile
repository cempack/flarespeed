PROG    = flarespeed
VERSION = 1.0.1

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man

CC         ?= cc
PKG_CONFIG ?= pkg-config
INSTALL    ?= install

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS   := $(shell $(PKG_CONFIG) --libs libcurl)

CFLAGS ?= -O2 -g
WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wformat=2 -Wundef -Wcast-qual -Wvla \
           -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough
# -ffp-contract=off keeps float results identical across compilers and
# architectures: the color math must not be fused into FMA instructions.
BUILD_CFLAGS   = -std=c11 $(WARNINGS) -ffp-contract=off $(CURL_CFLAGS) $(CFLAGS)
BUILD_CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
                 -DFLARESPEED_VERSION='"$(VERSION)"' $(CPPFLAGS)
LIBS = $(CURL_LIBS) -lm

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)
LIB_OBJS = $(filter-out src/main.o,$(OBJS))

TEST_SRCS = $(wildcard tests/test_*.c)
TEST_OBJS = $(TEST_SRCS:.c=.o)

FUZZ_CC     = clang
FUZZ_CFLAGS = -std=c11 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
              -ffp-contract=off $(BUILD_CPPFLAGS) -Isrc
FUZZ_SRCS   = $(filter-out src/main.c src/app.c src/term.c src/engine.c,$(SRCS))
FUZZERS     = $(patsubst %.c,%,$(wildcard tests/fuzz/fuzz_*.c))

.PHONY: all check check-integration fuzz clean install uninstall

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(BUILD_CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(BUILD_CPPFLAGS) $(BUILD_CFLAGS) -Isrc -MMD -MP -c -o $@ $<

tests/run_tests: $(LIB_OBJS) $(TEST_OBJS) tests/harness.o
	$(CC) $(BUILD_CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

check: tests/run_tests
	cd tests && ./run_tests

# End-to-end tests against a local mock server; needs python3.
check-integration: $(PROG)
	python3 tests/integration.py ./$(PROG)

# libFuzzer targets; needs clang.
fuzz: $(FUZZERS)

tests/fuzz/fuzz_%: tests/fuzz/fuzz_%.c $(FUZZ_SRCS)
	$(FUZZ_CC) $(FUZZ_CFLAGS) -o $@ $< $(FUZZ_SRCS) -lm

install: $(PROG)
	$(INSTALL) -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)/man1
	$(INSTALL) -m 755 $(PROG) $(DESTDIR)$(BINDIR)/$(PROG)
	$(INSTALL) -m 644 $(PROG).1 $(DESTDIR)$(MANDIR)/man1/$(PROG).1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROG) $(DESTDIR)$(MANDIR)/man1/$(PROG).1

clean:
	rm -f $(PROG) src/*.o src/*.d tests/*.o tests/*.d tests/run_tests $(FUZZERS)

-include $(OBJS:.o=.d) $(TEST_OBJS:.o=.d) tests/harness.d
