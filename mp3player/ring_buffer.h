// ring_buffer.h
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t* data;
    size_t   size;   // bytes
    size_t   r;      // read index
    size_t   w;      // write index
} ring_buffer_t;

bool  rb_init(ring_buffer_t* rb, size_t size_bytes);
void  rb_free(ring_buffer_t* rb);
size_t rb_free_space(const ring_buffer_t* rb); // bytes
size_t rb_used_space(const ring_buffer_t* rb); // bytes
size_t rb_write(ring_buffer_t* rb, const void* src, size_t nbytes);
size_t rb_read(ring_buffer_t* rb, void* dst, size_t nbytes);
void   rb_clear(ring_buffer_t* rb);
