#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root_dir"

fail=0

# 1) SDL2 must only appear in the viewer.
sdl_hits=$(grep -R --line-number -E '#include\s*<SDL2/SDL\.h>|pkg-config\s+--cflags\s+--libs\s+sdl2' . || true)
if [[ -n "$sdl_hits" ]]; then
  # Allow: tools/viewer_sdl.c include and Makefile viewer link line.
  bad=$(printf "%s\n" "$sdl_hits" | grep -v -E '^\./tools/viewer_sdl\.c:|^\./Makefile:' || true)
  if [[ -n "$bad" ]]; then
    echo "audit: FAIL (SDL2 usage outside viewer)" >&2
    printf "%s\n" "$bad" >&2
    fail=1
  fi
fi

# 2) Runtime code should not pull in userland libraries (libc headers).
# Kernel/uapi headers (e.g. <linux/fb.h>) are OK.
for dir in src/browser src/core src/inputd src/tls; do
  bad=$(grep -R --line-number -E '#include\s*<\s*(stdio|stdlib|string|unistd|fcntl|sys/[^>]+|arpa/[^>]+|netinet/[^>]+|netdb|pthread|dlfcn|execinfo|curl/[^>]+|openssl/[^>]+|zlib)\.h\s*>' "$dir" || true)
  if [[ -n "$bad" ]]; then
    echo "audit: FAIL (libc/external header include under $dir)" >&2
    printf "%s\n" "$bad" >&2
    fail=1
  fi
done

# 3) Spot-check Makefile: only the viewer may use pkg-config sdl2.
mk_hits=$(grep -n -E 'pkg-config.*sdl2' Makefile || true)
if [[ -n "$mk_hits" ]]; then
  bad=$(printf "%s\n" "$mk_hits" | grep -v -E 'pkg-config\s+--exists\s+sdl2|pkg-config\s+--cflags\s+--libs\s+sdl2' || true)
  if [[ -n "$bad" ]]; then
    echo "audit: FAIL (unexpected pkg-config sdl2 usage in Makefile)" >&2
    printf "%s\n" "$bad" >&2
    fail=1
  fi
fi

if [[ "$fail" -ne 0 ]]; then
  exit 1
fi

echo "audit: OK" >&2
