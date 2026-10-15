#pragma once

#include <stddef.h>

#define HASH_INIT 0
#define HASH_STEP(h, c) ((unsigned char)(c) + (h)*65599)

static inline size_t
hashs(size_t h, const char *s, unsigned *pn) {
   unsigned n = 0;
   for (; *s; ++n, ++s) h = HASH_STEP(h, *s);
   if (pn) *pn = n;
   return h;
}

static inline size_t
hashb(size_t h, const void *d, size_t n) {
   for (const unsigned char *b = d; n--; ++b) h = HASH_STEP(h, *b);
   return h;
}

static inline size_t
ptrhash(const void *p) {
   return (size_t)p * 2654435761u;
}
