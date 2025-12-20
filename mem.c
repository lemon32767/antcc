#include "common.h"
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>

static void
allocerr(const char *f)
{
   efmt("%s: %s\n", f, strerror(errno));
   ioflush(&bstdout);
   ioflush(&bstderr);
   abort();
}

void *
(xmalloc)(size_t n, const char *f)
{
   void *p = malloc(n);
   if (!p) allocerr(f);
   return p;
}

void *
(xcalloc)(size_t n, const char *f)
{
   void *p = calloc(n, 1);
   if (!p) allocerr(f);
   return p;
}

void *
(xrealloc)(void *p, size_t n, const char *f)
{
   p = p ? realloc(p, n) : malloc(n);
   if (!p) allocerr(f);
   return p;
}

/** string interning **/
internstr
intern_(const char *s, uint len)
{
   static uint N, n;
   static struct ht {
      internstr s;
      size_t h;
   } *ht;
   static struct { char m[sizeof(struct arena) + (2<<10)]; struct arena *_a; } amem;
   static struct arena *arena;

   if (!N) {
      ht = xcalloc((sizeof *ht) * (N = 1<<10));
      arena = (void *)amem.m, arena->cap = sizeof amem.m - sizeof(struct arena);
   }

   for (size_t h = len ? hashb(0, s, len) : hashs(0, s), i = h;;) {
      i &= N - 1;
      if (!ht[i].s) { /* insert */
         if (n < N/4*3 /*load factor 75%*/) {
            ++n;
            ht[i].h = h;
            ht[i].s = alloccopy(&arena, s, (len ? len : strlen(s))+1, 1);
            if (len) ((char *)ht[i].s)[len] = 0;
            return ht[i].s;
         }
         /* resize */
         size_t nnew = N * 2;
         struct ht *new = xcalloc(sizeof *new * nnew);
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
         continue;
      } else if (h == ht[i].h) {
         if (!len && !strcmp(s, &ht[i].s->c))
            return ht[i].s;
         else if (len && !strncmp(s, &ht[i].s->c, len) && ht[i].s[len].c == 0)
            return ht[i].s;
      }
      ++i;
   }
}

/** vec **/
void
vinit_(struct vecbase *v, void *inlbuf, uint cap, uint siz)
{
   assert(!v->p);
   v->cap = cap;
   if (inlbuf) {
      v->p = inlbuf;
      v->dyn = 0;
   } else if (cap) {
      v->p = xmalloc(cap*siz);
      v->dyn = 1;
   }
}

void
vpush_(struct vecbase *v, uint siz)
{
   if (v->n < v->cap) return;
   if (!v->dyn && v->n >= v->cap) { /* empty or inline buffer */
      int cap = v->cap ? v->cap * 2 : 8;
      void *old = v->p;
      v->p = xmalloc(cap * siz);
      if (old) memcpy(v->p, old, v->cap * siz);
      v->cap = cap;
      v->dyn = 1;
   } else if (v->dyn && v->n >= v->cap) { /* dyn buf */
      v->p = xrealloc(v->p, (v->cap *= 2) * siz);
   }
   while (v->n >= v->cap)
      v->p = xrealloc(v->p, (v->cap *= 2) * siz);
   assert(v->cap > v->n && "overflow");
}

void *
vpushn_(struct vecbase *v, uint siz, const void *dat, uint ndat)
{
   void *beg;

   if (!ndat) return v->p;
   v->n += ndat;
   while (v->n >= v->cap)
      vpush_(v, siz);
   beg = (char *)v->p + (v->n - ndat) * siz;
   assert(dat);
   memcpy(beg, dat, ndat * siz);
   return beg;
}

void
vresize_(struct vecbase *v, uint siz, uint N)
{
   while (v->cap < N) {
      vpush_(v, siz);
      v->n = v->cap;
   }
   v->n = N;
}

/** arena **/
struct arena *
newarena(uint chunksiz)
{
   struct arena *ar = xmalloc(offsetof(struct arena, mem) + chunksiz);
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
freearena(struct arena **par)
{
   struct arena *prev;
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

/** integer hashmap **/
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
   if (m->n >= m->N/4*3 /*load factor 75*/) {
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

/** pointer hashmap **/
void
pmap_init_(struct pmapbase *m, void **v, uint vsiz, uint N)
{
   assert(ispo2(N));
   uint sizk = N*sizeof*m->k,
        sizv = N*vsiz;

   m->k = xcalloc(sizk + sizv);
   *v = (char *)m->k + sizk;
   m->N = N;
}

int
pmap_get_(struct pmapbase *m, const void *k)
{
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
pmap_set_(struct pmapbase *m, void **v, uint vsiz, const void *k)
{
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
pmap_del_(struct pmapbase *m, const void *k)
{
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
