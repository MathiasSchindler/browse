CC ?= gcc
CFLAGS_COMMON := -Wall -Wextra -Werror -O2 -g

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

CORE_SRCS := src/core/start.S src/core/main.c
CORE_BIN := build/core

TLS_SRCS := \
	src/tls/sha256.c \
	src/tls/hmac_sha256.c \
	src/tls/hkdf_sha256.c \
	src/tls/x25519.c \
	src/tls/aes128.c \
	src/tls/gcm.c \
	src/tls/selftest.c

BROWSER_SRCS := src/core/start.S src/browser/main.c $(TLS_SRCS)
BROWSER_BIN := build/browser
BROWSER_CFLAGS := $(CORE_CFLAGS) -DTEXT_LOG_MISSING_GLYPHS

.PHONY: all core browser test test-crypto clean clean-all viewer
.PHONY: test-x25519

.PHONY: FORCE
FORCE:

all: core

build:
	@mkdir -p build

core: build $(CORE_BIN)

$(CORE_BIN): $(CORE_SRCS)
	$(CC) $(CORE_CFLAGS) $(CORE_LDFLAGS) -o $@ $(CORE_SRCS)

browser: build $(BROWSER_BIN)

$(BROWSER_BIN): $(BROWSER_SRCS)
	$(CC) $(BROWSER_CFLAGS) $(CORE_LDFLAGS) -o $@ $(BROWSER_SRCS)

TEST_CRYPTO_BIN := build/test_crypto
TEST_NET_IPV6_BIN := build/test_net_ipv6
TEST_X25519_BIN := build/test_x25519

test: test-crypto test-net-ipv6

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

# Optional dev viewer: needs libsdl2-dev
VIEWER_BIN := build/viewer
viewer: build $(VIEWER_BIN)

$(VIEWER_BIN): tools/viewer_sdl.c
	$(CC) $(CFLAGS_COMMON) -o $@ $< $(shell pkg-config --cflags --libs sdl2)

clean:
	rm -f $(CORE_BIN)
	rm -f $(BROWSER_BIN)
	rm -f $(TEST_CRYPTO_BIN) $(TEST_NET_IPV6_BIN)
	@true

clean-all:
	rm -rf build
