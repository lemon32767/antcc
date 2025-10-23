#include "ir.h"

static void
porec(struct block ***rpo, struct block *b)
{
   if (wasvisited(b)) return;
   markvisited(b);
   if (b->s2) porec(rpo, b->s2);
   if (b->s1) porec(rpo, b->s1);
   *--*rpo = b;
}

void
sortrpo(struct function *fn)
{
   static struct block **rpobuf;
   struct block **rpoend, **rpo;
   int i, ndead;

   xbgrow(&rpobuf, fn->nblk);
   rpo = rpoend = rpobuf + fn->nblk,

   startbbvisit();
   fn->entry->id = 0;
   porec(&rpo, fn->entry);
   ndead = rpo - rpobuf;
   for (struct block *blk = fn->entry->lprev, *next; ndead > 0; blk = next) {
      next = blk->lprev;
      if (!wasvisited(blk)) {
         for (int i = 0; i < blk->npred; ++i)
            assert(!wasvisited(blkpred(blk, i)));
         blk->lnext = blk->lprev = NULL;
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
}

/* vim:set ts=3 sw=3 expandtab: */
