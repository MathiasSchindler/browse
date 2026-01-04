# Size plan (browser binary)

Current baseline: `SIZE=1 STRIP=1 make all -j 12` produces a ~108 KiB `build/browser`.
LTO is enabled by default; use `LTO=0` only for debugging toolchain issues.

This document is a step-by-step plan to:

- (a) identify the biggest contributors to the binary size
- (b) find safe opportunities to reduce size without removing features
- (c) keep the codebase “size-friendly” so future work doesn’t regress size

The project is freestanding/syscall-only (except the SDL2 viewer), so most size comes from our own code + data, not libc.

## A) Identify reasons for file size

### 1) Capture a reproducible baseline

- Build:
  - `make clean && SIZE=1 STRIP=1 make browser`
- Record:
  - `ls -l build/browser`
  - `size build/browser` (section totals)
  - `readelf -S build/browser` (which sections dominate: `.text`, `.rodata`, `.data`, `.bss`)

Keep these numbers in a small table in the PR/issue so changes can be compared.

### 2) Find the largest symbols (functions + data)

Use host tools to identify what actually occupies bytes:

- Functions/data by size:
  - `nm -S --size-sort --radix=d build/browser | tail -n 50`
- Hotspots by file/object:
  - produce a link map (see next step) and sort by contribution

Interpretation tips:

- Large `.text` contributors: usually TLS, image decoders, parsers, big switch statements.
- Large `.rodata` contributors: fonts, lookup tables, strings.

### 3) Produce a link map + section garbage-collection report

Add (temporarily or behind a make flag) linker diagnostics:

- Linker map:
  - `-Wl,-Map,build/browser.map`
- GC report:
  - `-Wl,--print-gc-sections`

Then inspect:

- `build/browser.map` for the biggest archives/objects/symbols.
- GC output for dead code that *should* be removed but isn’t (a sign of unwanted references).

### 4) Confirm which codepaths are pulling code in

If a “surprisingly large” symbol shows up:

- Use grep to find who references it.
- If it’s reachable only via debug/rare paths, mark it `cold` or isolate it so it becomes GC-friendly.

## B) Opportunities to get a smaller binary (without removing features)

This list is ordered from “usually high win / low risk” to “bigger change”.

### 1) Enable LTO for size builds

Try link-time optimization for `SIZE=1`:

- Compile: `-flto`
- Link: `-flto`
- Consider: `-fno-fat-lto-objects` (toolchain-dependent)

Why it helps:

- Cross-TU inlining decisions improve (or reduce) code.
- Unused helpers/data can be dropped more aggressively.

Validation:

- `make test` must stay green.
- Compare `nm --size-sort` before/after.

### 2) Reduce debug/log strings and logging machinery in release

Even if debug info is stripped, runtime debug strings in `.rodata` remain.

Approach:

- Centralize debug printing behind compile-time flags.
- Make release default “quiet” (no debug strings unless explicitly enabled).

### 3) Tighten error paths (keep features)

Techniques that often reduce size without functional removals:

- Mark rare error helpers as `__attribute__((cold))`.
- Avoid formatting-heavy logging; prefer fixed strings.
- Consolidate repeated “error + cleanup” blocks into a single function (but avoid over-abstraction).

### 4) Review large lookup tables and constants

Common culprits:

- font bitmaps
- image decoder tables
- protocol constants

Options:

- Replace tables with computed values if smaller (measure!).
- Store compact encodings (e.g., RLE) and decode at runtime if total bytes drop.
  - This trades a bit of `.text` for reducing `.rodata`.
  - Only do this when `nm` shows `.rodata` dominates.

### 5) Avoid accidental code retention (GC friendliness)

Even with `-ffunction-sections -fdata-sections` and `--gc-sections`, code can be retained if:

- referenced by function pointers/dispatch tables
- referenced by “always included” tables
- pulled in via non-`static` symbols across translation units

Mitigations:

- Prefer `static` for internal helpers.
- Keep dispatch tables minimal and compiled conditionally.
- Split “optional/rare” features into separate translation units so GC can drop them when unused.
  - This does *not* remove features; it just improves dead-code elimination.

### 6) Consider smaller codegen switches (measure)

Sometimes `switch` with many cases produces big jump tables.

Options:

- Replace huge switches with small tables + search.
- Keep “fast path” small; move rare branches out-of-line.

This is workload- and compiler-dependent; only do it when the map/nm data shows it’s worth it.

## C) Codebase cleanup practices that support smaller binaries

### 1) Add a “size investigation” make mode

Add optional targets/flags (developer-only) for diagnostics:

- `make size-report`:
  - builds `SIZE=1 STRIP=1`
  - outputs: `size`, top-50 `nm`, and link map

This makes size regressions easy to spot in review.

### 2) Establish “no implicit dependencies” rules

- Runtime code must remain syscall-only.
- Only the viewer links SDL2.
- Keep host-side tools (like `fontgen`) dependency-free as well.

(There is already an audit target; extend it if new patterns appear.)

### 3) Keep modules cohesive and GC-friendly

Guidelines:

- Put big optional subsystems in their own `.c` file.
- Keep headers small; avoid inlining large helpers into headers.
- Prefer `static` functions for internal linkage.

### 4) Maintain a minimal “supported character set” contract

Define an explicit policy:

- which Unicode codepoints are transliterated to ASCII
- which codepoints map to Latin-1 bytes
- which bytes must have glyphs in the built-in font

This prevents ad-hoc additions that bloat tables or introduce duplicated glyphs.

### 5) Size regression checklist for PRs

For changes that touch TLS/HTML/CSS/images/fonts:

- run `SIZE=1 STRIP=1 make browser`
- compare `ls -l build/browser` to baseline
- if size increases, include `nm --size-sort | tail` diff in the PR description

## Suggested first investigation pass

1. Produce `build/browser.map` and the top-50 `nm` list.
2. Identify the top 3–5 offenders (symbols or object files).
3. Pick the smallest safe lever:
   - LTO if the offenders are spread across TUs
   - `.rodata` compaction if tables dominate
   - logging/string cleanup if strings dominate
4. Repeat until improvements plateau.
