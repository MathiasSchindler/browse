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

CORE_SRCS := \
	src/core/start.S \
	src/core/main.c \
	src/core/demo_frame.c \
	src/core/backend_shm.c

ifeq ($(BACKEND),fbdev)
CORE_SRCS += src/core/backend_fbdev.c
endif
CORE_BIN := build/core

INPUTD_SRCS := src/core/start.S src/inputd/main.c
INPUTD_BIN := build/inputd

TLS_SRCS := \
	src/tls/sha256.c \
	src/tls/hmac_sha256.c \
	src/tls/hkdf_sha256.c \
	src/tls/tls13_kdf.c \
	src/tls/x25519.c \
	src/tls/aes128.c \
	src/tls/gcm.c \
	src/tls/selftest.c

FONTGEN_BIN := build/fontgen
FONTGEN_SRCS := tools/fontgen.c

FONT_BUILTIN_8X8 := src/core/font/font_builtin_8x8.c
FONT_BUILTIN_8X16 := src/core/font/font_builtin_8x16.c
FONT_STAMP := build/fonts.stamp

FONT_SRCS := src/core/font/font_render.c $(FONT_BUILTIN_8X8) $(FONT_BUILTIN_8X16)

BROWSER_SRCS := src/core/start.S src/browser/main.c src/browser/browser_img.c src/browser/browser_nav.c src/browser/browser_ui.c src/browser/http.c src/browser/tls13_client.c src/browser/html_text.c src/browser/text_layout.c src/browser/style_attr.c src/browser/css_tiny.c src/browser/image/jpeg.c src/browser/image/jpeg_decode.c src/browser/image/png.c src/browser/image/png_decode.c src/browser/image/gif.c src/browser/image/gif_decode.c $(TLS_SRCS) $(FONT_SRCS)
BROWSER_BIN := build/browser
BROWSER_CFLAGS := $(CORE_CFLAGS) -DTEXT_LOG_MISSING_GLYPHS

.PHONY: all core browser inputd tests test test-crypto test-net-ipv6 test-http test-text-layout test-links test-style-attr test-spans test-css-parser test-text-font clean clean-all viewer audit
.PHONY: fontgen fonts
.PHONY: test-x25519
.PHONY: test-http

.PHONY: FORCE
FORCE:

HAVE_SDL2 := $(shell pkg-config --exists sdl2 && echo 1)

ifeq ($(HAVE_SDL2),1)
all: fontgen fonts core browser inputd tests viewer
else
all: fontgen fonts core browser inputd tests
endif

build:
	@mkdir -p build

fontgen: build $(FONTGEN_BIN)

$(FONTGEN_BIN): $(FONTGEN_SRCS)
	$(CC) $(CFLAGS_COMMON) -o $@ $(FONTGEN_SRCS)

$(FONTGEN_BIN): FORCE

fonts: $(FONT_BUILTIN_8X8) $(FONT_BUILTIN_8X16)

$(FONT_STAMP): build $(FONTGEN_BIN) $(FONTGEN_SRCS)
	@mkdir -p src/core/font
	./$(FONTGEN_BIN) --out-dir src/core/font
	@touch $@

$(FONT_BUILTIN_8X8) $(FONT_BUILTIN_8X16): $(FONT_STAMP)
	@true

core: build $(CORE_BIN)

$(CORE_BIN): $(CORE_SRCS) $(FONT_SRCS)
	$(CC) $(CORE_CFLAGS) $(CORE_LDFLAGS) -o $@ $(CORE_SRCS) $(FONT_SRCS)
	$(POST_LINK)

$(CORE_BIN): FORCE

browser: build $(BROWSER_BIN)

$(BROWSER_BIN): $(BROWSER_SRCS)
	$(CC) $(BROWSER_CFLAGS) $(CORE_LDFLAGS) -o $@ $(BROWSER_SRCS)
	$(POST_LINK)

$(BROWSER_BIN): FORCE

inputd: build $(INPUTD_BIN)

$(INPUTD_BIN): $(INPUTD_SRCS)
	$(CC) $(CORE_CFLAGS) $(CORE_LDFLAGS) -o $@ $(INPUTD_SRCS)
	$(POST_LINK)

$(INPUTD_BIN): FORCE

TEST_CRYPTO_BIN := build/test_crypto
TEST_NET_IPV6_BIN := build/test_net_ipv6
TEST_X25519_BIN := build/test_x25519
TEST_HTTP_BIN := build/test_http
TEST_HTTP_PARSE_BIN := build/test_http_parse
TEST_CHUNKED_BIN := build/test_chunked
TEST_VISIBLE_TEXT_BIN := build/test_visible_text
TEST_TEXT_LAYOUT_BIN := build/test_text_layout
TEST_LINKS_BIN := build/test_links
TEST_STYLE_ATTR_BIN := build/test_style_attr
TEST_SPANS_BIN := build/test_spans
TEST_CSS_PARSER_BIN := build/test_css_parser
TEST_TEXT_FONT_BIN := build/test_text_font
TEST_REDIRECT_BIN := build/test_redirect
TEST_JPEG_HEADER_BIN := build/test_jpeg_header
TEST_PNG_HEADER_BIN := build/test_png_header
TEST_GIF_HEADER_BIN := build/test_gif_header
TEST_GIF_DECODE_BIN := build/test_gif_decode
TEST_JPEG_DECODE_BIN := build/test_jpeg_decode
TEST_PNG_DECODE_BIN := build/test_png_decode

