#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL2/SDL.h>

/* Must match src/core/cfb.h */
enum {
	CFB_MAGIC = 0x30424643u,
	CFB_FORMAT_XRGB8888 = 1,
};

struct cfb_header {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t stride_bytes;
	uint32_t format;
	uint32_t reserved0;
	uint32_t reserved1;
	uint64_t frame_counter;
	uint64_t reserved2;
	uint64_t reserved3;
	uint64_t input_counter;
	uint32_t mouse_x;
	uint32_t mouse_y;
	uint32_t mouse_buttons;
	uint32_t mouse_last_button;
	uint32_t mouse_last_state;
	uint32_t mouse_last_x;
	uint32_t mouse_last_y;
	uint64_t mouse_event_counter;
};

enum {
	CFB_HEADER_V1_SIZE = 56, /* up to reserved3 inclusive */
};

static int map_file(const char *path, int *io_fd, void **io_map, size_t *io_len, ino_t *io_ino)
{
	if (*io_map && *io_len) {
		munmap(*io_map, *io_len);
		*io_map = NULL;
		*io_len = 0;
	}
	if (*io_fd >= 0) {
		close(*io_fd);
		*io_fd = -1;
	}

	int fd = open(path, O_RDWR);
	if (fd < 0) {
		return -1;
	}

	struct stat st;
	if (fstat(fd, &st) != 0) {
		close(fd);
		return -1;
	}
	if (st.st_size <= 0) {
		close(fd);
		errno = EINVAL;
		return -1;
	}

	void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return -1;
	}

	*io_fd = fd;
	*io_map = p;
	*io_len = (size_t)st.st_size;
	*io_ino = st.st_ino;
	return 0;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	const char *path = "/dev/shm/cfb0";

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Window *win = SDL_CreateWindow(
		"cfb viewer",
		SDL_WINDOWPOS_CENTERED,
		SDL_WINDOWPOS_CENTERED,
		1920,
		1080,
		SDL_WINDOW_RESIZABLE);
	if (!win) {
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Renderer *renderer = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
	if (!renderer) {
		fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
		return 1;
	}

	SDL_Texture *texture = NULL;
	int fb_fd = -1;
	void *mapped = NULL;
	size_t mapped_len = 0;
	ino_t mapped_ino = 0;
	uint64_t last_frame = (uint64_t)-1;

	for (;;) {
		SDL_Event ev;
		while (SDL_PollEvent(&ev)) {
			if (ev.type == SDL_QUIT) {
				goto out;
			}
			if (ev.type == SDL_MOUSEWHEEL && mapped && mapped_len >= CFB_HEADER_V1_SIZE) {
				struct cfb_header *hdr = (struct cfb_header *)mapped;
				if (hdr->magic == CFB_MAGIC && hdr->format == CFB_FORMAT_XRGB8888) {
					/* Only write into shm if the header supports it (v2+). */
					if (hdr->version >= 2 && mapped_len >= sizeof(struct cfb_header)) {
						/* Reuse reserved fields (reserved0=wheel delta y, reserved2=wheel counter). */
						hdr->reserved0 = (uint32_t)ev.wheel.y;
						hdr->reserved2++;
						hdr->input_counter++;
					}
					fprintf(stderr, "mouse wheel y=%d\n", ev.wheel.y);
					fflush(stderr);
				}
			}
			if ((ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) && mapped && mapped_len >= CFB_HEADER_V1_SIZE) {
				struct cfb_header *hdr = (struct cfb_header *)mapped;
				if (hdr->magic == CFB_MAGIC && hdr->format == CFB_FORMAT_XRGB8888) {
					int win_w = 1, win_h = 1;
					SDL_GetWindowSize(win, &win_w, &win_h);
					if (win_w <= 0) win_w = 1;
					if (win_h <= 0) win_h = 1;

					/* Map window coords -> framebuffer coords (texture is stretched to window). */
					uint32_t fb_x = (uint32_t)((uint64_t)ev.button.x * (uint64_t)hdr->width / (uint64_t)win_w);
					uint32_t fb_y = (uint32_t)((uint64_t)ev.button.y * (uint64_t)hdr->height / (uint64_t)win_h);
					if (fb_x >= hdr->width) fb_x = hdr->width ? (hdr->width - 1u) : 0u;
					if (fb_y >= hdr->height) fb_y = hdr->height ? (hdr->height - 1u) : 0u;

					/* Only write into shm if the header supports it (v2+). */
					if (hdr->version >= 2 && mapped_len >= sizeof(struct cfb_header)) {
						hdr->mouse_x = fb_x;
						hdr->mouse_y = fb_y;
						hdr->mouse_last_x = fb_x;
						hdr->mouse_last_y = fb_y;
						hdr->mouse_last_button = (uint32_t)ev.button.button;
						hdr->mouse_last_state = (ev.type == SDL_MOUSEBUTTONDOWN) ? 1u : 0u;
						hdr->mouse_event_counter++;
						hdr->input_counter++;

						/* Maintain a simple bitmask for current button state. */
						uint32_t bit = 0;
						switch (ev.button.button) {
						case SDL_BUTTON_LEFT: bit = 1u << 0; break;
						case SDL_BUTTON_MIDDLE: bit = 1u << 1; break;
						case SDL_BUTTON_RIGHT: bit = 1u << 2; break;
						default: bit = 0; break;
						}
						if (bit) {
							if (ev.type == SDL_MOUSEBUTTONDOWN) hdr->mouse_buttons |= bit;
							else hdr->mouse_buttons &= ~bit;
						}
					}

					fprintf(stderr, "mouse %s button=%u at window=(%d,%d) fb=(%u,%u)\n",
						(ev.type == SDL_MOUSEBUTTONDOWN) ? "down" : "up",
						(unsigned)ev.button.button,
						ev.button.x,
						ev.button.y,
						(unsigned)fb_x,
						(unsigned)fb_y);
					fflush(stderr);
				}
			}
		}

		if (!mapped) {
			if (map_file(path, &fb_fd, &mapped, &mapped_len, &mapped_ino) != 0) {
				SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
				SDL_RenderClear(renderer);
				SDL_RenderPresent(renderer);
				SDL_Delay(50);
				continue;
			}
		}

		/* Detect if producer replaced or resized the file and remap in-place. */
		struct stat st;
		if (fb_fd >= 0 && fstat(fb_fd, &st) == 0) {
			if (st.st_ino != mapped_ino || (size_t)st.st_size != mapped_len) {
				if (texture) {
					SDL_DestroyTexture(texture);
					texture = NULL;
				}
				last_frame = (uint64_t)-1;
				if (map_file(path, &fb_fd, &mapped, &mapped_len, &mapped_ino) != 0) {
					SDL_Delay(50);
					continue;
				}
			}
		}

		if (mapped_len < sizeof(struct cfb_header)) {
			map_file(path, &fb_fd, &mapped, &mapped_len, &mapped_ino);
			SDL_Delay(50);
			continue;
		}

		struct cfb_header *hdr = (struct cfb_header *)mapped;
		if (hdr->magic != CFB_MAGIC || hdr->format != CFB_FORMAT_XRGB8888) {
			map_file(path, &fb_fd, &mapped, &mapped_len, &mapped_ino);
			SDL_Delay(50);
			continue;
		}

		const uint32_t width = hdr->width;
		const uint32_t height = hdr->height;
		const uint32_t stride = hdr->stride_bytes;
		size_t pixels_off = sizeof(struct cfb_header);
		if (hdr->version < 2) pixels_off = CFB_HEADER_V1_SIZE;
		const size_t need = pixels_off + (size_t)height * (size_t)stride;
		if (need > mapped_len || width == 0 || height == 0 || stride < width * 4u) {
			map_file(path, &fb_fd, &mapped, &mapped_len, &mapped_ino);
			SDL_Delay(50);
			continue;
		}

		if (!texture) {
			texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)width, (int)height);
			if (!texture) {
				fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
				goto out;
			}
		}

		if (hdr->frame_counter != last_frame) {
			last_frame = hdr->frame_counter;
			const void *pixels = (const uint8_t *)mapped + pixels_off;
			SDL_UpdateTexture(texture, NULL, pixels, (int)stride);
		}

		SDL_RenderClear(renderer);
		SDL_RenderCopy(renderer, texture, NULL, NULL);
		SDL_RenderPresent(renderer);
		SDL_Delay(1);
	}

out:
	if (mapped && mapped_len) {
		munmap(mapped, mapped_len);
		mapped = NULL;
		mapped_len = 0;
	}
	if (fb_fd >= 0) {
		close(fb_fd);
		fb_fd = -1;
	}
	if (texture) {
		SDL_DestroyTexture(texture);
	}
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return 0;
}
