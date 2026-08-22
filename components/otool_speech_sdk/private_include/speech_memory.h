#ifndef OTOOL_SPEECH_MEMORY_H
#define OTOOL_SPEECH_MEMORY_H

#include <stddef.h>

/* Large speech buffers are sequentially accessed and do not require DMA.
 * Prefer external RAM so TLS/WebSocket control state retains scarce internal
 * memory. The implementation transparently falls back to normal 8-bit RAM on
 * targets without usable PSRAM. */
void *otool_speech_alloc_large(size_t size);
void *otool_speech_calloc_large(size_t count, size_t size);

#endif
