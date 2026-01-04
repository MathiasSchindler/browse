#pragma once

#include "syscall.h"

/* Shared-memory "simulated framebuffer" layout.
 * The viewer maps the same file and displays the pixel buffer.
 */

enum {
	CFB_MAGIC = 0x30424643u, /* 'CFB0' little-endian */
	CFB_FORMAT_XRGB8888 = 1,
};

enum {
	CFB_KEY_TEXT = 1,
	CFB_KEY_BACKSPACE = 2,
	CFB_KEY_ENTER = 3,
	CFB_KEY_ESCAPE = 4,
};

enum {
	CFB_KEYQ_SIZE = 32,
};

struct cfb_key_event {
	uint32_t kind;
	uint32_t ch; /* For CFB_KEY_TEXT: byte value (usually ASCII). */
};

struct cfb_header {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t stride_bytes;
	uint32_t format;
	uint32_t reserved0; /* used: wheel_delta_y (int32_t) */
	uint32_t reserved1;
	uint64_t frame_counter;
	uint64_t reserved2; /* used: wheel_event_counter */
	uint64_t reserved3;
	/* Input events (written by dev-only viewer or future input layer).
	 * Counters are monotonically increasing.
	 */
	uint64_t input_counter;
	uint32_t mouse_x;
	uint32_t mouse_y;
	uint32_t mouse_buttons;     /* bitmask: bit0=left, bit1=middle, bit2=right, ... */
	uint32_t mouse_last_button; /* SDL button id (1=left, 2=middle, 3=right, ...) */
	uint32_t mouse_last_state;  /* 1=down, 0=up */
	uint32_t mouse_last_x;
	uint32_t mouse_last_y;
	uint64_t mouse_event_counter;
	/* Keyboard/text events (written by dev viewer; consumed by browser).
	 * Events are pushed into a small ring buffer.
	 */
	uint32_t keyq_wpos;
	uint32_t reserved4;
	struct cfb_key_event keyq[CFB_KEYQ_SIZE];
};

static inline int32_t cfb_wheel_delta_y(const struct cfb_header *h)
{
	return h ? (int32_t)h->reserved0 : 0;
}

static inline void cfb_set_wheel_delta_y(struct cfb_header *h, int32_t dy)
{
	if (!h) return;
	h->reserved0 = (uint32_t)dy;
}

static inline uint64_t cfb_wheel_counter(const struct cfb_header *h)
{
	return h ? h->reserved2 : 0;
}

static inline void cfb_bump_wheel_counter(struct cfb_header *h)
{
	if (!h) return;
	h->reserved2++;
}

static inline size_t cfb_pixel_bytes(uint32_t width, uint32_t height)
{
	return (size_t)width * (size_t)height * 4u;
}
