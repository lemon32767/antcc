#pragma once

#include <stddef.h>

static inline size_t
hashs(size_t h, const char *s) {
   while (*s) h = (unsigned char)*s++ + h*65599;
   return h;
}

static inline size_t
hashb(size_t h, const void *d, size_t n) {
   const unsigned char *b = d;
   while (n--) h = *b++ + h*65599;
   return h;
}

static inline size_t
ptrhash(const void *p) {
   return (size_t)p * 2654435761u;
}
