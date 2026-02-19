#include "ir.h"

static void
porec(int *nblk, struct block ***rpo, struct block *b)
{
   if (wasvisited(b)) return;
   assert(*nblk > 0 && "nblk bad");
   --*nblk;
   markvisited(b);
   if (b->s2) porec(nblk, rpo, b->s2);
   if (b->s1) porec(nblk, rpo, b->s1);
   *--*rpo = b;
}

/* also blkid */
void
sortrpo(struct function *fn)
{
   static struct block **rpobuf;
   struct block **rpoend, **rpo, *blk, *next;
   int i, ndead;

   xbgrow(&rpobuf, fn->nblk);
   rpo = rpoend = rpobuf + fn->nblk,

   startbbvisit();
   fn->entry->id = 0;
   int nblk = fn->nblk;
   porec(&nblk, &rpo, fn->entry);
   ndead = rpo - rpobuf;
   if (ndead > 0) for (blk = fn->entry->lprev; blk != fn->entry; blk = next) {
      next = blk->lprev;
      if (!wasvisited(blk)) {
         for (int i = 0; i < blk->ins.n; ++i) {
            /* if unreachable block has alloca pseudo-instrs, move them to the entry
             * to be able to delete it */
            if (oisalloca(instrtab[blk->ins.p[i]].op)) {
               vpush(&fn->entry->ins, blk->ins.p[i]);
            }
         }
         for (int i = 0; i < blk->npred; ++i)
            assert(!wasvisited(blkpred(blk, i)));
         freeblk(fn, blk);
         --ndead;
      }
   }
   for (i = 1, ++rpo; rpo < rpoend; ++rpo, ++i) {
      rpo[-1]->lnext = rpo[0];
      rpo[0]->lprev = rpo[-1];
      rpo[0]->id = i;
   }
   fn->entry->lprev = rpo[-1];
   rpo[-1]->lnext = fn->entry;

   fn->prop |= FNBLKID | FNRPO;
}

/* also blkid */
void
filldom(struct function *fn)
{
   struct block *blk = fn->entry;
   int i = 0;

   FREQUIRE(FNRPO);

   /* Implements 'A Simple, Fast Dominance Algorithm' by K. Cooper, T. Harvey, and K. Kennedy */
   do blk->id = i++, blk->idom = NULL; while ((blk = blk->lnext) != fn->entry);
   fn->entry->idom = fn->entry;
   for (bool changed = 1; changed;) {
      changed = 0;
      do {
         int j;
         struct block *new = NULL;
         if (blk->npred == 0) continue;
         for (j = 0; j < blk->npred; ++j)
            if ((new = blkpred(blk, j))->id < blk->id) break;
         assert(new);
         for (int i = 0; i < blk->npred; ++i) {
            if (i == j) continue;
            struct block *p = blkpred(blk, i);
            if (p->idom) { /* new = intersect(p, new) */
               while (p != new) {
                  while (p->id > new->id) p = p->idom;
                  while (p->id < new->id) new = new->idom;
               }
            }
         }
         if (blk->idom != new) {
            blk->idom = new;
            changed = 1;
         }
      } while ((blk = blk->lnext) != fn->entry);
   }
   fn->prop |= FNBLKID | FNDOM;
}

/* vim:set ts=3 sw=3 expandtab: */
