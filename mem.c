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

void *
(xcalloc)(size_t n, const char *f)
{
   void *p = calloc(n, 1);
   if (!p) ALLOCERR(f);
   return p;
}

void *
(xrealloc)(void *p, size_t n, const char *f)
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
      *p = xrealloc(0, cap*siz);
      *pcap = -cap;
   }
}

void
vpush_(void **p, int *pcap, uint *pn, uint siz)
{
   if (*pcap >= 0 && *pn >= *pcap) { /* empty or inline buffer */
      int cap = *pcap ? *pcap * 2 : 8;
      void *old = *p;
      *p = xrealloc(NULL, cap * siz);
      if (old) memcpy(*p, old, *pcap * siz);
      *pcap = -cap;
   } else if (*pcap < 0 && *pn >= -*pcap) { /* dyn buf */
      *p = xrealloc(*p, -(*pcap *= 2) * siz);
   } else return;
   while (*pn >= -*pcap)
      *p = xrealloc(*p, -(*pcap *= 2) * siz);
   assert(-(volatile int)*pcap > *pn && "overflow");
}

void *
vpushn_(void **p, int *pcap, uint *pn, uint siz, const void *dat, uint ndat)
{
   void *beg;

   *pn += ndat;
   while (*pn >= (*pcap < 0 ? -*pcap : *pcap))
      vpush_(p, pcap, pn, siz);
   beg = (char *)*p + (*pn - ndat) * siz;
   memcpy(beg, dat, ndat * siz);
   return beg;
}

void
vresize_(void **p, int *pcap, uint *pn, uint siz, uint N)
{
   while ((*pcap < 0 ? -*pcap : *pcap) < N) {
      vpush_(p, pcap, pn, siz);
      *pn = *pcap < 0 ? -*pcap : *pcap;
   }
   *pn = N;
}

struct arena *
newarena(uint chunksiz)
{
   struct arena *ar = xcalloc(offsetof(struct arena, mem) + chunksiz);
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

void *
allocz(struct arena **par, uint siz, uint align)
{
   return memset(alloc(par, siz, align), 0, siz);
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

void
imap_init_(struct imapbase *m, void **v, uint vsiz, uint N)
{
   uint sizk = N*sizeof*m->k,
        sizv = N*vsiz,
        sizbs = BSSIZE(N)*sizeof(struct bitset);

   assert(ispo2(N));
   m->k = xcalloc(sizk + sizv + sizbs);
   *v = (char *)m->k + sizk;
   m->bs = (struct bitset *)((char *)*v + sizv);
   m->N = N;
}

int
imap_get_(struct imapbase *m, short k)
{
   if (!m->N) return -1;
   for (int i = k;; ++i) {
      bool notempty = bstest(m->bs, i &= m->N - 1);
      if (bstest(m->bs, i) && m->k[i] == k) return i;
      if (!notempty) return -1;
   }
}

static void
imap_rehash(struct imapbase *m, void **v, uint vsiz)
{
   short *newk, k;
   int i, j;
   void *newv;
   struct bitset *newbs;
   uint N2 = m->N ? m->N << 1 : 16,
        sizk = N2*sizeof*m->k,
        sizv = N2*vsiz,
        sizbs = BSSIZE(N2)*sizeof(struct bitset);

   assert(N2);
   newk = xcalloc(sizk + sizv + sizbs);
   memset(newk, 0, sizk + sizv + sizbs);
   newv = (char *)newk + sizk;
   newbs = (struct bitset *)((char *)newv + sizv);
   for (i = 0; i < m->N; ++i) {
      if (!bstest(m->bs, i))
         continue;
      j = k = m->k[i];
      for (;; ++j) {
         j &= N2 - 1;
         if (!bstest(newbs, j)) {
            bsset(newbs, j);
            newk[j] = k;
            memcpy((char *)newv + j*vsiz, (char *)*v + i*vsiz, vsiz);
            break;
         }
      }
   }
   free(m->k);
   m->k = newk;
   *v = newv;
   m->bs = newbs;
   m->N = N2;
}

int
imap_set_(struct imapbase *m, void **v, uint vsiz, short k)
{
   if (m->n >= m->N >> 1) {
      imap_rehash(m, v, vsiz);
      assert(m->n < m->N);
   }
   for (int i = k;; ++i) {
      bool notempty = bstest(m->bs, i &= m->N - 1);
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

void
pmap_init_(struct pmapbase *m, void **v, uint vsiz, uint N)
{
   uint sizk = N*sizeof(int),
        sizv = N*vsiz;

   assert(ispo2(N));
   m->k = xcalloc(sizk + sizv);
   *v = (char *)m->k + sizk;
   m->N = N;
}

int pmap_get_(struct pmapbase *m, const void *k)
{
   assert(k && "null key");
   if (!m->N) return -1;
   for (int i = ptrhash(k);; ++i) {
      i &= m->N - 1;
      if (m->k[i] == k) return i;
      if (!m->k[i]) return -1;
   }
}

static void
pmap_rehash(struct pmapbase *m, void **v, uint vsiz)
{
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
      if (!m->k[i])
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

int pmap_set_(struct pmapbase *m, void **v, uint vsiz, const void *k)
{
   assert(k && "null key");
   if (m->n >= m->N >> 1) {
      pmap_rehash(m, v, vsiz);
      assert(m->n < m->N);
   }
   for (int i = ptrhash(k);; ++i) {
      i &= m->N - 1;
      if (m->k[i] == k)
         return i;
      if (!m->k[i]) {
         m->k[i] = (void *)k;
         ++m->n;
         return i;
      }
   }
}

/* vim:set ts=3 sw=3 expandtab: */
