# Unicode + missing glyphs: lightweight implementation plan

This repo is byte-oriented at the renderer: the bitmap font maps **one byte (0..255)** to one glyph.
So “Unicode support” here means:

- Decode UTF-8 into Unicode codepoints.
- Map codepoints into a **displayable byte** (ASCII/Latin-1 or a small custom extension page).
- Ensure the selected byte has a bitmap glyph in the built-in fonts.

This plan focuses on a tight loop to **detect** missing characters and **add** only what we need, without pulling in external libraries.

## Goals

- Every character we cannot display triggers a **stderr warning**.
- Provide a repeatable workflow to:
  - turn warnings into a concrete “add glyphs/mappings” task list
  - implement those tasks by editing the font source-of-truth
  - validate with tests
- Keep runtime code syscall-only (viewer may use SDL2).

## Terminology (two kinds of “missing”)

1) **Unrenderable codepoint** (`unrenderable codepoint: U+....`)
- Happens during **HTML → visible-text extraction**.
- We decoded a Unicode codepoint but do not map it to a display byte.
- Fix is usually in the **codepoint→byte mapping**.

2) **Missing glyph** (`missing glyph: 0x..`)
- Happens during **font rendering**.
- A byte made it into the final text stream, but the bitmap font has no glyph for it.
- Fix is adding a glyph for that byte in the font generator.

## Step-by-step plan

### Phase 0 — Ensure warnings are enabled

1. Build the browser normally.
2. Confirm warnings show up on stderr when browsing real content.
3. Keep warnings deduplicated enough to be useful (avoid flooding on reflows/re-extracts).

### Phase 1 — Lightweight detection loop

4. Reproduce on a representative page that triggers warnings.
5. Capture stderr into a log file:
   - Run the browser and redirect stderr: `2>unicode.log`
   - If you use the viewer, capture the browser’s stderr separately.

6. Categorize warnings into two lists:
   - **Codepoints**: unique `U+........` values from `unrenderable codepoint`.
   - **Bytes**: unique `0x..` values from `missing glyph`.

7. Decide the handling for each codepoint:
   - **ASCII transliteration** (fastest): map to existing ASCII (e.g. smart quotes → `"`, dagger → `*`, arrows → `^`/`>`).
   - **Small extension page** (still byte-based): choose a byte range (commonly 0x80–0x9F) and assign specific punctuation codepoints to those bytes.

   Recommendation for “lightweight + useful”:
   - Start with transliteration for rare symbols.
   - Use an extension page only for very common punctuation that significantly improves readability.

### Phase 2 — Implement codepoint→byte mapping

8. Add mappings for the chosen codepoints in the UTF-8/text pipeline:
   - Location: the code that emits a byte from a Unicode codepoint (HTML extractor).

9. For each new mapping, pick a byte value that is:
   - stable (won’t conflict with current content)
   - supported by the renderer/font

10. Add/adjust a deterministic regression test:
   - Add a `visible-text` test case that includes the UTF-8 sequence and asserts the emitted output.
   - This ensures future refactors don’t silently drop it again.

### Phase 3 — Add missing glyph bitmaps

11. Treat generated font `.c` files as build artifacts.
    - Do **not** edit `font_builtin_*.c` directly.

12. Add glyphs to the source-of-truth in the generator:
    - Location: `g_glyphs_8x8[]` table in the font generator.
    - Add entries for bytes that triggered `missing glyph: 0x..`.

13. Regenerate font tables:
    - Run `make fonts` (or `make all`).

14. Extend the font regression test:
    - Update `test_text_font` to assert the new glyph rows match expected bit patterns.

15. Re-run the full test suite:
    - `make test`

### Phase 4 — Triage policy + automation

16. Ensure the “no external libs” policy stays enforced:
    - Run `make audit` (should pass).

17. Optional: add a tiny helper tool for log triage (still lightweight, no deps):
    - A small host-side script/program that reads `unicode.log` and prints:
      - unique sorted codepoints
      - unique sorted bytes
      - counts
    - Output format should be easy to paste into an issue/PR description.

## Extension page (if needed): suggested approach

If ASCII transliteration makes text too lossy (e.g., typographic quotes/daggers are ubiquitous on Wikipedia pages), introduce a minimal “punctuation page”:

- Reserve 0x80–0x9F for a handful of punctuation glyphs.
- Add glyph bitmaps for those bytes in the font generator.
- Map common codepoints to those bytes in the extractor.

Keep it small and explicit; avoid attempting general Unicode.

## Definition of done

- Running the browser on a representative page produces either:
  - no unicode-related warnings, or
  - only warnings for characters we intentionally don’t support yet.
- Any remaining warnings are actionable (codepoint list/byte list) and don’t spam uncontrollably.
- `make test` and `make audit` are green.
