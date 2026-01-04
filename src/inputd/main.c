#include "../core/cfb.h"
#include "../core/log.h"

enum {
	CFB_HEADER_V1_SIZE = 56u,
	PAGE_SIZE = 4096u,

	EV_SYN = 0x00,
	EV_KEY = 0x01,
	EV_REL = 0x02,

	REL_X = 0x00,
	REL_Y = 0x01,
	REL_WHEEL = 0x08,

	BTN_LEFT = 0x110,
	BTN_RIGHT = 0x111,
	BTN_MIDDLE = 0x112,

	EAGAIN = 11,
};

struct input_event {
	int64_t tv_sec;
	int64_t tv_usec;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

static inline uint32_t clamp_u32(int64_t v, uint32_t lo, uint32_t hi)
{
	if (v < (int64_t)lo) return lo;
	if (v > (int64_t)hi) return hi;
	return (uint32_t)v;
}

struct cfb_mapping {
	int fd;
	void *ptr;
	size_t len;
	uint32_t width;
	uint32_t height;
};

static int cfb_try_map(struct cfb_mapping *m)
{
	int fd = sys_openat(AT_FDCWD, "/dev/shm/cfb0", O_RDWR, 0);
	if (fd < 0) {
		return -1;
	}

	void *hdr_map = sys_mmap(0, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (hdr_map == MAP_FAILED) {
		sys_close(fd);
		return -1;
	}

	struct cfb_header *hdr = (struct cfb_header *)hdr_map;
	if (hdr->magic != CFB_MAGIC || hdr->format != CFB_FORMAT_XRGB8888 || hdr->width == 0 || hdr->height == 0 || hdr->stride_bytes == 0) {
		sys_munmap(hdr_map, PAGE_SIZE);
		sys_close(fd);
		return -1;
	}

	uint32_t width = hdr->width;
	uint32_t height = hdr->height;
	uint32_t stride_bytes = hdr->stride_bytes;
	uint32_t version = hdr->version;

	size_t pixels_off = (version >= 2) ? sizeof(struct cfb_header) : (size_t)CFB_HEADER_V1_SIZE;
	size_t total_len = pixels_off + (size_t)height * (size_t)stride_bytes;

	sys_munmap(hdr_map, PAGE_SIZE);

	void *full = sys_mmap(0, total_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (full == MAP_FAILED) {
		sys_close(fd);
		return -1;
	}

	struct cfb_header *full_hdr = (struct cfb_header *)full;
	if (full_hdr->magic != CFB_MAGIC || full_hdr->format != CFB_FORMAT_XRGB8888) {
		sys_munmap(full, total_len);
		sys_close(fd);
		return -1;
	}

	if (m->ptr && m->len) {
		sys_munmap(m->ptr, m->len);
	}
	if (m->fd >= 0) {
		sys_close(m->fd);
	}

	m->fd = fd;
	m->ptr = full;
	m->len = total_len;
	m->width = width;
	m->height = height;
	return 0;
}

static uint32_t btn_bit_from_code(uint16_t code)
{
	switch (code) {
	case BTN_LEFT: return 1u << 0;
	case BTN_MIDDLE: return 1u << 1;
	case BTN_RIGHT: return 1u << 2;
	default: return 0;
	}
}

static uint32_t btn_id_from_code(uint16_t code)
{
	/* SDL-compatible ids used by viewer:
	 * 1=left, 2=middle, 3=right
	 */
	switch (code) {
	case BTN_LEFT: return 1u;
	case BTN_MIDDLE: return 2u;
	case BTN_RIGHT: return 3u;
	default: return 0;
	}
}

int main(void)
{
	LOGI("inputd", "starting\n");

	int input_fds[32];
	uint32_t input_fd_count = 0;
	for (uint32_t i = 0; i < 32; i++) input_fds[i] = -1;

	/* Best-effort: open a bunch of /dev/input/event* nodes non-blocking.
	 * Permission note: on many distros this requires running as root or being
	 * in the 'input' group.
	 */
	for (uint32_t i = 0; i < 32; i++) {
		char path[32];
		/* Build "/dev/input/eventNN" */
		const char *prefix = "/dev/input/event";
		uint32_t p = 0;
		for (; prefix[p]; p++) path[p] = prefix[p];

		uint32_t n = i;
		char a = (char)('0' + (n / 10u));
		char b = (char)('0' + (n % 10u));
		if (n >= 10u) {
			path[p++] = a;
			path[p++] = b;
		} else {
			path[p++] = b;
		}
		path[p] = 0;

		int fd = sys_openat(AT_FDCWD, path, O_RDONLY | O_NONBLOCK, 0);
		if (fd >= 0 && input_fd_count < (uint32_t)(sizeof(input_fds) / sizeof(input_fds[0]))) {
			input_fds[input_fd_count++] = fd;
		}
	}

	if (input_fd_count == 0) {
		LOGW("inputd", "no readable /dev/input/event* devices (need perms?)\n");
	}

	struct cfb_mapping fb;
	fb.fd = -1;
	fb.ptr = 0;
	fb.len = 0;
	fb.width = 0;
	fb.height = 0;

	uint32_t remap_ticks = 0;

	for (;;) {
		if (!fb.ptr) {
			if (cfb_try_map(&fb) == 0) {
				LOGI("inputd", "mapped /dev/shm/cfb0\n");
			} else {
				struct timespec ts = {0, 100000000}; /* 100ms */
				sys_nanosleep(&ts, 0);
				continue;
			}
		}

		/* Periodically re-open and re-map to survive producer recreation. */
		if (++remap_ticks >= 200) {
			remap_ticks = 0;
			(void)cfb_try_map(&fb);
		}

		struct cfb_header *hdr = (struct cfb_header *)fb.ptr;
		if (hdr->magic != CFB_MAGIC) {
			/* Producer likely replaced the file; force re-map. */
			sys_munmap(fb.ptr, fb.len);
			fb.ptr = 0;
			fb.len = 0;
			if (fb.fd >= 0) sys_close(fb.fd);
			fb.fd = -1;
			continue;
		}

		/* Keep a local cursor state (initialized from shm). */
		uint32_t cur_x = hdr->mouse_x;
		uint32_t cur_y = hdr->mouse_y;

		int had_any = 0;
		for (uint32_t fi = 0; fi < input_fd_count; fi++) {
			int fd = input_fds[fi];
			if (fd < 0) continue;

			struct input_event ev[64];
			ssize_t n = sys_read(fd, ev, sizeof(ev));
			if (n == 0) continue;
			if (n < 0) {
				if (n == -(ssize_t)EAGAIN) continue;
				continue;
			}

			had_any = 1;
			size_t cnt = (size_t)n / sizeof(ev[0]);
			for (size_t i = 0; i < cnt; i++) {
				if (ev[i].type == EV_REL) {
					if (ev[i].code == REL_X) {
						int64_t nx = (int64_t)cur_x + (int64_t)ev[i].value;
						cur_x = clamp_u32(nx, 0u, (hdr->width ? (hdr->width - 1u) : 0u));
					} else if (ev[i].code == REL_Y) {
						int64_t ny = (int64_t)cur_y + (int64_t)ev[i].value;
						cur_y = clamp_u32(ny, 0u, (hdr->height ? (hdr->height - 1u) : 0u));
					} else if (ev[i].code == REL_WHEEL) {
						/* Wheel: reuse reserved fields (see core/cfb.h helpers). */
						cfb_set_wheel_delta_y(hdr, ev[i].value);
						cfb_bump_wheel_counter(hdr);
						hdr->input_counter++;
					}
					continue;
				}

				if (ev[i].type == EV_KEY) {
					uint32_t bit = btn_bit_from_code(ev[i].code);
					uint32_t btn_id = btn_id_from_code(ev[i].code);
					if (!bit || !btn_id) continue;

					if (ev[i].value) hdr->mouse_buttons |= bit;
					else hdr->mouse_buttons &= ~bit;

					hdr->mouse_x = cur_x;
					hdr->mouse_y = cur_y;
					hdr->mouse_last_x = cur_x;
					hdr->mouse_last_y = cur_y;
					hdr->mouse_last_button = btn_id;
					hdr->mouse_last_state = ev[i].value ? 1u : 0u;
					hdr->mouse_event_counter++;
					hdr->input_counter++;

					if (btn_id == 1u) {
						LOGI("inputd", ev[i].value ? "left down\n" : "left up\n");
					}
				}
			}
		}

		/* Publish latest cursor position even if no click happened. */
		hdr->mouse_x = cur_x;
		hdr->mouse_y = cur_y;

		if (!had_any) {
			struct timespec ts = {0, 5000000}; /* 5ms */
			sys_nanosleep(&ts, 0);
		}
	}
}
