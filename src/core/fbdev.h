#pragma once

#include "syscall.h"

/* Uses the kernel's fbdev API (/dev/fb0). This is the "flip the switch" backend.
 * Note: Many modern systems use DRM/KMS instead; fbdev might be unavailable.
 */

#include <linux/fb.h>

struct fbdev_surface {
	void *map;
	size_t map_len;
	uint32_t width;
	uint32_t height;
	uint32_t stride_bytes;
	uint32_t bpp;
};

static inline int fbdev_open_map(struct fbdev_surface *out, const char *path)
{
	int fd = sys_openat(AT_FDCWD, path, O_RDWR, 0);
	if (fd < 0) {
		dbg_write("openat /dev/fb0 failed\n");
		return -1;
	}

	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;

	if (sys_ioctl(fd, (ulong)FBIOGET_VSCREENINFO, &var) < 0) {
		dbg_write("ioctl FBIOGET_VSCREENINFO failed\n");
		sys_close(fd);
		return -1;
	}

	if (sys_ioctl(fd, (ulong)FBIOGET_FSCREENINFO, &fix) < 0) {
		dbg_write("ioctl FBIOGET_FSCREENINFO failed\n");
		sys_close(fd);
		return -1;
	}

	/* Basic expectations: 32bpp packed pixels. */
	out->width = (uint32_t)var.xres;
	out->height = (uint32_t)var.yres;
	out->stride_bytes = (uint32_t)fix.line_length;
	out->bpp = (uint32_t)var.bits_per_pixel;
	out->map_len = (size_t)fix.smem_len;

	if (out->width == 0 || out->height == 0 || out->stride_bytes == 0 || out->map_len == 0) {
		dbg_write("fbdev reported invalid geometry\n");
		sys_close(fd);
		return -1;
	}

	void *mapped = sys_mmap(0, out->map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		dbg_write("mmap fbdev failed\n");
		sys_close(fd);
		return -1;
	}

	out->map = mapped;
	/* We intentionally do not close fd here: some drivers require it kept open. */
	return fd;
}
