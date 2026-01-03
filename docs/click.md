# Mouse Click UI: Reload Button + URL Bar (Step-by-step plan)

This plan describes a clean way to add a clickable reload button and URL bar to the framebuffer UI, using the existing split between:

- `browser` (shipping): syscall-only, writes pixels to `/dev/shm/cfb0`, and reads input events from the same shared header.
- `viewer` (dev-only): SDL2 window that displays `/dev/shm/cfb0` and injects mouse events into the shared header.

---

## 1) Define the UI contract (rectangles + events)

1. Pick a fixed top-bar height (you already use 24px).
2. Define a small set of UI rectangles in framebuffer pixel coordinates:
   - Reload button rectangle (e.g. a 24×24 square at the left).
   - URL bar rectangle (rest of the top bar with padding).
3. Treat input as edge-triggered events:
   - A “click” is recognized on mouse *button-down*.
   - Use `mouse_event_counter` as the edge trigger (process an event only when the counter increases).

Outcome: the UI is describable as geometry + events, independent of networking/TLS.

---

## 2) Add a minimal input polling layer in `browser`

1. Create a small input module (e.g. `src/browser/input.h` + `src/browser/input.c`).
2. The module keeps `last_seen_mouse_event_counter`.
3. Each tick, it reads the shared header fields:
   - `mouse_event_counter`, `mouse_last_state`, `mouse_last_button`, `mouse_last_x`, `mouse_last_y`.
4. When `mouse_event_counter` increments, it emits exactly one `mouse_event` structure:
   - `x`, `y`, `button`, `down/up`.

Outcome: `browser` has a clean “one event at a time” API.

---

## 3) Add a minimal UI state + hit testing in `browser`

1. Create `src/browser/ui.h` + `src/browser/ui.c`.
2. Define a `ui_state` struct containing only data:
   - `char current_url[256]`
   - focus state: `FOCUS_NONE` / `FOCUS_URL`
   - `reload_requested` flag
   - (optional) `uint32_t nav_generation` counter for debugging.
3. Define a `ui_layout` struct with the rectangles for hit-testing.
4. Implement hit-testing helpers:
   - `rect_contains(rect, x, y)`
5. Implement `ui_handle_mouse(&state, &layout, &mouse_event)`:
   - Click on reload rect ⇒ set `reload_requested = 1`.
   - Click on URL bar rect ⇒ set focus to URL.

Outcome: UI logic is isolated and testable without TLS.

---

## 4) Render the top bar (reload + URL bar)

1. In `ui_draw(...)`, render:
   - Background and title text.
   - Reload button: simple label like `R` or `⟳` (ASCII-only initially).
   - URL bar: a box and the `current_url` string.
   - Focus highlight: if URL bar is focused, draw a brighter box.
2. Keep rendering independent from networking progress.

Outcome: the UI always stays visible and understandable.

---

## 5) Wire UI into `browser/main.c` with a small event loop

1. Convert the current “one-shot fetch then idle” logic into a tiny loop:
   - Poll input.
   - Update UI state.
   - If `reload_requested`, start (or restart) navigation.
   - Draw UI.
2. Keep a small “status line” string that shows the current nav step:
   - resolving / connecting / TLS / HTTP / done / error.

Outcome: clicks can trigger behavior, and the UI remains responsive.

---

## 6) Navigation: start simple, keep the seam clean

1. Initially keep the existing blocking pipeline (DNS → TCP → TLS → HTTP status line) but wrap it behind a single function:
   - `int navigate_fetch_status(url, out_status_line, out_error)`
2. Later evolve into a non-blocking state machine without rewriting the UI:
   - states: `NAV_IDLE`, `NAV_RESOLVING`, `NAV_CONNECTING`, `NAV_TLS`, `NAV_HTTP`, `NAV_DONE`, `NAV_ERROR`.

Outcome: clean separation between UI and networking, easy debugging.

---

## 7) URL editing strategy (short-term vs long-term)

Short-term (mouse-only; useful immediately)
- Clicking the URL bar focuses it.
- Clicking the reload button re-fetches the current URL.
- Optionally: clicking inside the URL bar cycles through a small preset URL list (debug feature) until real keyboard input exists.

Long-term (shipping)
- Implement keyboard input in `browser` via `/dev/input/event*` (syscalls only).
- Use the existing focus state and a cursor position to edit `current_url`.

Outcome: you get a working navigation UX now, and a clear path to real input later.

---

## 8) Regression + safety checks

1. Keep the shared header versioned.
2. Viewer should remain backward compatible:
   - If header `version < 2`, it must use the v1 pixel offset and must not write input fields.
3. Keep `make test` green.

Outcome: incremental progress without breaking the dev workflow.
