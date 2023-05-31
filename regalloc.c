#include "ir.h"

extern struct instr instr[];
extern vec_of(struct call) calls;
extern vec_of(struct phi) phis;

static struct bitset taken[1];

static int
nextreg(enum irclass cls)
{
   int r0, rend, i;

   if (kisint(cls)) {
      r0 = mctarg->gpr0;
      rend = mctarg->gpr0 + mctarg->ngpr;
   } else if (kisflt(cls)) {
      r0 = mctarg->fpr0;
      rend = mctarg->fpr0 + mctarg->nfpr;
   } else assert(0);
   for (i = r0; i < rend; ++i) {
      if (!bstest(taken, i)) {
         bsset(taken, i);
         return i;
      }
   }
   assert(!"no more reg");
}

static void
use(struct block *blk, enum op op, int hint, union ref *ref)
{
   struct instr *ins;
   if (ref->t == REXT) {
      if (op == Ocall) {
         struct call *call = &calls.p[ref->idx];
         for (int i = 0; i < call->narg; ++i)
            use(blk, 0, 0, &call->args[i]);
      } else if (op == Ophi) {
         struct phi *phi = &phis.p[ref->idx];
         for (int i = 0; i < phi->n; ++i)
            use(blk, 0, hint, &phi->ref[i]);
      } else assert("ext?");
      return;
   } else if (ref->t != RTMP) return;

   ins = &instr[ref->idx];
   if (in_range(ins->op, Oalloca1, Oalloca16)) return;
   if (!ins->reg) {
      if (op == -1) /* cond branch */
         if (in_range(ins->op, Oequ, Oulte) && ref->idx == blk->ins.p[blk->ins.n-1])
            /* result of comparison instr is only used to conditionally branch,
             * doesn't usually need a reg (handled by isel) */
            return;
      ins->reg = hint ? hint : nextreg(ins->cls);
   }
}

void
regalloc(struct function *fn)
{
   struct block *blk = fn->entry->lprev;
   struct instr *ins;

   /* a dumb linear register allocator that visits instructions backwards
    * starting at the end of the function, when encountering a use of a new
    * temporary, it allocates a register for it. when encountering the definition
    * of a temporary, it frees up its register
    */
   do {
      if (blk->jmp.arg.t) use(blk, blk->jmp.t == Jbcnd ? -1 : 0, 0, &blk->jmp.arg);
      for (int i = blk->phi.n - 1; i >= 0; --i) {
         ins = &instr[blk->phi.p[i]];
         bsclr(taken, ins->reg);
         assert(ins->op == Ophi);
         use(blk, Ophi, ins->reg, &ins->l);
      }
      for (int i = blk->ins.n - 1; i >= 0; --i) {
         ins = &instr[blk->ins.p[i]];
         bsclr(taken, ins->reg);
         if (ins->l.t) use(blk, ins->op, 0, &ins->l);
         if (ins->r.t) use(blk, ins->op, 0, &ins->r);
      }
      blk = blk->lprev;
   } while (blk != fn->entry->lprev);
}

/* vim:set ts=3 sw=3 expandtab: */
