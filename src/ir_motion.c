#include "ir.h"
#include "u_bits.h"

/* sink branch condition ins to block end so isel can use cc flags directly */
static void
sinkbbcmp(Function *fn, Block *b) {
   if (b->jmp.t == Jb && b->jmp.arg[0].t == RTMP && b->ins.n > 0) {
      int jc = b->jmp.arg[0].i;
      ushort *last = &b->ins.p[b->ins.n - 1];
      IRUse *use;
      if (*last != jc && oisarith(instrtab[jc].op) && (use = instruse[jc]) && use->u == USERJUMP && !use->next) {
         int j, mv;
         for (j = 0, mv = 0; j < b->ins.n; ++j) {
            if (b->ins.p[j] == jc) mv = 1;
            else if (mv) b->ins.p[j-1] = b->ins.p[j];
         }
         if (mv) *last = jc;
      }
   }
}

void
sinkcond(Function *fn) {
   FREQUIRE(FNUSE);
   for (struct Block *b = fn->entry->lnext; b != fn->entry; b = b->lnext) {
      sinkbbcmp(fn, b);
   }
}

/**
 * Global Code Motion: https://bernsteinbear.com/assets/img/click-gvn.pdf
 */

typedef struct GCM {
   BitSet *tvisit;
   Block **tblk;
} GCM;

/* pinned instruction (side effects/may trap) */
static bool
ispinned(Instr *ins) {
   switch (ins->op) {
   case Ocopy:
      return 0;
   case Odiv: case Orem: case Oudiv: case Ourem:
      return kisint(ins->cls);
   }
   return !oisarith(ins->op);
}

/* earliest block for node */
static Block *
schedearly(Function *fn, GCM *gcm, Ref r) {
   if (r.t != RTMP) return NULL;
   Instr *ins = &instrtab[r.i];
   if (oisalloca(ins->op)) /* allocas dont have strict dominance semantics */
      return fn->entry;
   Block *b = gcm->tblk[r.i];
   assert(b);
   if (bstest(gcm->tvisit, r.i)) /* instruction already scheduled? */
      return b;
   bsset(gcm->tvisit, r.i); /* start visit */
   for (Ref *i = ins->oper, *end = i + opnoper[ins->op]; i != end; ++i) {
      Block *b1 = schedearly(fn, gcm, *i); /* sched all inputs early */
      if (b1 && (!b || b1->domdepth > b->domdepth))
         b = b1; /* choose deepest dom input */
   }
   return gcm->tblk[r.i] = b;
}

/* pick lowest position in shallowest loop nest */
static Block *
bestblk(Block *early, Block *late) {
   if (!late) return early;
   assert(dominates(early, late));
   Block *best = late;
   while (late != early) {
      if (late->loopdepth < best->loopdepth)
         best = late;
      late = late->idom;
   }
   return best;
}

/* find latest legal block for node */
static Block *
schedlate(GCM *gcm, Ref r) {
   if (r.t != RTMP) return NULL;
   if (bstest(gcm->tvisit, r.i)) return gcm->tblk[r.i];
   Block *early = gcm->tblk[r.i];
   bsset(gcm->tvisit, r.i);
   if (ispinned(&instrtab[r.i]))
      return early; /* pinned, stay put */
   Block *lca = NULL;
   for (IRUse *y = instruse[r.i]; y; y = y->next) {
      Block *use;
      if (y->u == USERJUMP) {
         use = y->blk;
      } else if (!instrtab[y->u].op) { /* dead */
         continue;
      } else if (instrtab[y->u].op == Ophi) {
         /* phis have reverse depence edge from i to y */
         Ref *args = phiargs(y->u);
         use = NULL;
         for (int j = 0; j < y->blk->npred; ++j) {
            if (args[j].bits == r.bits)
               use = domlca(use, blkpred(y->blk, j));
         }
         assert(use);
      } else {
         use = schedlate(gcm, mkref(RTMP, y->u));
      }
      lca = domlca(lca, use);
   }
   return gcm->tblk[r.i] = bestblk(early, lca);
}

