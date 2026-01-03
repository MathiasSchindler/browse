# Framebuffer HTTPS Browser — Implementation Plan

Constraints / goals
- Target: Ubuntu x86-64, gcc-15
- Output: always render into the shared framebuffer `/dev/shm/cfb0` (viewer stays running)
- Distribution: statically linked, small binaries
- Dependencies: no external libraries for the browser (no OpenSSL, no libcurl, no ICU, etc.)
- Implementation rule: use Linux syscalls wherever possible and avoid the C standard library in the browser code (freestanding, custom `_start`, syscall wrappers)
- Networking requirement: IPv6-only. No IPv4 support, no tunneling (e.g. 6to4/Teredo), and no IPv4 fallback paths.
- Product goal: fast, usable browsing for popular sites (not pixel-perfect)

Dev-only exception
- The windowed framebuffer viewer is allowed to use libc/SDL2 for development convenience. The shipping browser must not depend on them.

Non-goals (initially)
- Full HTML/CSS spec coverage
- Full JS compatibility with modern engines
- Perfect typography, complex text shaping, full i18n
- Video playback / DRM

---

## 0) Architecture decisions (done before coding lots)
1. Process model
   - `viewer` (already exists; dev-only) shows `/dev/shm/cfb0` in a window.
   - `browser` (main deliverable) writes pixels to `/dev/shm/cfb0`.
   - Optional: split into `browser` + `netd` later if it helps debugging; start single-process to keep footprint down.

2. Layering
   - `platform/` syscalls: file, mmap, time, sockets, epoll, threads (optional), randomness.
   - `gfx/` software rendering: surfaces, blits, alpha, clipping, text rasterization.
   - `ui/` minimal browser chrome: URL bar, scroll, link highlight, status, error pages.
   - `net/` DNS + TCP + HTTP/1.1 (then HTTP/2).
   - `tls/` minimal TLS 1.3 client.
   - `parse/` URL, HTML, CSS.
   - `dom/` DOM tree + style tree.
   - `layout/` block/inline layout subset + scrolling.
   - `js/` minimal JS runtime + DOM bindings subset.

3. “Always visible” milestone rule
   - Every milestone must produce something visible in the viewer (even if it’s a debug UI or a partial render).

---

## 1) Framebuffer text baseline (Milestone: readable text in `/dev/shm/cfb0`)
1. Text rendering in the syscall-only renderer
   - Built-in bitmap font initially (ASCII subset) to avoid dependencies.
   - `draw_text()` with newline handling and clipping.
   - Integer-to-string helpers for debug stats (fps, timings).

2. Minimal UI scaffold
   - A top bar + status line rendered as text.
   - Visible cursor and selection highlight (for later URL bar / link focus).

Deliverable
- A syscall-only binary that renders stable, readable text into `/dev/shm/cfb0`.

---

## 2) Build + size discipline (Milestone: reproducible tiny binaries)
1. Add build profiles
   - `make debug`: symbols, asserts, logging.
   - `make release`: `-O2/-Os`, `-ffunction-sections -fdata-sections`, `-Wl,--gc-sections`, strip.
   - `-static` for release (evaluate glibc static size vs. a minimal no-libc runtime; prefer smallest viable approach).

2. Add a size report target
   - `size build/browser` + map file generation.
   - A “budget” table in docs (goal sizes per component).

3. Add a deterministic headless render dump
   - A tool mode that writes a PPM/PNG-like raw file from the framebuffer for regression testing.

Deliverable
- A small, statically linked browser binary with a measurable size budget.

---

## 2) Graphics + text baseline (Milestone: readable text and boxes)
1. Framebuffer integration
   - Keep using `/dev/shm/cfb0` backend as the primary dev target.
   - Add a “real fbdev” backend as a compile-time switch (already started) but treat it as deployment-only.

2. 2D drawing primitives
   - `fill_rect`, `blit`, `draw_line` (optional), clipping rectangles.
   - Alpha blend for UI and later CSS backgrounds.

