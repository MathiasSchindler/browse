#+#+#+#+# browse

Syscall-only framebuffer core + dev viewer (and a syscall-only HTTPS browser experiment).

Goal: develop a C renderer that uses **only Linux syscalls** (no libc) while still iterating inside GNOME/VS Code.

Approach:
- `build/core` is a **no-libc** binary that renders into a shared-memory backed file: `/dev/shm/cfb0`.
- `build/viewer` is a normal userspace program (uses SDL2 + libc) that opens `/dev/shm/cfb0` and shows it in a window.

New:
- `build/browser` is the starting point for the actual browser (also **no-libc**) and will always render into `/dev/shm/cfb0`.

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

## fbdev caveats

- On many modern Ubuntu installs, `/dev/fb0` may not exist or may not correspond to the active GNOME display.
- The fbdev backend currently expects `32bpp`.
