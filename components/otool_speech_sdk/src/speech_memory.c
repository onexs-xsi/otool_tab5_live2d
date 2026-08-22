#include "speech_memory.h"

#include "esp_heap_caps.h"

#include <stdint.h>
#include <string.h>

void *otool_speech_alloc_large(size_t size)
{
    if (size == 0) return NULL;
    void *memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory == NULL) {
        memory = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return memory;
}

void *otool_speech_calloc_large(size_t count, size_t size)
{
    if (count == 0 || size == 0 || count > SIZE_MAX / size) return NULL;
    size_t total = count * size;
    void *memory = otool_speech_alloc_large(total);
    if (memory != NULL) memset(memory, 0, total);
    return memory;
}
