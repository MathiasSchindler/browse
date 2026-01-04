# Bitmap font plan (freestanding)

This project currently draws text using a single, embedded 8×8 bitmap font implemented directly in `src/core/text.h`.

This document proposes a more robust and flexible bitmap font system that:

- supports 1–2 bitmap fonts in different sizes (e.g. 8×8 and 8×16)
- keeps all font data and rasterization logic in a small, well-isolated module
- preserves the freestanding/syscall-only constraints (no libc, no dynamic allocation required)
- provides a stable API so UI/layout/CSS work is not tightly coupled to font internals

## Goals

1. **Robust rendering**
   - clip safely to framebuffer bounds (no OOB writes)
   - correct handling of `\n` and `\r` (keep current behavior, make it explicit)
   - configurable background filling (transparent vs opaque)
   - predictable behavior for missing glyphs (fallback glyph + optional logging)

2. **Flexibility**
   - select between at least two bitmap fonts at runtime (or compile-time) without changing callers
   - allow adding more glyphs/encodings later (incrementally) without reworking render code
   - avoid “font data in a giant header” to reduce rebuild churn and keep core clean

3. **Separation**
   - font data files separate from renderer implementation
   - renderer API independent of the browser HTML/CSS/layout modules

4. **No third-party font content**
   - avoid importing/embedding any third-party font files
   - keep glyph source-of-truth in this repo and generated outputs deterministic

## Non-goals (for now)

- TTF/OTF parsing, kerning, complex text shaping
- full Unicode coverage
- proportional fonts
- subpixel rendering / antialiasing

## Proposed module layout

Create a new font module under `src/core/font/`:

- `src/core/font/font.h`
  - public API types (font descriptors, draw config)
  - declarations for built-in fonts (8×8, 8×16)

- `src/core/font/font_render.c`
  - rasterizer implementation (draw glyph, draw string, measure)

- `src/core/font/font_builtin_8x8.c`
  - font data + glyph lookup for 8×8

- `src/core/font/font_builtin_8x16.c`
  - font data + glyph lookup for 8×16

Generator tool (host-side, not linked into the browser):

- `tools/fontgen.c`
   - deterministic generator that emits the built-in font `.c` files
   - no external dependencies (no SDL, no FreeType)
   - glyphs are authored in-repo (as code/spec), so we don’t rely on third-party font content

Repo policy checks:

- `make audit` verifies that runtime code stays syscall-only and that SDL2 usage is confined to the dev-only viewer.

Optionally keep a thin compatibility wrapper:

- `src/core/text.h` (temporary)
  - forwards `draw_text_u32` to `font_draw_text_u32` using the default font

This preserves current call sites while enabling a gradual migration.

## API proposal

The API is designed to keep call sites stable and to isolate complexity.

### Core types

```c
// src/core/font/font.h

struct font_glyph {
	uint8_t w;          // glyph width in pixels
	uint8_t h;          // glyph height in pixels
	uint8_t advance;    // cursor advance in pixels (usually w)
	const uint8_t *rows; // pointer to bit rows (format defined below)
};

struct bitmap_font {
	uint8_t cell_w;     // nominal cell size (monospace)
	uint8_t cell_h;
	// maps an input byte (0..255) to a glyph; may return fallback
	struct font_glyph (*glyph_for)(unsigned char ch);
	// optional name/version for debugging
	const char *name;
};

struct font_draw_style {
	uint32_t fg_xrgb;
	uint32_t bg_xrgb;
	uint8_t opaque_bg;  // fill bg pixels inside glyph cells
	uint8_t bold;       // simple emboldening (see below)
	uint8_t underline;  // optional (phase 2)
};

struct font_surface_u32 {
	void *pixels;          // base pointer
	uint32_t stride_bytes; // bytes per row
	uint32_t w_px;
	uint32_t h_px;
};
```

### Rendering entry points

```c
void font_draw_glyph_u32(
	const struct bitmap_font *font,
	struct font_surface_u32 dst,
	uint32_t x,
	uint32_t y,
	unsigned char ch,
	struct font_draw_style st);

// Draws text, interpreting '\n' and skipping '\r' like today.
// Returns final cursor position via out params (optional).
void font_draw_text_u32(
	const struct bitmap_font *font,
	struct font_surface_u32 dst,
	uint32_t x,
	uint32_t y,
	const char *s,
	struct font_draw_style st,
	uint32_t *out_x,
	uint32_t *out_y);

// Measurement helpers used by layout.
uint32_t font_cell_w(const struct bitmap_font *font);
uint32_t font_cell_h(const struct bitmap_font *font);
```

### Font selection

Expose two built-in fonts:

```c
extern const struct bitmap_font g_font_8x8;
extern const struct bitmap_font g_font_8x16;
```

The browser UI can keep a single global “active font pointer” selected once at startup, or select based on viewport size.

## Glyph data formats

Keep formats simple and freestanding-friendly.

### Format A (w ≤ 8): one byte per row

For common 6×8 / 8×8 / 8×16 fonts:

- `rows` points to `h` bytes
- MSB is leftmost pixel (same as current)
- bits outside `w` must be 0

This is easiest to author and debug.

### Format B (w up to 16): two bytes per row

If we ever want 12×16 or 16×16:

- `rows` points to `h * 2` bytes
- treat each row as a 16-bit big-endian bitfield

We can add this later by extending `font_glyph` with a `row_bytes` field.

For now, stick to Format A for both 8×8 and 8×16.

## Robustness improvements

### 1) Safe clipping

Current rendering assumes the draw area is within bounds. New renderer should:

- early-return if `x >= w_px` or `y >= h_px`
- clip rows/cols so partial glyphs at edges don’t write OOB

This allows drawing near edges (e.g., UI chrome) without delicate caller logic.

### 2) Explicit background behavior

Keep `opaque_bg` semantics:

- if `opaque_bg == 1`: write bg for every “0 bit” within the glyph bounding box
- else: leave destination pixels unchanged for “0 bits”

This provides both transparent text and text-on-solid blocks.

### 3) Missing glyph handling

- `glyph_for` must never return `rows = NULL`
- always provide a fallback glyph (e.g. '?')
- optionally keep the existing `TEXT_LOG_MISSING_GLYPHS` mechanism behind a new module macro (e.g. `FONT_LOG_MISSING_GLYPHS`)

### 4) Bold/underline (minimal, optional)

Bold can stay as the current “double draw with +1px x offset”, but should be implemented inside the font module so callers just set `st.bold = 1`.

Underline is optional for later; for monospace it’s easy: draw a 1px line at `y + cell_h - 1` for the width of the glyph advance.

## Two font sizes: recommended choices

### 8×8 (existing)

- keep the current font table and semantics (ASCII-ish)
- map lowercase to uppercase (optional; can be a font-specific policy)

### 8×16 (new)

Goal: better readability (especially headings) without proportional fonts.

Two reasonable paths:

1. **Native 8×16 font data**
   - best quality
   - slightly more data

2. **Scaled 8×8 to 8×16 (interim)**
   - duplicate each row twice (nearest-neighbor vertical scaling)
   - quick to implement, lower quality

Recommendation: start with a small native 8×16 covering at least:

- space, punctuation, digits, A–Z, common symbols used by URLs

…and keep unknown as '?'.

## Integration steps (incremental, keep `make test` green)

1. **Introduce new module skeleton**
   - add `src/core/font/font.h` and `src/core/font/font_render.c`
   - add a tiny 8×8 built-in font file that reuses current glyphs

1b. **Add generator and make targets**
   - add `tools/fontgen.c` that writes `src/core/font/font_builtin_8x8.c` and `src/core/font/font_builtin_8x16.c`
   - add Makefile targets:
     - `make fontgen` builds `build/fontgen`
     - `make fonts` runs it (and only rewrites outputs if content changed)
   - ensure `make all` includes `fontgen` and `fonts` so CI/dev builds stay in sync

2. **Add clipping + background fill**
   - implement clipped loops
   - add small deterministic tests in `tools/` that render into a tiny buffer and assert pixel patterns

3. **Compatibility wrapper**
   - keep `src/core/text.h` but re-implement:
     - `fill_rect_u32` can remain in `text.h` or move into a small `gfx.h`
     - `draw_char_u32/draw_text_u32` forward to font module with default font
   - this prevents churn in `src/browser/main.c` initially

4. **Add 8×16 font**
   - add `g_font_8x16` + data file
   - allow selecting it via a compile-time option or a runtime toggle (e.g. keypress later)

5. **Migrate call sites**
   - move callers off `struct text_color` to `struct font_draw_style` or provide an adapter
   - keep old names until the migration is complete, then delete the wrapper

6. **Update layout constants**
   - replace hardcoded `8` and `16` step sizes in drawing/layout with `font_cell_w/h`
   - this is where 8×16 becomes truly useful without breaking wrapping/click mapping

## Tests (deterministic)

Add new tests under `tools/`:

- `test_font_clip.c`
  - draw a glyph partially off-screen (negative not representable, so test `x > 0` but near right edge) and assert no writes past the buffer end (use guard bytes)

- `test_font_glyphs.c`
  - verify a couple of known glyph bitmaps match expected pixels (e.g. 'A', '0', '?')

- `test_font_newlines.c`
  - `\r` skipped, `\n` moves down by `cell_h` and resets x

These tests should run offline and not require SDL.

## Acceptance criteria

- `make browser` still builds freestanding
- `make all` builds `fontgen`, runs `fonts`, and produces a usable browser binary
- `make test` stays green
- Rendering output for 8×8 matches current behavior for basic ASCII
- Switching to 8×16 only requires changing the selected `bitmap_font` (not touching rasterizer/callers)
- Font files can be edited/replaced without touching browser code

## `fontgen` content policy (practical)

To satisfy the “not derived from protected content” requirement:

- `fontgen` should not import external font formats (TTF/OTF/BDF/PCF) by default.
- The glyph patterns should be authored directly in this repo (either as primitive strokes/segments or as hand-authored bitmaps).
- Generated `.c` files are treated as build artifacts: they must be fully reproducible from the in-repo source-of-truth.

## Future extensions

- add limited extended glyph set (e.g. UTF-8 → fallback transliteration or a small Latin-1 supplement)
- add “font packs” generated offline (tool converts BDF/PCF into `.c` arrays)
- add per-glyph advance or simple proportional spacing (optional)
- add basic text decorations (underline), and link hover/active styles