static void
schedlatei(GCM *gcm, int i) {
   if (ispinned(&instrtab[i])) {
      bsset(gcm->tvisit, i);
      for (IRUse *y = instruse[i]; y; y = y->next) {
         if (y->u != USERJUMP)
            schedlate(gcm, mkref(RTMP, y->u));
      }
   }
}

/* move each ins to scheduled block, can put defs after uses,
 * must be fixed by schedblk */
static void
gcmmove(Function *fn, GCM *gcm) {
   Block *end = fn->entry->lprev, *b = end;
   do {
      for (int i = b->ins.n - 1; i >= 0; --i) {
         Block *to = gcm->tblk[b->ins.p[i]];
         assert(to);
         if (to != b) moveinstr(fn, b, i, to, 0);
      }
   } while ((b = b->lprev) != end);
}

typedef vec_of(ushort) vec_of_ushort;
static void
dfsemit(GCM *gcm, Block *b, vec_of_ushort *out, int t) {
   /* reuse tblk[t] as visit marker (null) */
   if (!gcm->tblk[t]) return;
   gcm->tblk[t] = NULL;
   Instr *ins = &instrtab[t];
   if (ins->op == Ophi) return;
   for (Ref *o = ins->oper, *end = o + opnoper[ins->op]; o != end; ++o) {
      if (o->t == RTMP && gcm->tblk[o->i] == b)
         dfsemit(gcm, b, out, o->i);
   }
   vpush(out, t);
}
/* DFS traversal to schedule instructions that were shuffled by gcmmove */
static void
schedblk(GCM *gcm, Block *b) {
   vec_of_ushort out = {0};
   vinit(&out, NULL, b->ins.n);
   for (int i = 0; i < b->ins.n; ++i)
      dfsemit(gcm, b, &out, b->ins.p[i]);
   vfree(&b->ins);
   memcpy(&b->ins, &out, sizeof out);
}

void
gcm(Function *fn) {
   extern int ninstrtab;
   FREQUIRE(FNDOM | FNRPO | FNBLKID | FNLOOP | FNUSE);
   GCM gcm = {
      anewbitset(fn->passarena, ninstrtab),
      allocz(fn->passarena, ninstrtab * sizeof(Block *), 0),
   };

   Block *b = fn->entry;
   /* schedule early */
   do {
      for (int i = 0; i < b->phi.n; ++i) {
         /* phis are pinned */
         gcm.tblk[b->phi.p[i]] = b;
         bsset(gcm.tvisit, b->phi.p[i]);
      }
      for (int i = 0, t; i < b->ins.n; ++i) {
         Instr *ins = &instrtab[t = b->ins.p[i]];
         gcm.tblk[t] = b;
         if (ispinned(ins)) { /* pinned ins remain in their original block */
            bsset(gcm.tvisit, t);
            for (Ref *r = ins->oper, *end = r + opnoper[ins->op]; r != end; ++r) {
               /* schedule their inputs */
               schedearly(fn, &gcm, *r);
            }
         }
      }
      for (int i = 0; i < countof(b->jmp.arg); ++i) {
         /* branch args are pinned */
         if (!b->jmp.arg[i].t) break;
         schedearly(fn, &gcm, b->jmp.arg[i]);
      }
   } while ((b = b->lnext) != fn->entry);

   bszero(gcm.tvisit, BSSIZE(ninstrtab));

   /* schedule late */
   do {
      for (int i = 0; i < b->phi.n; ++i)
         schedlatei(&gcm, b->phi.p[i]);
      for (int i = 0; i < b->ins.n; ++i)
         schedlatei(&gcm,  b->ins.p[i]);
      for (int i = 0; i < countof(b->jmp.arg); ++i) {
         if (!b->jmp.arg[i].t) break;
         schedlate(&gcm, b->jmp.arg[i]);
      }
   } while ((b = b->lnext) != fn->entry);

   gcmmove(fn, &gcm);
   do schedblk(&gcm, b); while ((b = b->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