3. Font rendering strategy (pick one, start simple)
   - Start with a built-in bitmap font for debug + UI.
   - Next: implement a minimal TrueType rasterizer (subset) OR ship a single small font file and parse enough to rasterize glyph outlines.
   - Limit to Latin-1/UTF-8 subset at first.

4. Text layout (minimal)
   - Word wrapping, line breaking at spaces.
   - Basic measuring API: `measure_text()`, `draw_text()`.

Deliverable
- URL bar + status line + a scrollable text pane.

---

## 3) Input + navigation UX (Milestone: interactive shell browser)
1. Input
   - Read keyboard/mouse via `/dev/input/event*` (syscalls only), map to internal events.
   - Support: typing in URL bar, Enter to navigate, PageUp/Down, arrows, mouse wheel.

2. UI states
   - Loading indicator, error page, basic history (back/forward).

3. URL handling
   - Parse `http://`, `https://`, relative links.
   - Minimal URL normalization.

Deliverable
- User can type a URL, load a local mock page, scroll and click links.

---

## 3) Minimal HTTPS to Wikipedia (Milestone: TLS 1.3 + HTTP/1.1 GET to `wikipedia.org`)
Goal
- Fetch and display a readable, text-only version of Wikipedia content over HTTPS using only syscalls + in-tree code.

1. Networking prerequisites (syscalls-only)
   - DNS resolver: parse `/etc/resolv.conf`, send UDP DNS queries (AAAA only).
   - TCP client: IPv6 sockets only (`AF_INET6`), non-blocking + `epoll`.
   - No IPv4 parsing, no A-record lookup, no dual-stack connect attempts.

2. HTTP/1.1 client (over TLS)
   - Requests: `GET / HTTP/1.1` with `Host: wikipedia.org` and `Connection: close` initially.
   - Response parsing: status line, headers, chunked transfer, content-length.
   - Redirect handling (Wikipedia will redirect; follow to final `https://*.wikipedia.org/`).
   - Start by sending `Accept-Encoding: identity` to avoid decompression.

3. TLS 1.3 client-only stack
   - CSPRNG via `getrandom()`.
   - Crypto: SHA-256, HMAC, HKDF; X25519; AEAD (prefer ChaCha20-Poly1305 first).
   - Certificate parsing/verification: minimal X.509 chain validation with a bundled CA set.
   - SNI support for `wikipedia.org`.
   - ALPN not required initially (HTTP/1.1 is acceptable); keep scope minimal.

Deliverable
- `browser` can fetch `https://wikipedia.org/` and render the page text into the framebuffer.

---

## 4) Networking baseline (Milestone: robust HTTP plumbing)
1. DNS
   - Minimal resolver: read `/etc/resolv.conf`, send UDP DNS queries (AAAA only).
   - Cache AAAA responses.

2. TCP
   - IPv6-only sockets + `epoll`.

3. HTTP/1.1 client (no TLS yet)
   - Requests: GET, HEAD.
   - Response parsing: status line, headers, chunked transfer, content-length.
   - Redirects (301/302/307/308) with limits.
   - Basic connection reuse.

4. Decompression (optional early)
   - Initially request `Accept-Encoding: identity`.
   - Later add a small DEFLATE implementation if needed (many sites require gzip/br).

Deliverable
- Load and display plaintext/HTML from `http://` sites (or a local server).

---

## 5) TLS 1.3 hardening (Milestone: broader HTTPS compatibility)
1. Crypto primitives (no deps)
   - SHA-256, HMAC, HKDF
   - X25519 key exchange
   - Signature verify: start with ECDSA P-256 (common) and/or RSA-PSS (bigger).
   - AEAD: AES-128-GCM and/or ChaCha20-Poly1305 (ChaCha is often simpler/faster in software).
   - CSPRNG: `getrandom()` syscall.

