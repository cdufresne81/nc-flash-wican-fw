/* Host-test stub for ESP-IDF's esp_heap_caps.h. The firmware frees heap_caps_* memory
 * with plain free(), so these must be the libc allocator -- which is also what lets
 * AddressSanitizer see every parser allocation and report a leak or overflow. */
#ifndef HOSTTEST_STUB_ESP_HEAP_CAPS_H
#define HOSTTEST_STUB_ESP_HEAP_CAPS_H
#include <stdlib.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT     (1u << 2)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}
static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(n, size);
}
static inline void heap_caps_free(void *p)
{
    free(p);
}
#endif
