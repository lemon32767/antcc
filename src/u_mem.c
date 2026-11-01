#include "antcc.h"
#include "u_hash.h"
#include "u_chkarith.h"
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

static void
allocerr(void) {
   efmt("antcc fatal: %s\n", strerror(errno));
   ioflush(&bstdout);
   ioflush(&bstderr);
   abort();
}

void *
xmalloc(size_t n) {
   void *p = malloc(n);
   if (!p) allocerr();
   return p;
}

void *
xcalloc(size_t n) {
   void *p = calloc(n, 1);
   if (!p) allocerr();
   return p;
}

void *
xrealloc(void *p, size_t n) {
   p = p ? realloc(p, n) : malloc(n);
   if (!p) allocerr();
   return p;
}

/** string interning **/
internstr
_intern_hashed(const char *s, uint len, size_t h) {
   static uint N, n;
   static struct Ht {
      internstr s;
      size_t h;
   } *ht;
   static struct { char m[sizeof(Arena) + (2<<10)]; Arena *_a; } amem;
   static Arena *arena;

   if (!N) {
      ht = xcalloc((sizeof *ht) * (N = 1<<10));
      arena = (void *)amem.m, arena->cap = sizeof amem.m - sizeof(Arena);
   }

Lookup:
   for (size_t i = h;; ++i) {
      i &= N - 1;
      if (!ht[i].s) { /* insert */
         if (n < N/4*3 /*load factor 75%*/) {
            ++n;
            ht[i].h = h;
            char *p = (char *)alloc(&arena, len+2, 1) + 1;
            p[-1] = 0; /* tag */
            memcpy(p, s, len);
            p[len] = 0;
            return ht[i].s = (internstr) p;
         }
         /* resize */
         size_t nnew = N * 2;
         struct Ht *new = xcalloc(sizeof *new * nnew);
         for (uint i = 0; i < N; ++i) { /* rehash */
            if (!ht[i].s) continue;
            uint j = ht[i].h;
            while (new[j &= nnew-1].s) ++j;
            new[j] = ht[i];
         }
         free(ht);
         ht = new;
         N = nnew;
         i = h;
         goto Lookup;
      } else if (h == ht[i].h && !strncmp(s, &ht[i].s->c, len) && ht[i].s[len].c == 0) {
         return ht[i].s;
      }
   }
}

internstr
internn(const char *s, uint len) {
   size_t h = len ? hashb(HASH_INIT, s, len) : hashs(HASH_INIT, s, &len);
   return _intern_hashed(s, len, h);
}

/** vec **/
void
vinit_(struct vecbase *v, void *inlbuf, uint cap, uint siz) {
   assert(!v->p);
   v->cap = cap;
   if (inlbuf) {
      v->p = inlbuf;
      v->dyn = 0;
   } else if (cap) {
      v->p = xmalloc(chkx_umul(cap, siz));
      v->dyn = 1;
   }
}

enum { VMINCAP = 8, VMAXCAP = (1u<<31)-1 };

static void
vgrow_(struct vecbase *v, uint elsiz, uint need) {
   if (need <= v->cap) return;
   assert(need <= VMAXCAP && "oom");

   uint cap = v->cap ? v->cap : VMINCAP;
   if (cap < need) {
      cap = cap > VMAXCAP/2 ? VMAXCAP : cap*2;
      if (cap < need) cap = need;
   }

   size_t xcap = chkx_umul(cap, elsiz);
   if (v->dyn && v->p) {
      v->p = xrealloc(v->p, xcap);
   } else { /* empty or inline buffer */
      void *old = v->p;
      v->p = xmalloc(xcap);
      if (old) memcpy(v->p, old, v->n * elsiz); /* mul can't overflow bc. n <= cap */
      v->dyn = 1;
   }
   v->cap = cap;
}
#define vgrow(v, elsiz, need) if ((need) > (v)->cap) vgrow_((v), elsiz, (need))

void
vpush_(struct vecbase *v, uint elsiz) {
   assert(v->n < VMAXCAP && "oom");
   vgrow(v, elsiz, v->n + 1);
}

void *
vpushn_(struct vecbase *v, uint elsiz, const void *dat, uint ndat) {
   if (!ndat) return v->p;
   assert(dat);
   uint dbeg = v->n,
        dend = chkx_uadd(v->n, ndat);
   vgrow(v, elsiz, dend);
   v->n = dend;

   return memcpy((char *)v->p + dbeg*elsiz, dat, ndat*elsiz);
}