2. TLS 1.3 handshake (client-only)
   - ClientHello with SNI, supported groups/ciphers.
   - ServerHello, key schedule, encrypted handshake.
   - Certificate parse + verify chain.

3. Certificate validation (pragmatic)
   - Minimal X.509 parser.
   - Trust store: bundle a small curated CA set (or read system store) and implement path validation.
   - Time validation via `clock_gettime()` (or allow override).

4. HTTP/1.1 over TLS
   - Wrap socket with TLS record layer.

Deliverable
- Fetch `https://example.com` and other stable sites and render text.

---

## 6) HTML parsing + DOM (Milestone: real pages render as readable documents)
1. HTML tokenizer/parser (pragmatic subset)
   - Handle common tags: `html`, `head`, `body`, `div`, `span`, `p`, `a`, `img`, `ul/ol/li`, `h1..h6`, `pre`, `code`, `br`, `hr`.
   - Handle entity decoding for common entities.

2. DOM tree
   - Node types: element, text.
   - Attributes storage with interned strings.

3. Resource loading
   - Resolve relative URLs.
   - Images: start with PNG (or none), or allow only JPEG/PNG later.

Deliverable
- HTML pages show as structured text with links and basic block layout.

---

## 7) CSS subset + layout engine (Milestone: looks “web-like”)
1. CSS parsing
   - `style` tags and inline `style` attributes.
   - Minimal selector support: tag, class, id; descendant.

2. Style system
   - Inheritance basics.
   - Properties to start:
     - `display` (block/inline/none)
     - `margin/padding/border` (px only)
     - `color`, `background-color`
     - `font-size` (px), `font-weight` (normal/bold)
     - `width/height` (auto/px)

3. Layout
   - Block layout + inline text layout with wrapping.
   - Scrollable viewport, anchor navigation.

4. Painting
   - Backgrounds, borders, text.

Deliverable
- Popular “article” pages become readable and reasonably styled.

---

## 8) JavaScript (Milestone: navigation works on modern sites)
1. Strategy: incremental JS capability
   - Phase A: no JS; show noscript fallback.
   - Phase B: minimal interpreter to run trivial scripts.
   - Phase C: enough DOM + events to handle common navigation.

2. Minimal JS runtime scope
   - Parser + bytecode VM (or a small interpreter) for ES5-ish core.
   - Garbage collector (simple mark/sweep).

3. Web APIs (very small subset)
   - `console.log`
   - `setTimeout` (coarse)
   - DOM: `document.querySelector`, `getElementById`, `createElement`, `appendChild`, `textContent`.
   - Events: `click`, `input`.

4. Networking APIs (careful)
   - Start with no `fetch`.
   - Later: minimal `XMLHttpRequest`/`fetch` subset if needed for navigation.

Deliverable
- Can log in? Probably not initially.
- Can navigate a JS-heavy site’s basic flows? Target a curated set of popular sites.

---

## 9) Performance + memory (Milestone: fast and stable)
1. Profiling hooks
   - Frame time, layout time, paint time.

2. Caches
   - Parsed CSS, layout results, image decode cache.

3. Memory arenas
   - Region allocators for DOM/layout.
   - Reuse between navigations.

Deliverable
- Smooth scroll, predictable memory growth, responsive input.

---

## 10) Hardening + “useful browser” features
1. Basic security
   - TLS verification defaults on.
   - Same-origin basics.

2. UX
   - Bookmarks, history, downloads directory.

3. Standards pragmatism
   - Add the 20% of features needed for 80% of sites.

Deliverable
- A minimal daily-use reader browser.

---

## First concrete milestones (recommended order)
1. UI + input (URL bar, scrolling text pane)
2. HTTP/1.1 over plain TCP to a local test server
3. TLS 1.3 + HTTPS GET to a couple of stable sites
4. HTML parsing and simple layout (readable articles)
5. CSS subset for “web-like” appearance
6. JS incrementally, only as needed for target sites
