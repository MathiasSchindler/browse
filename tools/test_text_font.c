#include "../src/core/text.h"

/* Freestanding regression test: ensure our tiny font defines some punctuation. */

int main(void)
{
	{
		static const uint8_t expect[8] = {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30};
		const uint8_t *g = glyph_for((unsigned char)';');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30};
		const uint8_t *g = glyph_for((unsigned char)',');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x3C, 0x66, 0x06, 0x0C, 0x18, 0x00, 0x18, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'?');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x62, 0x64, 0x08, 0x10, 0x26, 0x46, 0x00, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'%');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x1C, 0x22, 0x14, 0x18, 0x25, 0x22, 0x1D, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'&');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	return 0;
}
