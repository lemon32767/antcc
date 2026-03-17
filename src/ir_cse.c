#include "ir.h"

static inline bool
pure(const struct instr *ins)
{
   return oisarith(ins->op) || (oisload(ins->op) && !ins->keep);
}

static inline bool
insequ(const struct instr *a, const struct instr *b)
{
   if (a->op != b->op) return 0;
   enum op op = a->op;
   switch (opnarg[op]) {
   default: assert(0);
   case 2: if (a->r.bits != b->r.bits) return 0;
   case 1: if (a->l.bits != b->l.bits) return 0;
   }
   return 1;
}

static struct ht {
   uint t;
   ushort memno, cutoff;
   struct block *b;
} *insht;
static uint ninsht;

static inline size_t
hashins(const struct instr *ins)
{
   return hashb(0, ins, sizeof *ins);
}

static bool
doms(struct block *blk, struct block *b)
{
   for (;; b = b->idom) {
      if (blk == b) return 1;
      if (blk == b->idom) return 1;
      if (blk->id > b->id) return 0;
   }
}

static int
uniq(int t, struct block *blk, int cutoff, int memno)
{
   assert((uint)t < MAXINSTR);
   struct instr *ins = &instrtab[t];
   if (!pure(ins)) return t;
   for (size_t h = hashins(&instrtab[t]), i = h;; ++i) {
      struct ht *p = &insht[i &= (ninsht-1)];
      if (!p->b) Put: {
         p->b = blk;
         p->memno = memno;
         p->cutoff = cutoff;
         return p->t = t;
      } else if (insequ(&instrtab[p->t], ins)) {
         if (p->cutoff == cutoff && (!oisload(ins->op) || p->memno == memno) && doms(p->b, blk))
            return p->t;
         goto Put;
      }
   }
}

void
cselim(struct function *fn)
{
   FREQUIRE(FNUSE | FNRPO | FNDOM | FNBLKID);
   extern int ninstr;
   for (ninsht = 32; ninsht <= ninstr; ninsht *= 2) ;
   insht = allocz(fn->passarena, ninsht * sizeof *insht, 0);
   int memno = 0, cutoff = 0;
   struct block *blk = fn->entry;
   do {
      ++memno;
      for (int i = 0; i < blk->ins.n; ++i) {
         int t = blk->ins.p[i], q;
         if ((q = uniq(t, blk, cutoff, memno)) != t) {
            replcuses(mkref(RTMP, t), mkref(RTMP, q));
            delinstr(blk, i--);
         } else if (oisstore(instrtab[t].op)) {
            /* assume everything alias everything */
            ++memno;
         } else if (instrtab[t].op == Ocall) {
            ++cutoff;
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
