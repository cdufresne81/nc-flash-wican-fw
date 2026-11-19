/* Host-test stub for ESP-IDF's esp_err.h. Values match ESP-IDF so a test that prints
 * one reads the same as a device log. */
#ifndef HOSTTEST_STUB_ESP_ERR_H
#define HOSTTEST_STUB_ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_FAIL        -1
#define ESP_ERR_NO_MEM  0x101
#define ESP_ERR_TIMEOUT 0x107
#endif
