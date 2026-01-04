# `src/core/main.c` refactor plan (split into modules)

This document is a step-by-step implementation plan for splitting the core demo entrypoint into smaller, focused translation units while keeping behavior identical and preventing regressions.

## Goals

- Keep the current behavior unchanged:
  - `./build/core` (default `BACKEND=shm`) produces frames in `/dev/shm/cfb0`.
  - `BACKEND=fbdev` continues to work when enabled.
  - Viewer workflow remains the same.
- Make `main.c` a thin entrypoint and move implementation details into dedicated files.
- Keep builds strict (`-Wall -Wextra -Werror`) and catch issues early.

## Pre-flight checks (baseline)

Run these first so you know you’re starting from green.

1. Ensure `src/core` is not ignored by Git (historical issue):

   ```bash
   git check-ignore -v src/core/main.c || true
   ```

   Expected: no output.

2. Clean + rebuild all artifacts:

   ```bash
   make clean-all
   make all
   ```

3. Build core with both backends (compilation check):

   ```bash
   make core BACKEND=shm
   make core BACKEND=fbdev
   ```

4. Run host-side tests:

   ```bash
   make test
   ```

5. (Optional, but recommended) Quick manual smoke test:

   - Terminal A: `./build/core`
   - Terminal B: `./build/viewer`

   Expected: animated pattern + readable overlay text.

## High-level target structure

Proposed split (names are suggestions; choose what matches your style):

- `src/core/main.c`
  - Only parses/chooses backend and calls a `core_run()` or backend function.
- `src/core/demo_frame.c` + `src/core/demo_frame.h`
  - `draw_frame(...)` and any demo-only text overlay helpers.
- `src/core/backend_shm.c` + `src/core/backend_shm.h`
  - `run_shm_backend()` and shm-specific setup for `/dev/shm/cfb0`.
- `src/core/backend_fbdev.c` + `src/core/backend_fbdev.h` (optional)
  - `run_fbdev_backend()` behind `#if defined(BACKEND_FBDEV)`.

## Implementation plan (incremental, regression-safe)

### Step 1 — Introduce `demo_frame.h/.c`

1. Create `src/core/demo_frame.h` that declares:

   - `void draw_frame(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t frame);`

2. Move the current `static void draw_frame(...)` implementation out of `src/core/main.c` into `src/core/demo_frame.c`.

3. Update `src/core/main.c`:

   - Remove the old implementation.
   - `#include "demo_frame.h"`.

**Checks**

- Compile only core:

  ```bash
  make clean
  make core
  ```

- Verify symbol linkage issues early:

  - No `undefined reference` errors.
  - No `static` visibility assumptions in the moved function.

### Step 2 — Extract SHM backend into `backend_shm.*`

1. Create `src/core/backend_shm.h` declaring:

   - `int run_shm_backend(void);`

2. Move `run_shm_backend()` (and any SHM-only constants/locals) from `src/core/main.c` to `src/core/backend_shm.c`.

3. Keep shared demo constants (like `FB_W/FB_H`) in one place:

   - Option A: Leave `FB_W/FB_H` in `backend_shm.c` only.
   - Option B: Move them to a small `core_config.h` used by both backends.

4. Ensure `backend_shm.c` includes the headers it actually uses (e.g. `cfb.h`, syscall wrappers, etc.). Avoid relying on transitive includes from `main.c`.

**Checks**

- Build `core` twice to ensure no stale objects hide missing includes:

  ```bash
  make clean
  make core
  make clean
  make core
  ```

- Manual runtime check (SHM path):

  ```bash
  ./build/core
  ```

  In another terminal:

  ```bash
  ./build/viewer
  ```

  Expected: same output as baseline.

### Step 3 — Extract fbdev backend (if used) into `backend_fbdev.*`

1. Create `src/core/backend_fbdev.h` declaring:

   - `int run_fbdev_backend(void);`

2. Move the `#if defined(BACKEND_FBDEV)` block to `src/core/backend_fbdev.c`.

3. Keep the compile-time guard:

   - `backend_fbdev.c` should compile even when `BACKEND_FBDEV` is not set, or it should be conditionally included in the build. Pick one and be consistent.

**Checks**

- Compile fbdev variant:

  ```bash
  make clean
  make core BACKEND=fbdev
  ```

- If you have a machine with `/dev/fb0` and permission, do a quick smoke run there.

### Step 4 — Make `main.c` a minimal dispatcher

Refactor `main()` to be a thin wrapper:

- Includes only the backend headers.
- Calls the selected backend:

  - If `BACKEND_FBDEV`: `return run_fbdev_backend();`
  - Else: `return run_shm_backend();`

**Checks**

- Ensure `main.c` no longer pulls in implementation-heavy headers unnecessarily.
- Build all targets that depend on the core toolchain flags:

  ```bash
  make clean-all
  make all
  ```

### Step 5 — Update build wiring (Makefile)

Update `CORE_SRCS` in the Makefile so the new `.c` files are compiled into `build/core`:

- Add `src/core/demo_frame.c`
- Add `src/core/backend_shm.c`
- Add `src/core/backend_fbdev.c` (if used)

**Checks**

- Run:

  ```bash
  make clean
  make core
  make test
  ```

- Ensure no other targets accidentally require the new files (keep split local to core unless intentionally shared).

### Step 6 — Regression checklist (before merging)

Run the full “confidence sweep”:

```bash
make clean-all
make all
make core BACKEND=shm
make core BACKEND=fbdev
make test
```

Then (manual): run `./build/core` + `./build/viewer` and visually confirm:

- Overlay text still renders and is readable.
- Frame counter increments normally.
- No tearing/stride artifacts appear (basic sanity).

## Additional guardrails (optional)

- Add a lightweight “build-only CI step” script (e.g. `tools/ci_build.sh`) that runs the commands in the regression checklist.
- Consider a tiny header discipline rule: each `.c` includes what it uses, and moved code should not depend on `main.c` includes.

## Notes on keeping changes safe

- Move code in small steps and re-run `make core` after each move.
- Prefer mechanical moves (cut/paste + includes) before any renaming.
- Avoid changing exported APIs during the split; do cleanups in a follow-up.
