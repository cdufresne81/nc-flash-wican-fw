/* Host-test stub for ESP-IDF's esp_log.h. Silent by default so test output stays the
 * pass/fail lines; the arguments stay type-checked against the format (and stay "used",
 * so -Wunused-parameter does not trip on a parameter that only feeds a log line).
 * Build with -DHOSTTEST_LOG to see the parser's warnings on stderr. */
#ifndef HOSTTEST_STUB_ESP_LOG_H
#define HOSTTEST_STUB_ESP_LOG_H
#include <stdio.h>
#ifdef HOSTTEST_LOG
#define HOSTTEST_LOG_ON 1
#else
#define HOSTTEST_LOG_ON 0
#endif
#define HOSTTEST_LOGX(lvl, tag, fmt, ...) \
    do { if (HOSTTEST_LOG_ON) fprintf(stderr, lvl " (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) HOSTTEST_LOGX("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOSTTEST_LOGX("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOSTTEST_LOGX("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) HOSTTEST_LOGX("D", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) HOSTTEST_LOGX("V", tag, fmt, ##__VA_ARGS__)
#endif
