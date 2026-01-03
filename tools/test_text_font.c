#include "../src/core/text.h"

/* Freestanding regression test: ensure our tiny font defines ';'. */

int main(void)
{
	static const uint8_t expect[8] = {0x00, 0x18, 0x18, 0x00, 0x00, 0x18, 0x18, 0x30};
	const uint8_t *g = glyph_for((unsigned char)';');
	for (size_t i = 0; i < 8; i++) {
		if (g[i] != expect[i]) return 1;
	}
	return 0;
}
