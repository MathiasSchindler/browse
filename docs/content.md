# content.md — Plan: HTML parsing + visible text rendering, then CSS

Goal: starting from the current HTTPS fetch (status + headers), incrementally parse enough of the HTTP body to render **visible text** into the framebuffer, with regression tests after each step. Once text rendering is stable, add a minimal CSS pipeline in small, testable slices.

North Star
- Wikipedia readability is our primary “north star” while building, because it’s a real, popular, complex document.
- We will not add Wikipedia-specific hacks/shortcuts that don’t generalize (or that would hurt other pages/projects). Prefer semantic, standards-based improvements that help broadly.

Non-goals (for now): full HTML5 compliance, full CSS spec, JS, fonts, layout engine, images, forms.

---

## Phase 0 — Plumbing: fetch the response body

### Step 0.1: Add an API to fetch body bytes (not just status line)
- Add a new function (or extend existing):
  - `tls13_https_get_body(sock, host, path, out_buf, out_cap, out_len, ...)`
  - It should:
    - do TLS handshake
    - send GET
    - parse status + headers
    - then read **body** (possibly limited to N bytes)
- Keep it bounded: cap body read to something like `256 KiB` initially.

Tests
- Add `tools/test_http_headers.c` style tests (pure C, no network) for:
  - header/body boundary detection (`\r\n\r\n`)
  - `Content-Length` parsing (happy path + missing)

Regression discipline
- Keep `make test` green.
- Add a deterministic, offline test for header parsing.

### Step 0.2: Handle `Transfer-Encoding: chunked` (minimal)
- Wikipedia often uses `Content-Length` for page bodies, but chunked is common.
- Implement minimal chunked decoding:
  - parse hex chunk size line
  - read chunk payload
  - stop at 0-sized chunk
  - ignore chunk extensions

Tests
- Add `tools/test_chunked.c` with known chunked fixtures:
  - a small chunked body
  - chunk extensions present
  - invalid hex handling

---

## Phase 1 — HTML: tokenization → visible text

Principle: **render text without building a full DOM** at first. Streaming tokenizer is enough.

### Step 1.1: Implement an HTML tokenizer (very small)
Create `src/browser/html_tok.h/.c` (or `.h` header-only) that can iterate through input bytes and yield tokens:
- `TEXT` token: literal text between tags
- `TAG_OPEN` token: `<tag ...>`
- `TAG_CLOSE` token: `</tag>`
- `COMMENT` token: `<!-- ... -->` (skip)

Keep it ASCII-only initially; treat bytes as-is.

Tests
- `tools/test_html_tok.c`:
  - basic text and tags: `Hello<b>World</b>`
  - comments ignored
  - malformed tags should not crash; best-effort recovery

### Step 1.2: Decode a minimal set of HTML entities in TEXT
Implement a tiny entity decoder for common cases:
- `&amp; &lt; &gt; &quot; &apos;` and numeric `&#...;` and `&#x...;`.

Tests
- `tools/test_html_entity.c`:
  - `A &amp; B` → `A & B`
  - `&#169;` (or another known value) numeric conversion
  - unknown entity → leave as-is or replace with `?` (pick one and test it)

### Step 1.3: “Visible text” rules (first pass)
Define a minimal visibility/filtering layer:
- Ignore content inside these tags:
  - `script`, `style`, `noscript`
- Treat these as block separators (emit a newline before/after):
  - `p`, `div`, `br`, `h1..h6`, `li`, `ul`, `ol`, `table`, `tr`, `td`, `th`, `hr`
- Collapse runs of whitespace to a single space.

Output format
- Produce a plain text stream with `\n` line breaks.
- Feed into existing `draw_text_u32` (or a simple text layout function).

Tests
- `tools/test_visible_text.c`:
  - ignores `<script>bad</script>`
  - `<p>Hello</p><p>World</p>` becomes `Hello\n\nWorld` (choose exact formatting and lock it)
  - whitespace collapse: `Hello\n   World` → `Hello World`

---

## Phase 2 — Text layout and rendering

### Step 2.1: Build a simple text layout engine
Current `draw_text_u32` draws at fixed x/y and only handles `\n` by moving down 8px.
Add a wrapper “text box renderer” that:
- word-wraps to a width (e.g. full screen width minus margins)
- maintains a scroll offset (later)
- clips at screen bounds

