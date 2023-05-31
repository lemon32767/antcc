#include "common.h"
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

#define ALLOCERR(f) do {                  \
   efmt("%s: %s\n", f, strerror(errno)); \
   ioflush(&bstdout);                    \
   ioflush(&bstderr);                    \
   abort();                              \
} while (0)

static void *
xcalloc(size_t n, const char *f)
{
   void *p = calloc(n, 1);
   if (!p) ALLOCERR(f);
   return p;
}

static void *
xrealloc(void *p, size_t n, const char *f)
{
   p = p ? realloc(p, n) : malloc(n);
   if (!p) ALLOCERR(f);
   return p;
}

/* vec: when _cap < 0, buf is dynamic allocated, otherwise an user provided buf */

void
vinit_(void **p, int *pcap, void *inlbuf, int cap, uint siz)
{
   assert(!*p);
   *pcap = cap;
   if (inlbuf) {
      *p = inlbuf; 
   } else if (cap) {
      *p = xrealloc(0, cap*siz, "vinit");
      *pcap = -cap;
   }
}

void
vpush_(void **p, int *pcap, uint *pn, uint siz)
{
   if (*pn == *pcap) { /* empty or inline buffer */
      int cap = *pcap ? *pcap * 2 : 8;
      *p = xrealloc(NULL, cap * siz, "vpush");
      *pcap = -cap;
   } else if (*pn == -*pcap) { /* dyn buf */
      *p = xrealloc(*p, -(*pcap *= 2) * siz, "vpush");
   }
   assert(-(volatile int)*pcap > *pn && "overflow");
}

void *
vpushn_(void **p, int *pcap, uint *pn, uint siz, const void *dat, uint ndat)
{
   void *beg;

   for (uint i = 0; i < ndat; ++i)
      vpush_(p, pcap, pn, siz);
   beg = (char *)*p + *pn * siz;
   memcpy(beg, dat, ndat * siz);
   *pn += ndat;
   return beg;
}

struct arena *
newarena(uint chunksiz)
{
   struct arena *ar = xcalloc(offsetof(struct arena, mem) + chunksiz, "newarena");
   assert(chunksiz < 1u<<31 && "toobig");
   ar->cap = chunksiz;
   ar->dyn = 1;
   return ar;
}

void *
alloc(struct arena **par, uint siz, uint align)
{
   uint idx;
   struct arena *new;
   
   if (siz > (*par)->cap) {
      new = newarena(siz);
      new->n = siz;
      new->prev = (*par)->prev;
      (*par)->prev = new;
      return new->mem;
   }
   align = align ? align : sizeof(void *);
   idx = (uchar *)alignup((uintptr_t)&(*par)->mem[(*par)->n], align) - (*par)->mem;
   if ((*par)->cap - idx >= siz) {
      (*par)->n = idx + siz;
      return (*par)->mem + idx;
   }
   new = newarena((*par)->cap);
   new->prev = *par;
   *par = new;
   new->n = siz;
   return new->mem;
}

void
freearena(struct arena *ar)
{
   struct arena *prev;
   for (; ar; ar = prev) {
      prev = ar->prev;
      if (ar->dyn)
         free(ar);
      else {
         assert(!prev);
         ar->n = 0;
      }
   }
}

#if 0

int
map_get_(struct mapbase *m, int k)
{
   if (!m->N) return 0;
   for (int i = k;; ++i) {
      bool notempty = bstest(m->bs, k);
      i &= m->N - 1;
      if (notempty && m->k[i] == k)
         return i;
      if (!notempty)
         return -1;
   }
}


void
map_init_(struct mapbase *m, void **v, uint vsiz, uint N)
{
   uint sizk = N*sizeof(int),
        sizv = N*vsiz,
        sizbs = BSCOUNT(N)*sizeof(struct bitset);

   assert(N && (N & (N - 1)) == 0);
   m->k = xcalloc(sizk + sizv + sizbs, "map_rehash");
   *v = (char *)m->k + sizk;
   m->bs = (struct bitset *)((char *)*v + sizv);
   m->N = N;
}

static void
map_rehash(struct mapbase *m, void **v, uint vsiz)
{
   int *newk, i, k, j;
   void *newv;
   struct bitset *newbs;
   uint N2 = m->N << 1,
        sizk = N2*sizeof(int),
        sizv = N2*vsiz,
        sizbs = BSCOUNT(N2)*sizeof(struct bitset);

   assert(N2);
   newk = xrealloc(NULL, sizk + sizv + sizbs, "map_rehash");
   newv = (char *)newk + sizk;
   newbs = (struct bitset *)((char *)newv + sizv);
   for (i = 0; i < m->N; ++i) {
      if (!bstest(m->bs, i))
         continue;
      j = k = m->k[i];
      for (;; ++j) {
         j &= N2 - 1;
         if (!bstest(newbs, i)) {
            bsset(newbs, i);
            m->k[j] = k;
            memcpy((char *)newv + j*vsiz, (char *)*v + i*vsiz, vsiz);
            break;
         }
      }
   }
   free(m->k);
   free(*v);
   free(m->bs);
   m->k = newk;
   *v = newv;
   m->bs = newbs;
   m->N = N2;
}

int
map_set_(struct mapbase *m, void **v, uint vsiz, int k)
{
   if (!m->N) return 0;
   if (m->n >= m->N >> 1) {
      map_rehash(m, v, vsiz);
      assert(m->n < m->N);
   }
   for (int i = k;; ++i) {
      bool notempty = bstest(m->bs, k);
      i &= m->N - 1;
      if (notempty && m->k[i] == k)
         return i;
      if (!notempty) {
         m->k[i] = k;
         bsset(m->bs, i);
         ++m->n;
         return i;
      }
   }
}

#endif

/* vim:set ts=3 sw=3 expandtab: */
