#define _GNU_SOURCE
#include "log.h"

#include "ui.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static enum log_level g_level = LOG_LEVEL_INFO;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

void log_set_level(enum log_level level)
{
	g_level = level;
}

enum log_level log_get_level(void)
{
	return g_level;
}

static const char *level_tag(enum log_level level)
{
	switch (level) {
	case LOG_LEVEL_ERROR:
		return "ERR";
	case LOG_LEVEL_WARN:
		return "WRN";
	case LOG_LEVEL_INFO:
		return "INF";
	default:
		return "DBG";
	}
}

void log_msg(enum log_level level, const char *fmt, ...)
{
	struct timeval tv;
	struct tm tm;
	char stamp[32];
	va_list ap;

	if (level > g_level)
		return;

	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm);
	snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour,
		 tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));

	const char *tag_color = level == LOG_LEVEL_ERROR   ? ui_ec("31")
				: level == LOG_LEVEL_WARN  ? ui_ec("33")
				: level == LOG_LEVEL_DEBUG ? ui_ec("2")
							   : ui_ec("36");

	pthread_mutex_lock(&g_lock);
	fprintf(stderr, "%s[%s]%s %s%s%s ", ui_ec("2"), stamp, ui_ec("0"),
		tag_color, level_tag(level), ui_ec("0"));
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
	pthread_mutex_unlock(&g_lock);
}
