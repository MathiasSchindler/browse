# images.md — Plan: early image support (layout + decoding)

Goal: render (some) images from real-world pages (especially Wikipedia/Wikimedia) inside our existing text-centric layout, **without external dependencies** and still compatible with the syscall-only/no-libc build.

This is a “huge win” feature, but it’s also one of the highest-effort items because Wikimedia serves primarily **JPEG/PNG/WebP/SVG**, and PNG/JPEG both require non-trivial decoding.

This plan is intentionally incremental: we can get a visible, useful result early (image placeholders that reserve space), then progressively add decoding and better selection heuristics.

---

## Constraints / Non-goals

Constraints
- Browser binary is **freestanding / no-libc**. All decoding code must avoid malloc/stdio and be careful with bounds.
- No third-party libraries.
- Framebuffer output is XRGB8888.
- Networking is already HTTPS GET of a bounded body; images will reuse that.

Non-goals (initial)
- Full CSS layout (floats, flexbox, positioning).
- High-fidelity image scaling and color management.
- Animated formats (GIF/APNG) beyond “first frame”.
- SVG rendering (we’ll treat it as “not supported yet”).

---

## What “early image support” means here

Minimum viable: when encountering `<img>`, we:
1) reserve an area in the layout (block or inline-replaced element)
2) draw a placeholder box (and possibly `alt=` text)
3) optionally fetch+decode the image and blit it when ready

Key design choice: **keep the existing streaming HTML-to-layout approach**. We do not need a DOM.

---

## Phase 1 — Layout + placeholders (no decoding yet)

### 1.1 Extend HTML extraction to emit image items
Today we extract a text buffer plus link/span metadata. For images we need “items” rather than only plain text.

Option A (recommended): introduce an intermediate “flow list”
- Define `struct flow_item { kind = TEXT | NEWLINE | IMAGE | ... }`
- `TEXT`: points into the existing visible-text buffer (or stores a small inline string)
- `IMAGE`: stores `src` URL (clipped), optional `width/height`, and `alt`

This keeps image logic out of the text buffer and makes layout deterministic.

Where
- New module: `src/browser/flow.h/.c` or extend existing `html_text.*` with a second output API.
- Keep current API for tests; add a new API side-by-side to avoid a big-bang refactor.

### 1.2 Parse `<img>` attributes (subset)
We need:
- `src=` (required)
- `srcset=` (optional; pick a usable candidate)
- `width=` / `height=` (optional; for initial reserved space)
- `alt=` (optional; placeholder text)

Important: Wikipedia often uses `srcset` with `.webp` plus fallback. Early heuristic:
- prefer `.png` then `.jpg/.jpeg` then `.gif` then `.webp` (until WebP exists)
- if `src` ends in `.svg`, skip decoding and render placeholder/alt only

### 1.3 Teach `text_layout` about “replaced elements”
We need layout rules, but can stay extremely simple:
- Treat `<img>` as **block** if it appears in a block context or has large intrinsic size.
- Otherwise treat as **inline** item that consumes width.

Implementation idea
- Extend `text_layout.c` to accept an array of flow items and produce “render ops”:
  - `DRAW_TEXT(x,y,str,style)`
  - `DRAW_IMAGE_PLACEHOLDER(x,y,w,h,alt)`
  - later: `DRAW_IMAGE(x,y,w,h,image_handle)`

Scaling
- Reserve size in pixels.
- If width/height missing, use a reasonable default thumbnail size (e.g. 160×120) or 25% of screen width.

### 1.4 Rendering placeholders
Add a very small renderer:
- draw a rectangle border
- draw “IMG” and a clipped alt string
- optionally show URL host/path suffix in debug builds

Tests
- New offline tests for HTML parsing: ensure `<img src=...>` yields an IMAGE flow item.
- Layout tests: placeholder reserves correct rows and affects wrapping.

Outcome
- Even without decoding, Wikipedia becomes more readable because images no longer collapse the surrounding content; users see where media belongs.

---

## Phase 2 — Image fetch pipeline (async-ish)

We want images to appear without blocking the whole UI. With no threads, we can do cooperative work in the main loop.

### 2.1 Image cache + state machine
Create `src/browser/image_cache.h/.c`:
- `image_request(url) -> handle`
- Cache by URL string hash.
- States: `EMPTY` → `FETCHING` → `DECODED` or `FAILED`

Memory limits
- Cap total decoded pixel memory (e.g. 8–16 MiB).
- Cap max image dimensions (e.g. 2048×2048) and max decoded pixels (avoid bombs).

### 2.2 Network fetch integration
- Reuse HTTPS GET to download image bytes (bounded).
- Respect `Content-Type` if present, but also sniff magic bytes.

### 2.3 Rerender on completion
When an image transitions to `DECODED`, trigger a re-render of the current page.