Tests
- `tools/test_text_layout.c` (offline):
  - word-wrapping decisions given a width
  - ensures no line exceeds max chars

### Step 2.2: Render the extracted visible text
- Fetch HTML body (Phase 0)
- Tokenize + visible-text filter (Phase 1)
- Layout + render (this phase)

Regression test strategy
- Add a local “fixture HTML” file under `tools/fixtures/` (small, curated) and test:
  - extracted visible text equals expected output
  - layout output is stable (or at least bounded)

Note: since the project avoids libc, tests can still include fixtures by embedding them as C string literals (or use small included headers generated manually).

---

## Phase 3 — Links (still no CSS)

### Step 3.1: Parse `<a href="...">` and keep link ranges
Keep a lightweight link table:
- Each link has: `href` string (clipped), start/end character offsets in the visible-text stream

Rendering
- Color links differently (e.g. blue text) while still using the same bitmap font.

Input integration
- On click, map click position → character offset → if inside link range, navigate.

Tests
- `tools/test_links.c`:
  - `<a href="/wiki/X">X</a>` captured
  - relative URL resolution (reuse existing redirect/location URL helpers)

---

## Phase 4 — CSS: minimal and incremental

Principle: do **very small** CSS that improves readability without building a full engine.

### Step 4.1: Support inline style attributes (subset)
Parse `style="..."` for a tiny set of properties:
- `color: #rrggbb`
- `background-color: #rrggbb`
- `font-weight: bold` (optional; could map to brighter color since font is fixed)

Apply only to:
- `a` links
- headings
- body text blocks

Tests
- `tools/test_style_attr.c`:
  - parse `color` and apply to span range

Status
- Implemented `src/browser/style_attr.h/.c` with a forgiving parser for `color`, `background-color`, and `font-weight:bold`.
- Wired `<a style="...">` extraction to capture `fg_xrgb`/`bg_xrgb`/`bold` into the link table; renderer now respects link `background-color` and emulates bold by double-drawing.
- Extended extraction/rendering to support non-link inline styles as spans (e.g. `<p style="...">`, `<span style="...">`, `<b>/<strong>`), with deterministic tests.

### Step 4.2: Support `<style>` blocks (tiny CSS parser)
Implement a tiny CSS tokenizer/parser:
- selectors: tag selectors only (`p`, `a`, `h1`)
- declarations: `color`, `background-color`, `display` (block/inline)

Apply precedence (simple)
- inline style overrides `<style>`
- later rules override earlier rules

Tests
- `tools/test_css_parser.c`:
  - parse a few rules
  - ensure correct rule picked

Status
- Implemented a tiny style-block parser in `src/browser/css_tiny.h/.c` for tag selectors and a small property subset.
- Integrated `<style>` parsing into HTML extraction: tag rules apply to generated spans; `display:block/inline` influences block/newline behavior.

### Step 4.3: Add basic layout semantics using CSS-ish model
Introduce a minimal “layout node” representation:
- block nodes start new lines and add vertical spacing
- inline nodes flow within line

Tests
- Extend visible-text fixture tests to ensure:
  - headings have blank line separation
  - list items show bullets or dashes

Status
- Implemented minimal layout semantics in the HTML→text stage (no DOM): headings get extra vertical separation, and list items render with a simple `- ` prefix.
- Headings default to bold via the span styling layer.

---

## Suggested order of implementation PR-style

1. Header/body boundary parsing + Content-Length (tests)
2. Chunked decoding (tests)
3. HTML tokenizer (tests)
4. Entity decoding (tests)
5. Visible-text extraction rules (tests + fixtures)
6. Text layout + word wrap (tests)
7. Integrate into framebuffer rendering
8. Link capture + clickable navigation
9. CSS inline styles
10. CSS style blocks

---

## Practical fixtures to add early

Create a few tiny HTML inputs as fixtures (embedded in tests):
- `fixture_minimal.html`: just headings + paragraphs
- `fixture_entities.html`: lots of entities
- `fixture_links.html`: a handful of links and relative URLs
- `fixture_script_style.html`: contains `<script>` and `<style>` blocks that must be ignored for visible text

For each fixture, store expected extracted visible text (exact) as another string literal.

---

## Guardrails

- Keep every stage bounded (max bytes, max tokens, max links) to avoid hangs.
- Treat malformed HTML/CSS as “best effort”: never crash.
- Avoid network-based tests (too flaky); focus on deterministic parsing/layout tests.