# Build (but do not run) all test binaries.
tests: build $(TEST_CRYPTO_BIN) $(TEST_NET_IPV6_BIN) $(TEST_HTTP_BIN) $(TEST_HTTP_PARSE_BIN) $(TEST_CHUNKED_BIN) $(TEST_VISIBLE_TEXT_BIN) $(TEST_TEXT_LAYOUT_BIN) $(TEST_LINKS_BIN) $(TEST_STYLE_ATTR_BIN) $(TEST_SPANS_BIN) $(TEST_CSS_PARSER_BIN) $(TEST_TEXT_FONT_BIN) $(TEST_X25519_BIN) $(TEST_REDIRECT_BIN) $(TEST_JPEG_HEADER_BIN) $(TEST_PNG_HEADER_BIN) $(TEST_GIF_HEADER_BIN) $(TEST_GIF_DECODE_BIN) $(TEST_JPEG_DECODE_BIN) $(TEST_PNG_DECODE_BIN)

test: test-crypto test-net-ipv6 test-http test-http-parse test-chunked test-visible-text test-text-layout test-links test-style-attr test-spans test-css-parser test-text-font test-redirect test-jpeg-header test-png-header test-gif-header test-gif-decode test-jpeg-decode test-png-decode

test-png-decode: build $(TEST_PNG_DECODE_BIN)
	./$(TEST_PNG_DECODE_BIN)

$(TEST_PNG_DECODE_BIN): tools/test_png_decode.c src/browser/image/png_decode.c src/browser/image/png_decode.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_png_decode.c src/browser/image/png_decode.c

$(TEST_PNG_DECODE_BIN): FORCE

test-jpeg-decode: build $(TEST_JPEG_DECODE_BIN)
	./$(TEST_JPEG_DECODE_BIN)

$(TEST_JPEG_DECODE_BIN): tools/test_jpeg_decode.c src/browser/image/jpeg_decode.c src/browser/image/jpeg_decode.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_jpeg_decode.c src/browser/image/jpeg_decode.c

$(TEST_JPEG_DECODE_BIN): FORCE

test-gif-decode: build $(TEST_GIF_DECODE_BIN)
	./$(TEST_GIF_DECODE_BIN)

$(TEST_GIF_DECODE_BIN): tools/test_gif_decode.c src/browser/image/gif_decode.c src/browser/image/gif_decode.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_gif_decode.c src/browser/image/gif_decode.c

$(TEST_GIF_DECODE_BIN): FORCE

test-png-header: build $(TEST_PNG_HEADER_BIN)
	./$(TEST_PNG_HEADER_BIN)

$(TEST_PNG_HEADER_BIN): tools/test_png_header.c src/browser/image/png.c src/browser/image/png.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_png_header.c src/browser/image/png.c

$(TEST_PNG_HEADER_BIN): FORCE

test-gif-header: build $(TEST_GIF_HEADER_BIN)
	./$(TEST_GIF_HEADER_BIN)

$(TEST_GIF_HEADER_BIN): tools/test_gif_header.c src/browser/image/gif.c src/browser/image/gif.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_gif_header.c src/browser/image/gif.c

$(TEST_GIF_HEADER_BIN): FORCE

test-jpeg-header: build $(TEST_JPEG_HEADER_BIN)
	./$(TEST_JPEG_HEADER_BIN)

$(TEST_JPEG_HEADER_BIN): tools/test_jpeg_header.c src/browser/image/jpeg.c src/browser/image/jpeg.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_jpeg_header.c src/browser/image/jpeg.c

$(TEST_JPEG_HEADER_BIN): FORCE

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

test-http-parse: build $(TEST_HTTP_PARSE_BIN)
	./$(TEST_HTTP_PARSE_BIN)

$(TEST_HTTP_PARSE_BIN): tools/test_http_parse.c src/browser/http_parse.h src/browser/url.h src/browser/util.h src/core/syscall.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_http_parse.c

$(TEST_HTTP_PARSE_BIN): FORCE

test-chunked: build $(TEST_CHUNKED_BIN)
	./$(TEST_CHUNKED_BIN)

$(TEST_CHUNKED_BIN): tools/test_chunked.c src/browser/http_parse.h src/browser/url.h src/browser/util.h src/core/syscall.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_chunked.c

$(TEST_CHUNKED_BIN): FORCE

