#pragma once

#include "../core/syscall.h"

#define FB_W 1920u
#define FB_H 1080u

#define UI_TOPBAR_H 24u
#define UI_CONTENT_PAD_Y 2u
#define UI_CONTENT_Y0 (UI_TOPBAR_H + UI_CONTENT_PAD_Y)

enum {
	HOST_BUF_LEN = 128,
	PATH_BUF_LEN = 2048,
	URL_BUF_LEN = 2304,
};
