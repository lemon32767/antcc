#include "ir.h"

static void
porec(int *nblk, Block ***rpo, Block *b) {
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
sortrpo(Function *fn) {
   static Block **rpobuf;
   Block **rpoend, **rpo, *blk, *next;
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


static void
gcmark(Block *b) {
   if (wasvisited(b)) return;
   markvisited(b);
   if (b->s2) gcmark(b->s2);
   if (b->s1) gcmark(b->s1);
}

void
deldeadblks(Function *fn) {
   Block *b = fn->entry, *next;
   startbbvisit();
   gcmark(b);
   do {
      next = b->lnext;
      if (!wasvisited(b)) freeblk(fn, b);
   } while ((b = next) != fn->entry);
}

/* also blkid */
void
filldom(Function *fn) {
   Block *b = fn->entry;
   int i = 0;

   FREQUIRE(FNRPO);

   /* Implements 'A Simple, Fast Dominance Algorithm' by K. Cooper, T. Harvey, and K. Kennedy */
   do b->id = i++, b->idom = NULL; while ((b = b->lnext) != fn->entry);
   fn->entry->idom = fn->entry;
   for (bool changed = 1; changed;) {
      changed = 0;
      do {
         int j;
         Block *new = NULL;
         if (b->npred == 0) continue;
         for (j = 0; j < b->npred; ++j)
            if ((new = blkpred(b, j))->id < b->id) break;
         assert(new);
         for (int i = 0; i < b->npred; ++i) {
            if (i == j) continue;
            Block *p = blkpred(b, i);
            if (p->idom) { /* new = intersect(p, new) */
               while (p != new) {
                  while (p->id > new->id) p = p->idom;
                  while (p->id < new->id) new = new->idom;
               }
            }
         }
         if (b->idom != new) {
            b->idom = new;
            changed = 1;
         }
      } while ((b = b->lnext) != fn->entry);
   }

   fn->entry->domdepth = 0;
   b = fn->entry->lnext;
   do b->domdepth = b->idom->domdepth + 1; while ((b = b->lnext) != fn->entry);

   fn->prop |= FNBLKID | FNDOM;
}

/* requires dom, rpo, blkid */
bool
dominates(Block *B, Block *b) {
   assert(B->id >= 0);
   for (;; b = b->idom) {
      if (B == b) return 1;
      if (B == b->idom) return 1;
      assert(b->id >= 0);
      if (B->id > b->id) return 0;
   }
}

/* requires dom, rpo, blkid *
 * least common ancestor in dom tree */
Block *
domlca(Block *a, Block *b) {
   if (!a) return b;
   if (!b) return a;
   while (a->domdepth > b->domdepth)
      a = a->idom;
   while (b->domdepth > a->domdepth)
      b = b->idom;
   while (a != b)
      a = a->idom, b = b->idom;
   return a;
}

/* vim:set ts=3 sw=3 expandtab: */