void
vresize_(struct vecbase *v, uint elsiz, uint N) {
   vgrow(v, elsiz, N);
   v->n = N;
}

/** arena **/
Arena *
newarena(uint chunksiz) {
   Arena *ar = xmalloc(offsetof(Arena, mem) + chunksiz);
   assert(chunksiz < 1u<<31 && "toobig");
   ar->prev = NULL;
   ar->cap = chunksiz;
   ar->dyn = 1;
   ar->n = 0;
   return ar;
}

void *
alloc(Arena **par, uint siz, uint align) {
   uint idx;
   Arena *new;

   if (siz > (*par)->cap) {
      new = newarena(siz);
      new->n = siz;
      new->prev = (*par)->prev;
      (*par)->prev = new;
      return new->mem;
   }
   if (align) assert(ispo2(align) && align <= sizeof(void *));
   else align = sizeof(void *);

   idx = (uchar *)alignup((uintptr_t)&(*par)->mem[(*par)->n], align) - (*par)->mem;
   if ((*par)->cap - idx >= siz) {
      (*par)->n = idx + siz;
      return (*par)->mem + idx;
   }
   uint newcap = (*par)->cap;
   if (newcap < 1<<26) newcap *= 2;
   new = newarena(newcap);
   new->prev = *par;
   *par = new;
   new->n = siz;
   return new->mem;
}

void *
allocz(Arena **par, uint siz, uint align) {
   return memset(alloc(par, siz, align), 0, siz);
}

void
freearena(Arena **par) {
   Arena *prev;
   for (; *par; *par = prev) {
      prev = (*par)->prev;
      if ((*par)->dyn)
         free(*par);
      else {
         if (prev) freearena(&prev);
         (*par)->prev = NULL;
         (*par)->n = 0;
         return;
      }
   }
}

/** pointer hashmap **/
void
pmap_init_(struct pmapbase *m, void **v, uint vsiz, uint N) {
   assert(ispo2(N));
   uint sizk = N*sizeof*m->k,
        sizv = N*vsiz;

   m->k = xcalloc(sizk + sizv);
   *v = (char *)m->k + sizk;
   m->N = N;
}

int
pmap_get_(struct pmapbase *m, const void *k) {
   assert(k && "null key");
   if (!m->N) return -1;
   for (size_t i = ptrhash(k);; ++i) {
      i &= m->N - 1;
      if (m->k[i] == k) return i;
      if (!m->k[i]) return -1;
   }
}

char pmap_tombstone_[1];

static void
pmap_rehash(struct pmapbase *m, void **v, uint vsiz) {
   void **newk;
   int i, j;
   void *k;
   void *newv;
   uint N2 = m->N ? m->N << 1 : 16,
        sizk = N2*sizeof*m->k,
        sizv = N2*vsiz;

   assert(N2);
   newk = xcalloc(sizk + sizv);
   newv = (char *)newk + sizk;
   for (i = 0; i < m->N; ++i) {
      if (!m->k[i] || m->k[i] == pmap_tombstone_)
         continue;
      j = ptrhash(k = m->k[i]);
      for (;; ++j) {
         j &= N2 - 1;
         if (!newk[j]) {
            newk[j] = k;
            memcpy((char *)newv + j*vsiz, (char *)*v + i*vsiz, vsiz);
            break;
         }
      }
   }
   free(m->k);
   m->k = newk;
   *v = newv;
   m->N = N2;
}

int
pmap_set_(struct pmapbase *m, void **v, uint vsiz, const void *k) {
   assert(k && "null key");
   if (m->n >= m->N/4*3 /*load factor 75%*/) {
      pmap_rehash(m, v, vsiz);
      assert(m->n < m->N);
   }
   for (size_t i = ptrhash(k);; ++i) {
      i &= m->N - 1;
      if (m->k[i] == k)
         return i;
      if (!m->k[i] || m->k[i] == pmap_tombstone_) {
         m->k[i] = (void *)k;
         ++m->n;
         return i;
      }
   }
}

void
pmap_del_(struct pmapbase *m, const void *k) {
   assert(k && "null key");
   for (size_t i = ptrhash(k);; ++i) {
      i &= m->N - 1;
      if (m->k[i] == k) {
         m->k[i] = pmap_tombstone_;
         --m->n;
         return;
      } else if (!m->k[i])
         return;
   }
}

/* vim:set ts=3 sw=3 expandtab: */
