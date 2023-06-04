#include "ir.h"

static struct bitset taken[1];

static void
def(struct instr *ins)
{
   if (ins->reg)
      bsclr(taken, ins->reg - 1);
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
   if (ref->t == RMORE) {
      if (op == Ocall || op == Ointrin) {
         struct call *call = &calltab.p[ref->idx];
         for (int i = 0; i < call->narg; ++i)
            use(blk, 0, op == Ocall ? call->abiargregs[i] : -1, &call->args[i]);
      } else if (op == Ophi) {
         struct phi *phi = &phitab.p[ref->idx];
         for (int i = 0; i < phi->n; ++i)
            use(blk, 0, hint, &phi->ref[i]);
      } else assert("ext?");
      return;
   } else if (ref->t != RTMP) return;

   ins = &instrtab[ref->idx];
   if (oisalloca(ins->op)) return;
   if (!ins->cls) return;
   if (!ins->reg) {
      if (op == -1) /* cond branch */
         if (oiscmp(ins->op) && ref->idx == blk->ins.p[blk->ins.n-1])
            /* result of comparison instr is only used to conditionally branch,
             * doesn't usually need a reg (handled by isel) */
            return;
      /* TODO implement actual constraints and stuff */
      if (ins->op == Ocall) {
         struct call *call = &calltab.p[ins->r.idx];
         hint = call->abiret[0].reg;
      } else if (ins->op == Ocall2r) {
         struct instr *ins2 = &instrtab[ins->l.idx];
         struct call *call = &calltab.p[ins2->r.idx];
         assert(ins->l.t == RTMP && ins2->op == Ocall);
         hint = call->abiret[0].reg;
      }
      if (hint != -1 && !bstest(taken, hint)) {
         bsset(taken, hint);
         ins->reg = hint + 1;
      } else {
         ins->reg = nextreg(ins->cls) + 1;
      }
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
      for (int i = blk->phi.n - 1; i >= 0; --i) {
         ins = &instrtab[blk->phi.p[i]];
         def(ins);
         assert(ins->op == Ophi);
         use(blk, Ophi, ins->reg - 1, &ins->l);
      }
      for (int i = blk->ins.n - 1; i >= 0; --i) {
         int hint0 = -1, hint1 = -1;
         ins = &instrtab[blk->ins.p[i]];
         def(ins);
         if (ins->op == Ocopy) hint0 = ins->reg - 1;
         if (ins->l.t) use(blk, ins->op, hint0, &ins->l);
         if (ins->r.t) use(blk, ins->op, hint1, &ins->r);
      }
   } while ((blk = blk->lprev) != last);
}

/* vim:set ts=3 sw=3 expandtab: */
