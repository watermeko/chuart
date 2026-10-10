#ifndef CHUART_LOG_H
#define CHUART_LOG_H

#include <stdarg.h>

enum log_level {
	LOG_LEVEL_ERROR = 1,
	LOG_LEVEL_WARN = 2,
	LOG_LEVEL_INFO = 3,
	LOG_LEVEL_DEBUG = 4,
};

void log_set_level(enum log_level level);
enum log_level log_get_level(void);

void log_msg(enum log_level level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define log_error(...) log_msg(LOG_LEVEL_ERROR, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_LEVEL_WARN, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_LEVEL_INFO, __VA_ARGS__)
#define log_debug(...)                                                         \
	do {                                                                   \
		if (log_get_level() >= LOG_LEVEL_DEBUG)                        \
			log_msg(LOG_LEVEL_DEBUG, __VA_ARGS__);                 \
	} while (0)

#endif /* CHUART_LOG_H */
