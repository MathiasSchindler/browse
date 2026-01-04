#pragma once

#include "../core/cfb.h"

struct shm_fb {
	struct cfb_header *hdr;
	void *base;
	size_t map_len;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t *pixels;
	int fd;
};

static inline int shm_fb_open(struct shm_fb *out, uint32_t width, uint32_t height)
{
	const char *path = "/dev/shm/cfb0";
	uint32_t stride = width * 4u;
	size_t total_size = sizeof(struct cfb_header) + cfb_pixel_bytes(width, height);

	int fd = sys_openat(AT_FDCWD, path, O_CREAT | O_RDWR, 0666);
	if (fd < 0) {
		dbg_write("openat /dev/shm/cfb0 failed\n");
		return -1;
	}
	if (sys_ftruncate(fd, (off_t)total_size) < 0) {
		dbg_write("ftruncate failed\n");
		sys_close(fd);
		return -1;
	}

	void *mapped = sys_mmap(0, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		dbg_write("mmap failed\n");
		sys_close(fd);
		return -1;
	}

	struct cfb_header *hdr = (struct cfb_header *)mapped;
	hdr->magic = CFB_MAGIC;
	hdr->version = 3;
	hdr->width = width;
	hdr->height = height;
	hdr->stride_bytes = stride;
	hdr->format = CFB_FORMAT_XRGB8888;
	hdr->reserved0 = 0;
	hdr->reserved1 = 0;
	/* Initialize optional input fields to 0. */
	hdr->input_counter = 0;
	hdr->mouse_x = 0;
	hdr->mouse_y = 0;
	hdr->mouse_buttons = 0;
	hdr->mouse_last_button = 0;
	hdr->mouse_last_state = 0;
	hdr->mouse_last_x = 0;
	hdr->mouse_last_y = 0;
	hdr->mouse_event_counter = 0;
	hdr->keyq_wpos = 0;
	hdr->reserved4 = 0;
	for (uint32_t i = 0; i < CFB_KEYQ_SIZE; i++) {
		hdr->keyq[i].kind = 0;
		hdr->keyq[i].ch = 0;
	}
	hdr->reserved2 = 0;
	hdr->reserved3 = 0;

	out->hdr = hdr;
	out->base = mapped;
	out->map_len = total_size;
	out->width = width;
	out->height = height;
	out->stride = stride;
	out->pixels = (uint32_t *)((uint8_t *)mapped + sizeof(struct cfb_header));
	out->fd = fd;
	return 0;
}
