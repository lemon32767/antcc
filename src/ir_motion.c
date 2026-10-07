#include "ir.h"

/* sink branch condition ins to block end so isel can use cc flags directly */
static void
sinkbbcmp(Function *fn, Block *b)
{
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
sinkcond(Function *fn)
{
   FREQUIRE(FNUSE);
   for (struct Block *b = fn->entry->lnext; b != fn->entry; b = b->lnext) {
      sinkbbcmp(fn, b);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
