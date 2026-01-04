#pragma once

#include "syscall.h"

/* Minimal structured logging (no libc).
 *
 * LOG_LEVEL:
 *   0 = ERROR only
 *   1 = WARN + ERROR
 *   2 = INFO + WARN + ERROR (default)
 *   3 = DEBUG + INFO + WARN + ERROR
 *
 * LOG_COLOR:
 *   0 = no ANSI colors (default)
 *   1 = colored level prefix
 */

#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#ifndef LOG_COLOR
#define LOG_COLOR 0
#endif

enum log_level {
	LOG_LVL_ERROR = 0,
	LOG_LVL_WARN = 1,
	LOG_LVL_INFO = 2,
	LOG_LVL_DEBUG = 3,
};

static inline void log__write_lit(const char *s)
{
	sys_write(2, s, c_strlen(s));
}

static inline void log__prefix(enum log_level lvl, const char *tag)
{
	(void)lvl;
	if (LOG_COLOR) {
		switch (lvl) {
		case LOG_LVL_ERROR: log__write_lit("\x1b[31m"); break;
		case LOG_LVL_WARN: log__write_lit("\x1b[33m"); break;
		case LOG_LVL_INFO: log__write_lit("\x1b[32m"); break;
		case LOG_LVL_DEBUG: log__write_lit("\x1b[36m"); break;
		default: break;
		}
	}

	switch (lvl) {
	case LOG_LVL_ERROR: log__write_lit("[ERROR]"); break;
	case LOG_LVL_WARN: log__write_lit("[WARN]"); break;
	case LOG_LVL_INFO: log__write_lit("[INFO]"); break;
	case LOG_LVL_DEBUG: log__write_lit("[DEBUG]"); break;
	default: log__write_lit("[LOG]"); break;
	}

	if (LOG_COLOR) {
		log__write_lit("\x1b[0m");
	}

	log__write_lit(" ");
	if (tag && tag[0]) {
		log__write_lit(tag);
		log__write_lit(": ");
	}
}

static inline void log__maybe_newline(const char *buf, size_t n)
{
	if (buf && n > 0 && buf[n - 1] == '\n') return;
	log__write_lit("\n");
}

static inline void log_write(enum log_level lvl, const char *tag, const char *buf, size_t n)
{
	log__prefix(lvl, tag);
	if (buf && n) sys_write(2, buf, n);
	log__maybe_newline(buf, n);
}

static inline void log_cstr(enum log_level lvl, const char *tag, const char *s)
{
	log__prefix(lvl, tag);
	if (s) {
		size_t n = c_strlen(s);
		if (n) sys_write(2, s, n);
		log__maybe_newline(s, n);
	} else {
		log__maybe_newline(0, 0);
	}
}

#define LOGE(tag, msg) do { if (LOG_LEVEL >= 0) log_cstr(LOG_LVL_ERROR, (tag), (msg)); } while (0)
#define LOGW(tag, msg) do { if (LOG_LEVEL >= 1) log_cstr(LOG_LVL_WARN, (tag), (msg)); } while (0)
#define LOGI(tag, msg) do { if (LOG_LEVEL >= 2) log_cstr(LOG_LVL_INFO, (tag), (msg)); } while (0)
#define LOGD(tag, msg) do { if (LOG_LEVEL >= 3) log_cstr(LOG_LVL_DEBUG, (tag), (msg)); } while (0)

#define LOGW_BUF(tag, buf, n) do { if (LOG_LEVEL >= 1) log_write(LOG_LVL_WARN, (tag), (buf), (n)); } while (0)
#define LOGE_BUF(tag, buf, n) do { if (LOG_LEVEL >= 0) log_write(LOG_LVL_ERROR, (tag), (buf), (n)); } while (0)
#define LOGI_BUF(tag, buf, n) do { if (LOG_LEVEL >= 2) log_write(LOG_LVL_INFO, (tag), (buf), (n)); } while (0)
#define LOGD_BUF(tag, buf, n) do { if (LOG_LEVEL >= 3) log_write(LOG_LVL_DEBUG, (tag), (buf), (n)); } while (0)
