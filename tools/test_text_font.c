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
	{
		static const uint8_t expect[8] = {0x18, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'!');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x66, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'"');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'\'');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'\\');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'|');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x3C, 0x42, 0x9D, 0xA5, 0x9D, 0x41, 0x3E, 0x00};
		const uint8_t *g = glyph_for((unsigned char)'@');
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x66, 0x18, 0x3C, 0x66, 0x7E, 0x66, 0x66, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xC4);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x66, 0x3C, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xD6);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x66, 0x00, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xDC);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x3C, 0x66, 0x60, 0x7C, 0x66, 0x66, 0x7C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xDF);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x0C, 0x18, 0x7E, 0x60, 0x7C, 0x60, 0x7E, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xE9);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x30, 0x18, 0x18, 0x3C, 0x66, 0x7E, 0x66, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xE0);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x32, 0x4C, 0x66, 0x76, 0x7E, 0x6E, 0x66, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xF1);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x18};
		const uint8_t *g = glyph_for((unsigned char)0xE7);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x66, 0x00, 0x7E, 0x60, 0x7C, 0x60, 0x7E, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xEB);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x18, 0x24, 0x18, 0x3C, 0x66, 0x7E, 0x66, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xE5);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x18, 0x3C, 0x7E, 0x60, 0x7C, 0x60, 0x7E, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xEA);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x18, 0x3C, 0x18, 0x3C, 0x66, 0x7E, 0x66, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xE2);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x0C, 0x18, 0x18, 0x3C, 0x66, 0x7E, 0x66, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xE1);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x30, 0x18, 0x00, 0x18, 0x18, 0x18, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xEC);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x0C, 0x18, 0x00, 0x18, 0x18, 0x18, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xED);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x18, 0x3C, 0x00, 0x18, 0x18, 0x18, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xEE);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x30, 0x18, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xF2);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x18, 0x3C, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xF4);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x32, 0x4C, 0x3C, 0x66, 0x66, 0x66, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xF5);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xF8);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x30, 0x18, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xF9);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x18, 0x24, 0x24, 0x18, 0x00, 0x00, 0x00, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xB0);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x0C, 0x18, 0x7E, 0x60, 0x7C, 0x60, 0x7E, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xC9);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x0C, 0x18, 0x3C, 0x18, 0x18, 0x18, 0x3C, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xCD);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	{
		static const uint8_t expect[8] = {0x3E, 0x6C, 0x6C, 0x7E, 0x6C, 0x6C, 0x6E, 0x00};
		const uint8_t *g = glyph_for((unsigned char)0xC6);
		for (size_t i = 0; i < 8; i++) {
			if (g[i] != expect[i]) return 1;
		}
	}
	return 0;
}
