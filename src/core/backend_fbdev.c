#include "backend_fbdev.h"

#include "demo_frame.h"
#include "fbdev.h"

int run_fbdev_backend(void)
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
