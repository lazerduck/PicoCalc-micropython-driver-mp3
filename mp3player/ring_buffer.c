// ring_buffer.c
#include "ring_buffer.h"
#include <stdlib.h>
#include <string.h>
#include "py/misc.h"

static size_t minz(size_t a, size_t b){ return a < b ? a : b; }

bool rb_init(ring_buffer_t* rb, size_t size_bytes) {
    rb->data = (uint8_t*)m_new(uint8_t, size_bytes);
    if (!rb->data) return false;
    rb->size = size_bytes; rb->r = rb->w = 0;
    return true;
}

void rb_free(ring_buffer_t* rb){
    if (rb->data) m_del(uint8_t, rb->data, rb->size);
    rb->data=NULL; rb->size=rb->r=rb->w=0;
}
size_t rb_used_space(const ring_buffer_t* rb){
    return (rb->w >= rb->r) ? (rb->w - rb->r) : (rb->size - (rb->r - rb->w));
}
size_t rb_free_space(const ring_buffer_t* rb){ return rb->size - rb_used_space(rb) - 1; }
size_t rb_write(ring_buffer_t* rb, const void* src, size_t n){
    size_t free1 = rb_free_space(rb); if (n > free1) n = free1;
    size_t tail = rb->size - rb->w;
    size_t n1 = minz(n, tail);
    memcpy(rb->data + rb->w, src, n1);
    size_t n2 = n - n1;
    if (n2) memcpy(rb->data, (const uint8_t*)src + n1, n2);
    rb->w = (rb->w + n) % rb->size;
    return n;
}
size_t rb_read(ring_buffer_t* rb, void* dst, size_t n){
    size_t used = rb_used_space(rb); if (n > used) n = used;
    size_t tail = rb->size - rb->r;
    size_t n1 = minz(n, tail);
    memcpy(dst, rb->data + rb->r, n1);
    size_t n2 = n - n1;
    if (n2) memcpy((uint8_t*)dst + n1, rb->data, n2);
    rb->r = (rb->r + n) % rb->size;
    return n;
}
void rb_clear(ring_buffer_t* rb){ rb->r = rb->w = 0; }