Outcome
- Still placeholders first, images “pop in” when ready.

---

## Phase 3 — First real decoder: PNG (recommended first)

Why PNG first
- Wikimedia uses PNG heavily (icons, diagrams, UI elements).
- Once we have zlib/DEFLATE, it’s reusable and well-contained.

### 3.1 Implement zlib + DEFLATE (freestanding)
- New module: `src/browser/deflate.h/.c`
- Support zlib-wrapped DEFLATE (RFC1950/1951)
- Must handle dynamic Huffman blocks (real PNGs require this)

Safety
- Output size must be bounded by expected uncompressed size.
- Reject invalid codes / overflows.

Host-side tests
- Add `tools/test_deflate.c` with small known compressed fixtures.

### 3.2 Implement minimal PNG decode
New module: `src/browser/png.h/.c`
- Parse signature, IHDR, IDAT, IEND
- Support color types likely needed first:
  - Truecolor (RGB) and Truecolor+Alpha (RGBA), 8-bit
  - Indexed color (PLTE) can be added next (common for icons)
- Implement scanline unfiltering (Sub/Up/Average/Paeth)

Output
- Convert to XRGB8888 (ignore alpha initially or blend against page bg).

Tests
- `tools/test_png.c` with a tiny embedded PNG (e.g. 2×2) and expected pixels.

Outcome
- Many Wikipedia images will render (especially PNG thumbnails/icons/diagrams).

---

## Phase 4 — JPEG (baseline)

### Phase 4.0 — JPEG header only (dimensions first)
Before decoding pixels, implement a tiny JPEG header parser that can extract
width/height from SOF markers. Use this to improve placeholders (e.g. show
`800x600`, and later drive better layout reservation).

Where
- `src/browser/image/jpeg.h/.c`

Outcome
- Better placeholders without committing to full JPEG decode yet.

Why
- Photographs on Wikimedia are overwhelmingly JPEG.

Scope
- Baseline DCT JPEG (no progressive, no CMYK initially).

Implementation
- New module: `src/browser/image/jpeg.h/.c`
- Huffman decode + IDCT.
- Convert YCbCr to RGB.

Tests
- `tools/test_jpeg.c` with a tiny baseline JPEG fixture.

Outcome
- Most photos start rendering.

---

## Phase 5 — Follow-ups (optional / later)

### WebP
- Wikimedia increasingly prefers WebP in `srcset`.
- Decoder is significantly more work (VP8/VP8L).
- Until supported, prefer `.png/.jpg` candidates from `srcset`.

### GIF / APNG
- First-frame only is doable (GIF LZW is moderate).
- Animation can be ignored early.

### SVG
- Full SVG is huge.
- Pragmatic approach: rely on Wikimedia raster thumbnails when present, otherwise show placeholder.

---

## Selector/URL heuristics specifically for Wikimedia

To maximize early success without format support explosion:
- Prefer image URLs that already end in `.png` or `.jpg`.
- When parsing `srcset`, pick the first candidate with a supported extension.
- If only `.webp` is present, fall back to `src` (sometimes still `.jpg`).

Future refinement
- Detect Wikimedia thumbnail URLs and rewrite to request a supported format if possible (only if deterministic and safe).

---

## Proposed file/module layout

- `src/browser/image/` — image-related modules (sniffing, parsers, decoders)

- `src/browser/image_cache.h/.c` — URL→image handle, state machine, memory caps
- `src/browser/deflate.h/.c` — zlib/deflate decompressor
- `src/browser/png.h/.c` — PNG parser + unfilter + RGBA→XRGB
- `src/browser/image/jpeg.h/.c` — JPEG header parser + later baseline JPEG decoder
- `src/browser/flow.h/.c` — HTML extraction output as flow items (TEXT/IMAGE)
- `src/browser/text_layout.c` — accept flow items; produce draw ops for placeholders/images
- `src/browser/main.c` — drive fetch/decode steps and rerender on completion

---

## Testing strategy

Keep tests deterministic and offline:
- Add very small embedded binary fixtures as `static const uint8_t[]` in tools tests.
- Unit test each decoder module on host (`tools/test_png.c`, `tools/test_jpeg.c`, `tools/test_deflate.c`).
- Add integration tests for HTML parsing of `<img>` + `srcset` selection.

---

## Risks / effort notes

- PNG requires a correct DEFLATE implementation; this is the biggest single chunk of work.
- JPEG baseline decoding is also substantial (Huffman + IDCT), but can be isolated.
- Memory safety is critical: malformed images must not crash or overrun buffers.

---

## Suggested milestone path (fastest perceived win)

1) Placeholders + layout reservation for `<img>` (very visible improvement)
2) Async fetch + caching (placeholders populate later)
3) PNG decode (icons/diagrams start working)
4) JPEG decode (photos start working)
5) `srcset` heuristics improvements + optional GIF/WebP
