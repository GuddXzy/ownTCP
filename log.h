#ifndef LOG_H
#define LOG_H

#include <stdio.h>
#include <time.h>

#define LOG_DEBUG 0
#define LOG_INFO  1
#define LOG_WARN  2
#define LOG_ERROR 3

#define LOG_LEVEL LOG_DEBUG

static const char* level_str[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};

#define log_msg(level, fmt, ...) do { \
    if (level >= LOG_LEVEL) { \
        time_t t = time(NULL); \
        struct tm *tm_info = localtime(&t); \
        char timebuf[20]; \
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info); \
        printf("[%s] [%s] " fmt "\n", timebuf, level_str[level], ##__VA_ARGS__); \
    } \
} while(0)

#define log_debug(fmt, ...) log_msg(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_info(fmt,  ...) log_msg(LOG_INFO,  fmt, ##__VA_ARGS__)
#define log_warn(fmt,  ...) log_msg(LOG_WARN,  fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) log_msg(LOG_ERROR, fmt, ##__VA_ARGS__)

#endif
