#include "ir.h"

static struct bitset globusage[1];
static struct bitset taken[1];

static void
def(struct instr *ins)
{
   if (ins->reg)
      bsclr(taken, ins->reg - 1);
}

static void
take(int r) {
   bsset(taken, r);
   bsset(globusage, r);
}

static int
nextreg(enum irclass cls)
{
   int r0, rend, i;

   assert(cls);
   if (kisint(cls)) {
      r0 = mctarg->gpr0;
      rend = mctarg->gpr0 + mctarg->ngpr;
   } else if (kisflt(cls)) {
      r0 = mctarg->fpr0;
      rend = mctarg->fpr0 + mctarg->nfpr;
   } else assert(0);
   for (i = r0; i < rend; ++i) {
      if (!bstest(taken, i)) {
         take(i);
         return i;
      }
   }
   assert(!"no more reg");
}

static void
use(struct block *blk, enum op op, int hint, union ref *ref)
{
   struct instr *ins;
   if (ref->t == RMORE) {
      if (op == Ophi) {
         struct phi *phi = &phitab.p[ref->i];
         for (int i = 0; i < phi->n; ++i)
            use(blk, 0, hint, &phi->ref[i]);
      } else {
         struct addr *addr = &addrtab.p[ref->i];
         if (addr->base.t) use(blk, 0, hint, &addr->base);
         if (addr->index.t) use(blk, 0, hint, &addr->index);
      }
      return;
   } else if (ref->t != RTMP) return;

   ins = &instrtab[ref->i];
   if (oisalloca(ins->op)) return;
   if (!ins->cls) return;
   if (!ins->reg) {
      if (op == -1) /* cond branch */
         if (oiscmp(ins->op) && ref->i == blk->ins.p[blk->ins.n-1])
            /* result of comparison instr is only used to conditionally branch,
             * doesn't usually need a reg (handled by isel) */
            return;
      assert(ins->op != Ocall);
      if (hint != -1 && !bstest(taken, hint)) {
         take(hint);
         ins->reg = hint + 1;
      } else {
         ins->reg = nextreg(ins->cls) + 1;
      }
      *ref = mkref(RREG, ins->reg-1);
   }
}

void
regalloc(struct function *fn)
{
   struct instr *ins;
   struct block *last = fn->entry->lprev, *blk;

   /* a dumb linear register allocator that visits instructions physically backwards
    * starting at the end of the function, when encountering a use of a new
    * temporary, it allocates a register for it. when encountering the definition
    * of a temporary, it frees up its register
    */
   blk = last;
   do {
      for (int i = 0; i < 2; ++i) {
         if (!blk->jmp.arg[i].t) break;
         use(blk, (blk->jmp.t == Jb) - 1,
                  blk->jmp.t == Jret ? fn->abiret[i].reg : -1,
                  &blk->jmp.arg[i]);
      }
      for (struct block *s = blk->s1; s; s = blk->s2) {
         /* introduce necessary moves for successors' phis */
         for (int i = 0; i < s->phi.n; ++i) {
            ins = &instrtab[s->phi.p[i]];
            struct phi *phi = &phitab.p[ins->l.i];
            for (int i = 0; i < phi->n; ++i) {
               if (phi->blk[i] == blk) {
                  union ref src = phi->ref[i];
                  int reg = ins->reg - 1;
                  assert(reg >= 0);
                  if (src.t != RTMP || instrtab[src.i].reg-1 != reg) /* not in the right register already */
                     insertinstr(blk, blk->ins.n,
                                 (struct instr){Ocopy, ins->cls, reg+1, .l = src});
                  break;
               }
            }
         }
         if (s == blk->s2) break;
      }
      for (int i = blk->ins.n - 1; i >= 0; --i) {
         int hint0 = -1, hint1 = -1;
         ins = &instrtab[blk->ins.p[i]];
         if (!ins->reg && ins->skip) { /* unused */
            *ins = mkinstr(Onop, 0,);
            continue;
         }
         def(ins);
         if (ins->op == Omove) {
            use(blk, ins->op, ins->l.i, &ins->r);
         } else if (ins->op != Ocall) {
            if (ins->op == Ocopy) hint0 = ins->reg - 1;
            if (ins->l.t) use(blk, ins->op, hint0, &ins->l);
            if (ins->r.t) use(blk, ins->op, hint1, &ins->r);
         } else {
            struct call *call = &calltab.p[ins->r.i];
            use(blk, ins->op, hint0, &ins->l);
         }
      }
      for (int i = blk->phi.n - 1; i >= 0; --i) {
         ins = &instrtab[blk->phi.p[i]];
         assert(ins->op == Ophi);
         def(ins);
         use(blk, Ophi, ins->reg - 1, &ins->l);
      }
   } while ((blk = blk->lprev) != last);

   do vfree(&blk->phi); while ((blk = blk->lprev) != last);

   if (ccopt.dbg.r) {
      efmt("after regalloc:\n");
      irdump(fn, fn->name);
   }
   bscopy(fn->regusage, globusage, 1);
}

/* vim:set ts=3 sw=3 expandtab: */
