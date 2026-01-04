#include "cfb.h"

#include "text.h"

#if defined(BACKEND_FBDEV)
#include "fbdev.h"
#endif

#define FB_W 1920u
#define FB_H 1080u

static void draw_frame(uint32_t *pixels, uint32_t width, uint32_t height, uint32_t stride_bytes, uint32_t frame)
{
	(void)stride_bytes;

	for (uint32_t y = 0; y < height; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)pixels + (size_t)y * (size_t)stride_bytes);
		for (uint32_t x = 0; x < width; x++) {
			uint8_t r = (uint8_t)((x + frame) & 0xffu);
			uint8_t g = (uint8_t)((y + (frame * 3u)) & 0xffu);
			uint8_t b = (uint8_t)(((x ^ y) + (frame * 7u)) & 0xffu);
			row[x] = 0xff000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
		}
	}

	/* Simple text overlay to validate readability. */
	struct text_color tc;
	tc.fg = 0xffffffffu;
	tc.bg = 0x00000000u;
	tc.opaque_bg = 0;

	fill_rect_u32(pixels, stride_bytes, 0, 0, width, 16, 0x80000000u);
	draw_text_u32(pixels, stride_bytes, 8, 4, "BROWSER DEV FB (SHM)", tc);

	char num[11];
	u32_to_dec(num, frame);
	fill_rect_u32(pixels, stride_bytes, 0, 16, 220, 16, 0x80000000u);
	draw_text_u32(pixels, stride_bytes, 8, 20, "FRAME:", tc);
	draw_text_u32(pixels, stride_bytes, 8 + 6 * 8, 20, num, tc);
}

static int run_shm_backend(void)
{
	const char *path = "/dev/shm/cfb0";
	uint32_t width = FB_W;
	uint32_t height = FB_H;
	uint32_t stride = width * 4u;
	size_t total_size = sizeof(struct cfb_header) + cfb_pixel_bytes(width, height);

	int fd = sys_openat(AT_FDCWD, path, O_CREAT | O_RDWR, 0666);
	if (fd < 0) {
		dbg_write("openat /dev/shm/cfb0 failed\n");
		return 1;
	}

	if (sys_ftruncate(fd, (off_t)total_size) < 0) {
		dbg_write("ftruncate failed\n");
		sys_close(fd);
		return 1;
	}

	void *mapped = sys_mmap(0, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		dbg_write("mmap failed\n");
		sys_close(fd);
		return 1;
	}

	struct cfb_header *hdr = (struct cfb_header *)mapped;
	hdr->magic = CFB_MAGIC;
	hdr->version = 2;
	hdr->width = width;
	hdr->height = height;
	hdr->stride_bytes = stride;
	hdr->format = CFB_FORMAT_XRGB8888;
	hdr->input_counter = 0;
	hdr->mouse_x = 0;
	hdr->mouse_y = 0;
	hdr->mouse_buttons = 0;
	hdr->mouse_last_button = 0;
	hdr->mouse_last_state = 0;
	hdr->mouse_last_x = 0;
	hdr->mouse_last_y = 0;
	hdr->mouse_event_counter = 0;
	hdr->frame_counter = 0;

	uint32_t *pixels = (uint32_t *)((uint8_t *)mapped + sizeof(struct cfb_header));

	struct timespec req;
	req.tv_sec = 0;
	req.tv_nsec = 16 * 1000 * 1000; /* ~60fps */

	for (uint32_t frame = 0;; frame++) {
		draw_frame(pixels, width, height, stride, frame);
		hdr->frame_counter = (uint64_t)frame;
		sys_nanosleep(&req, 0);
	}

	/* unreachable */
	/* sys_munmap(mapped, total_size); */
	/* sys_close(fd); */
	return 0;
}

#if defined(BACKEND_FBDEV)
static int run_fbdev_backend(void)
{
	struct fbdev_surface fb;
	int fd = fbdev_open_map(&fb, "/dev/fb0");
	if (fd < 0) {
		return 1;
	}

	if (fb.bpp != 32) {
		dbg_write("fbdev backend currently requires 32bpp\n");
		return 1;
	}

	struct timespec req;
	req.tv_sec = 0;
	req.tv_nsec = 16 * 1000 * 1000;

	uint32_t *pixels = (uint32_t *)fb.map;
	for (uint32_t frame = 0;; frame++) {
		draw_frame(pixels, fb.width, fb.height, fb.stride_bytes, frame);
		sys_nanosleep(&req, 0);
	}

	/* unreachable */
	(void)fd;
	return 0;
}
#endif

int main(void)
{
#if defined(BACKEND_FBDEV)
	return run_fbdev_backend();
#else
	return run_shm_backend();
#endif
}
