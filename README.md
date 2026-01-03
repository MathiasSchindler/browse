#+#+#+#+# browse

Syscall-only framebuffer core + dev viewer (and a syscall-only HTTPS browser experiment).

Goal: develop a C renderer that uses **only Linux syscalls** (no libc) while still iterating inside GNOME/VS Code.

Approach:
- `build/core` is a **no-libc** binary that renders into a shared-memory backed file: `/dev/shm/cfb0`.
- `build/viewer` is a normal userspace program (uses SDL2 + libc) that opens `/dev/shm/cfb0` and shows it in a window.

New:
- `build/browser` is the starting point for the actual browser (also **no-libc**) and will always render into `/dev/shm/cfb0`.

## Current capabilities (browser)

The browser is still an experiment, but it’s already more than a skeleton:

- HTTPS fetch with redirect handling (syscall-only runtime, no libc).
- HTML → visible-text extraction with link metadata, basic layout, and a small CSS engine.
- CSS selectors: tag, `.class`, `tag.class`, `#id`, `tag#id`, plus a limited descendant selector (`A B`).
- `display:none` support with a small set of UA/Wikipedia-specific hide rules.
- Underline rendering for links/spans.
- Built-in bitmap fonts are generated deterministically at build time (see `tools/fontgen.c`).

### Images

- `<img>` placeholders render as boxes in the framebuffer (with reserved vertical space).
- Image format sniffing is magic-byte based (not file extension based), cached, and kept off the render path to avoid scroll stutter.
- Header-only dimension parsing for JPEG/PNG/GIF is used to size image boxes.
- Decoders (small images):
	- GIF: first-frame decode to XRGB8888.
	- JPEG: baseline (SOF0) decode to XRGB8888.
	- PNG: non-interlaced, bit depth 8; color types 0/2/3/6; decodes to XRGB8888.

PNG limitations (current): no Adam7 interlace, no bit depths other than 8, and alpha is currently composited over black.

Later, you can switch the core to a backend that opens `/dev/fb0` (or DRM/KMS) while keeping the same renderer.

## Build

Core (no-libc):

```bash
make
```

Browser skeleton (no-libc):

```bash
make browser
```

Run unit tests (host-side):

```bash
make test
```

Size-oriented build toggles:

```bash
SIZE=1 STRIP=1 make all
```

Build core against a real Linux framebuffer device (fbdev):

```bash
make core BACKEND=fbdev
```

Viewer (needs SDL2 dev package):

```bash
sudo apt-get update
sudo apt-get install -y libsdl2-dev pkg-config
make viewer
```

Cleaning build outputs:

- `make clean` removes the syscall-only core (`build/core`) but keeps the dev viewer.
- `make clean-all` removes the entire `build/` directory.

## Run

In one terminal:

```bash
./build/core
```

In another terminal:

```bash
./build/viewer
```

You should see a 1920x1080 animated pattern.

The viewer is designed to keep running even if you stop/restart the producer, or if the producer recreates `/dev/shm/cfb0`.

To run the browser skeleton instead of the demo:

```bash
./build/browser
```

## Notes

- The core uses raw syscalls (`openat`, `ftruncate`, `mmap`, `nanosleep`).
- The pixel format is XRGB8888 written as 0xFFRRGGBB in memory.

## AI assistance

This codebase was mostly written with GPT-5.2.

## fbdev caveats

- On many modern Ubuntu installs, `/dev/fb0` may not exist or may not correspond to the active GNOME display.
- The fbdev backend currently expects `32bpp`.
