#include "backend_shm.h"

#include "cfb.h"
#include "demo_frame.h"
#include "log.h"

#define FB_W 1920u
#define FB_H 1080u

int run_shm_backend(void)
{
	const char *path = "/dev/shm/cfb0";
	uint32_t width = FB_W;
	uint32_t height = FB_H;
	uint32_t stride = width * 4u;
	size_t total_size = sizeof(struct cfb_header) + cfb_pixel_bytes(width, height);

	int fd = sys_openat(AT_FDCWD, path, O_CREAT | O_RDWR, 0666);
	if (fd < 0) {
		LOGE("shm", "openat /dev/shm/cfb0 failed\n");
		return 1;
	}

	if (sys_ftruncate(fd, (off_t)total_size) < 0) {
		LOGE("shm", "ftruncate failed\n");
		sys_close(fd);
		return 1;
	}

	void *mapped = sys_mmap(0, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		LOGE("shm", "mmap failed\n");
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
	return 0;
}
