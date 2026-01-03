CC ?= gcc

# Build toggles:
# - SIZE=1 enables size-oriented optimizations (-Os + section GC).
# - STRIP=1 splits debug info into a separate file and strips it from the binary.
SIZE ?= 0
STRIP ?= 0

OBJCOPY ?= objcopy
STRIPBIN ?= strip

OPTFLAGS := -O2
ifeq ($(SIZE),1)
OPTFLAGS := -Os
endif

CFLAGS_COMMON := -Wall -Wextra -Werror $(OPTFLAGS) -g

BACKEND ?= shm

CORE_BACKEND_CFLAGS :=
ifeq ($(BACKEND),shm)
CORE_BACKEND_CFLAGS += -DBACKEND_SHM
else ifeq ($(BACKEND),fbdev)
CORE_BACKEND_CFLAGS += -DBACKEND_FBDEV
else
$(error Unknown BACKEND=$(BACKEND) (use shm or fbdev))
endif

CORE_CFLAGS := $(CFLAGS_COMMON) $(CORE_BACKEND_CFLAGS) \
	-ffreestanding -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-pie -no-pie
CORE_LDFLAGS := -nostdlib -nodefaultlibs -nostartfiles -Wl,-e,_start -no-pie

ifeq ($(SIZE),1)
CORE_CFLAGS += -ffunction-sections -fdata-sections
CORE_LDFLAGS += -Wl,--gc-sections
endif

POST_LINK :=
ifeq ($(STRIP),1)
POST_LINK = \
	$(OBJCOPY) --only-keep-debug $@ $@.debug && \
	$(STRIPBIN) --strip-debug $@ && \
	$(OBJCOPY) --add-gnu-debuglink=$@.debug $@
endif

CORE_SRCS := src/core/start.S src/core/main.c
CORE_BIN := build/core

TLS_SRCS := \
	src/tls/sha256.c \
	src/tls/hmac_sha256.c \
	src/tls/hkdf_sha256.c \
	src/tls/tls13_kdf.c \
	src/tls/x25519.c \
	src/tls/aes128.c \
	src/tls/gcm.c \
	src/tls/selftest.c

BROWSER_SRCS := src/core/start.S src/browser/main.c src/browser/http.c src/browser/tls13_client.c $(TLS_SRCS)
BROWSER_BIN := build/browser
BROWSER_CFLAGS := $(CORE_CFLAGS) -DTEXT_LOG_MISSING_GLYPHS

.PHONY: all core browser tests test test-crypto test-net-ipv6 test-http test-text-font clean clean-all viewer
.PHONY: test-x25519
.PHONY: test-http

.PHONY: FORCE
FORCE:

HAVE_SDL2 := $(shell pkg-config --exists sdl2 && echo 1)

ifeq ($(HAVE_SDL2),1)
all: core browser tests viewer
else
all: core browser tests
endif

build:
	@mkdir -p build

core: build $(CORE_BIN)

$(CORE_BIN): $(CORE_SRCS)
	$(CC) $(CORE_CFLAGS) $(CORE_LDFLAGS) -o $@ $(CORE_SRCS)
	$(POST_LINK)

browser: build $(BROWSER_BIN)

$(BROWSER_BIN): $(BROWSER_SRCS)
	$(CC) $(BROWSER_CFLAGS) $(CORE_LDFLAGS) -o $@ $(BROWSER_SRCS)
	$(POST_LINK)

TEST_CRYPTO_BIN := build/test_crypto
TEST_NET_IPV6_BIN := build/test_net_ipv6
TEST_X25519_BIN := build/test_x25519
TEST_HTTP_BIN := build/test_http
TEST_TEXT_FONT_BIN := build/test_text_font

# Build (but do not run) all test binaries.
tests: build $(TEST_CRYPTO_BIN) $(TEST_NET_IPV6_BIN) $(TEST_HTTP_BIN) $(TEST_TEXT_FONT_BIN) $(TEST_X25519_BIN)

test: test-crypto test-net-ipv6 test-http test-text-font

test-x25519: build $(TEST_X25519_BIN)
	./$(TEST_X25519_BIN)

$(TEST_X25519_BIN): tools/test_x25519.c src/tls/x25519.c
	$(CC) $(CFLAGS_COMMON) -o $@ tools/test_x25519.c src/tls/x25519.c

test-crypto: build $(TEST_CRYPTO_BIN)
	./$(TEST_CRYPTO_BIN)

$(TEST_CRYPTO_BIN): tools/test_crypto.c $(TLS_SRCS)
	$(CC) $(CFLAGS_COMMON) -o $@ tools/test_crypto.c $(TLS_SRCS)

$(TEST_CRYPTO_BIN): FORCE

test-net-ipv6: build $(TEST_NET_IPV6_BIN)
	./$(TEST_NET_IPV6_BIN)

$(TEST_NET_IPV6_BIN): tools/test_net_ipv6.c
	$(CC) $(CFLAGS_COMMON) -o $@ $<

$(TEST_NET_IPV6_BIN): FORCE

test-http: build $(TEST_HTTP_BIN)
	./$(TEST_HTTP_BIN)

$(TEST_HTTP_BIN): tools/test_http.c src/browser/http.c src/browser/http.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_http.c src/browser/http.c

$(TEST_HTTP_BIN): FORCE

test-text-font: build $(TEST_TEXT_FONT_BIN)
	./$(TEST_TEXT_FONT_BIN)

$(TEST_TEXT_FONT_BIN): tools/test_text_font.c src/core/start.S src/core/text.h src/core/syscall.h
	$(CC) $(CORE_CFLAGS) $(CORE_LDFLAGS) -o $@ src/core/start.S tools/test_text_font.c

$(TEST_TEXT_FONT_BIN): FORCE

# Optional dev viewer: needs libsdl2-dev
VIEWER_BIN := build/viewer
viewer: build $(VIEWER_BIN)

$(VIEWER_BIN): tools/viewer_sdl.c
	$(CC) $(CFLAGS_COMMON) -o $@ $< $(shell pkg-config --cflags --libs sdl2)

clean:
	rm -f $(CORE_BIN) $(CORE_BIN).debug
	rm -f $(BROWSER_BIN) $(BROWSER_BIN).debug
	rm -f $(TEST_CRYPTO_BIN) $(TEST_NET_IPV6_BIN) $(TEST_HTTP_BIN) $(TEST_X25519_BIN) $(TEST_TEXT_FONT_BIN)
	rm -f build/*.debug
	rm -f $(VIEWER_BIN)
	@true

clean-all:
	rm -rf build