test-visible-text: build $(TEST_VISIBLE_TEXT_BIN)
	./$(TEST_VISIBLE_TEXT_BIN)

$(TEST_VISIBLE_TEXT_BIN): tools/test_visible_text.c src/browser/html_text.c src/browser/html_text.h src/browser/util.h src/core/syscall.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_visible_text.c src/browser/html_text.c src/browser/style_attr.c src/browser/css_tiny.c

$(TEST_VISIBLE_TEXT_BIN): FORCE

test-text-layout: build $(TEST_TEXT_LAYOUT_BIN)
	./$(TEST_TEXT_LAYOUT_BIN)

$(TEST_TEXT_LAYOUT_BIN): tools/test_text_layout.c src/browser/text_layout.c src/browser/text_layout.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_text_layout.c src/browser/text_layout.c

$(TEST_TEXT_LAYOUT_BIN): FORCE

test-links: build $(TEST_LINKS_BIN)
	./$(TEST_LINKS_BIN)

$(TEST_LINKS_BIN): tools/test_links.c src/browser/html_text.c src/browser/html_text.h src/browser/util.h src/core/syscall.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_links.c src/browser/html_text.c src/browser/style_attr.c src/browser/css_tiny.c

$(TEST_LINKS_BIN): FORCE

test-style-attr: build $(TEST_STYLE_ATTR_BIN)
	./$(TEST_STYLE_ATTR_BIN)

$(TEST_STYLE_ATTR_BIN): tools/test_style_attr.c src/browser/style_attr.c src/browser/style_attr.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_style_attr.c src/browser/style_attr.c

$(TEST_STYLE_ATTR_BIN): FORCE

test-spans: build $(TEST_SPANS_BIN)
	./$(TEST_SPANS_BIN)

$(TEST_SPANS_BIN): tools/test_spans.c src/browser/html_text.c src/browser/html_text.h src/browser/style_attr.c src/browser/style_attr.h src/browser/util.h src/core/syscall.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_spans.c src/browser/html_text.c src/browser/style_attr.c src/browser/css_tiny.c

$(TEST_SPANS_BIN): FORCE

test-css-parser: build $(TEST_CSS_PARSER_BIN)
	./$(TEST_CSS_PARSER_BIN)

$(TEST_CSS_PARSER_BIN): tools/test_css_parser.c src/browser/css_tiny.c src/browser/css_tiny.h src/browser/style_attr.c src/browser/style_attr.h
	$(CC) $(CFLAGS_COMMON) -Isrc -o $@ tools/test_css_parser.c src/browser/css_tiny.c src/browser/style_attr.c

$(TEST_CSS_PARSER_BIN): FORCE

test-text-font: build $(TEST_TEXT_FONT_BIN)
	./$(TEST_TEXT_FONT_BIN)

$(TEST_TEXT_FONT_BIN): tools/test_text_font.c src/core/start.S src/core/text.h src/core/syscall.h
	$(CC) $(CORE_CFLAGS) $(CORE_LDFLAGS) -o $@ src/core/start.S tools/test_text_font.c $(FONT_SRCS)

$(TEST_TEXT_FONT_BIN): FORCE

test-redirect: build $(TEST_REDIRECT_BIN)
	./$(TEST_REDIRECT_BIN)

$(TEST_REDIRECT_BIN): tools/test_redirect.c src/core/start.S src/core/syscall.h src/browser/util.h src/browser/url.h
	$(CC) $(CORE_CFLAGS) $(CORE_LDFLAGS) -Isrc -o $@ src/core/start.S tools/test_redirect.c

$(TEST_REDIRECT_BIN): FORCE

# Optional dev viewer: needs libsdl2-dev
VIEWER_BIN := build/viewer
viewer: build $(VIEWER_BIN)

$(VIEWER_BIN): tools/viewer_sdl.c
	$(CC) $(CFLAGS_COMMON) -o $@ $< $(shell pkg-config --cflags --libs sdl2)

audit:
	@chmod +x tools/audit_no_external.sh
	@./tools/audit_no_external.sh

clean:
	rm -f $(CORE_BIN) $(CORE_BIN).debug
	rm -f $(BROWSER_BIN) $(BROWSER_BIN).debug
	rm -f $(INPUTD_BIN) $(INPUTD_BIN).debug
	rm -f $(TEST_CRYPTO_BIN) $(TEST_NET_IPV6_BIN) $(TEST_HTTP_BIN) $(TEST_HTTP_PARSE_BIN) $(TEST_CHUNKED_BIN) $(TEST_VISIBLE_TEXT_BIN) $(TEST_X25519_BIN) $(TEST_TEXT_FONT_BIN) $(TEST_REDIRECT_BIN)
	rm -f build/*.debug
	rm -f $(FONTGEN_BIN)
	rm -f $(FONT_STAMP)
	rm -f $(VIEWER_BIN)
	@true

clean-all:
	rm -rf build
