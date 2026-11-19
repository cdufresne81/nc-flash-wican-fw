/* Host-test stub for ESP-IDF's FreeRTOS.h -- only what the autopid headers name.
 * autopid.h declares a SemaphoreHandle_t member; nothing under test touches it. */
#ifndef HOSTTEST_STUB_FREERTOS_H
#define HOSTTEST_STUB_FREERTOS_H
#include <stdint.h>
typedef void *SemaphoreHandle_t;
#endif
