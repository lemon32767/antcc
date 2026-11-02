#include "ir.h"
#include "u_chkarith.h"
#include "u_hash.h"

static inline bool
pure(const Instr *ins) {
   return oisarith(ins->op) || (oisload(ins->op) && !(ins->f&INSVOLATILE));
}

static inline bool
insequ(const Instr *a, const Instr *b) {
   if (a->op != b->op) return 0;
   if (a->f != b->f) return 0;
   if (a->cls != b->cls) return 0;
   enum op op = a->op;
   switch (opnoper[op]) {
   default: assert(0);
   case 2: if (a->r.bits != b->r.bits)
              return opcommut[op] && (a->l.bits == b->r.bits && a->r.bits == b->l.bits);
           /* fallthru */
   case 1: if (a->l.bits != b->l.bits) return 0;
   }
   return 1;
}

static struct Ht {
   uint t;
   ushort memno, cutoff;
   Block *b;
} *insht;
static uint ninsht;

static inline size_t
hashins(const Instr *ins) {
   size_t h = HASH_INIT;
   h = HASH_STEP(h, ins->op);
   h = HASH_STEP(h, ins->f);
   h = HASH_STEP(h, ins->cls);
   if (opcommut[ins->op] && ins->l.bits > ins->r.bits) {
      h = hashb(h, &ins->r, sizeof ins->r);
      h = hashb(h, &ins->l, sizeof ins->l);
   } else {
      h = hashb(h, ins->oper, sizeof *ins->oper * opnoper[ins->op]);
   }
   return h;
}

static int
uniq(int t, Block *blk, int cutoff, int memno) {
   assert((uint)t < MAXINSTR);
   Instr *ins = &instrtab[t];
   if (!pure(ins)) return t;
   for (size_t h = hashins(&instrtab[t]), i = h;; ++i) {
      struct Ht *p = &insht[i &= (ninsht-1)];
      if (!p->b) Put: {
         p->b = blk;
         p->memno = memno;
         p->cutoff = cutoff;
         return p->t = t;
      } else if (insequ(&instrtab[p->t], ins)) {
         if (p->cutoff == cutoff
          && (!oisload(ins->op) || p->memno == memno)
          && dominates(p->b, blk))
            return p->t;
         goto Put;
      }
   }
}

int
cselim(Function *fn) {
   FREQUIRE(FNUSE | FNRPO | FNDOM | FNBLKID);
   bool any = 0;
   extern int ninstrtab;
   for (ninsht = 32; ninsht <= ninstrtab; ninsht *= 2) ;
   insht = anewnz(fn->passarena, struct Ht, ninsht);
   int memno = 0, cutoff = 0;
   Block *blk = fn->entry;
   do {
      ++memno;
      for (int i = 0; i < blk->ins.n; ++i) {
         int t = blk->ins.p[i], q;
         if ((q = uniq(t, blk, cutoff, memno)) != t) {
            replcuses(mkref(RTMP, t), mkref(RTMP, q), NULL, REPLC_ALL);
            delinstr(blk, i--);
            any = 1;
         } else if (oisstore(instrtab[t].op)) {
            /* assume everything alias everything */
            ++memno;
         } else if (instrtab[t].op == Ocall) {
            ++cutoff;
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
   return any;
}

/* vim:set ts=3 sw=3 expandtab: */
