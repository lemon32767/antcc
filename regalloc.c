#include "ir.h"

/* Implements linear scan register allocation */

static regset gpregset, fpregset;
static regset globusage;

#define isfpr(reg) in_range((reg), mctarg->fpr0, mctarg->fpr0 + mctarg->nfpr - 1)
#define isgpr(reg) in_range((reg), mctarg->gpr0, mctarg->gpr0 + mctarg->nfpr - 1)

enum { ADEAD, AREG, ASTACK } ;
struct alloc { ushort t : 2, a : 14; };
#define afree() ((struct alloc) { ADEAD })
#define areg(r) ((struct alloc) { AREG, (r) })
#define astack(s) ((struct alloc) { ASTACK, (s) })

enum { MAXSPILL = 128 };

struct range { ushort from, to; };
struct interval {
   struct interval *next;
   struct alloc alloc;
   schar rhint : 7, fpr : 1;
   uchar nrange;
   union {
      struct range _inl[2];
      struct range *_dyn;
   };
};
struct lifetimes {
   imap_of(struct interval) temps;
   struct fixinterval {
      struct fixinterval *next;
      regset rs;
      struct range range;
   } *fixed;
};

struct rega {
   struct function *fn;
   struct arena *arena;
   regset free; /* free registers */
   struct bitset freestk[BSSIZE(MAXSPILL)]; /* free stack slots */
   int maxstk, stktop;
   struct lifetimes intervals;
};

static vec_of(union ref *) stkslotrefs;

static union ref
addstkslotref(union ref inst)
{
   vpush(&stkslotrefs, &instrtab[inst.i].l);
   return inst;
}

#define mkmove(k, rd, rs) mkinstr(Omove, k, mkref(RREG, rd), mkref(RREG, rs))

#if 1
#define DBG(...) if(ccopt.dbg.r) efmt(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

static int
allocstk(struct rega *ra, int var)
{
   int s = -1;

   for (int i = 0; i < BSSIZE(MAXSPILL); ++i) {
      if (ra->freestk[i].u == 0) continue;
      s = i*64 + lowestsetbit(ra->freestk[i].u);
   }
   if (s != -1) {
      bsclr(ra->freestk, s);
      if (ra->stktop < s) ra->stktop = s+1;
   } else {
      s = ra->stktop++;
   }
   if (ra->maxstk < s+1) ra->maxstk = s+1;
   imap_get(&ra->intervals.temps, var)->alloc = astack(s);
   return s;
}

static void
freestk(struct rega *ra, int slot)
{
   DBG("FREE stk %d\n",slot);
   if (slot < MAXSPILL)
      bsset(ra->freestk, slot);
   else if (slot == ra->stktop - 1)
      --ra->stktop;
}

/* Parallel moves algorithm from QBE: https://c9x.me/git/qbe.git/tree/rega.c?id=e493a7f23352f51acc0a1e12284ab19d7894488a#n201 */

enum pmstat { PMTOMOVE, PMMOVING, PMDONE };
static struct pmove {
   uchar k;
   uchar stat;
   struct alloc dst, src;
} pmove[MAXREGS];
static int npmove;

static void
pmadd(enum irclass k, struct alloc dst, struct alloc src)
{
   if (!memcmp(&dst, &src, sizeof dst)) return;
   assert(npmove < MAXREGS);
   pmove[npmove++] = (struct pmove) { k, PMTOMOVE, dst, src };
}

static void
emitmove(enum irclass k, struct alloc dst, struct alloc src, struct block *blk, int curi)
{
   struct instr mv = {.keep = 1};
   if (dst.t == AREG && src.t == AREG) {
      insertinstr(blk, curi, mkmove(k, dst.a, src.a));
   } else if (dst.t == ASTACK && src.t == AREG) {
      mv = mkinstr(Ostore1+ilog2(cls2siz[k]), 0, mkref(RICON, dst.a*8), mkref(RREG, src.a));
      addstkslotref(insertinstr(blk, curi, mv));
   } else if (dst.t == AREG && src.t == ASTACK) {
      switch (mv.cls = k) {
      default: assert(0);
      case KI4: mv.op = Oloads4; break;
      case KI8: mv.op = Oloadi8; break;
      case KPTR: mv.op = targ_64bit ? Oloadi8 : Oloads4; break;
      case KF4: mv.op = Oloadf4; break;
      case KF8: mv.op = Oloadf8; break;
      }
      mv.l = mkref(RICON, src.a*8);
      mv.reg = dst.a+1;
      addstkslotref(insertinstr(blk, curi, mv));
   }
}

static int
pmrec(int i, struct block *blk, int curi, enum irclass *k)
{
   int j, c;

   if (!memcmp(&pmove[i].dst, &pmove[i].src, sizeof pmove->dst)) {
      pmove[i].stat = PMDONE;
      return -1;
   }

   /* widen when necessary */
   assert(kisint(pmove[i].k) == kisint(*k));
   if (cls2siz[pmove[i].k] > cls2siz[*k])
      *k = pmove[i].k;

   for (j = 0; j < npmove; ++j) {
      if (!memcmp(&pmove[j].dst, &pmove[i].src, sizeof pmove->dst)) {
         break;
      }
   }
   if (j == npmove) goto Done;
   switch (pmove[j].stat) {
   default: assert(0);
   case PMMOVING:
      c = j;
   Swap:
      assert(pmove[i].src.t == AREG && pmove[i].dst.t == AREG);
      insertinstr(blk, curi,
            mkinstr(Oswap, *k, mkref(RREG, pmove[i].dst.a), mkref(RREG, pmove[i].src.a), .keep = 1));
      break;
   case PMTOMOVE:
      pmove[i].stat = PMMOVING;
      c = pmrec(j, blk, curi, k);
      if (c == i) {
         c = -1;
         break;
      } else if (c != -1) {
         goto Swap;
      }
      /* fallthru */
   case PMDONE:
   Done:
      c = -1;
      emitmove(*k, pmove[i].dst, pmove[i].src, blk, curi);
      break;
   }

   pmove[i].stat = PMDONE;
   return c;
}

static void
emitpm(struct block *blk)
{
   int curi = blk->ins.n;
   for (int i = 0; i < npmove; ++i) {
      if (pmove[i].stat == PMTOMOVE) {
         pmrec(i, blk, curi, &(enum irclass) { pmove[i].k });
      }
   }
}

static void
lowerphis(struct rega *ra, struct block *blk, struct block *suc)
{
   int predno;
   struct block *n = NULL;

   if (!blk->s2) n = blk;

   for (predno = 0; predno < suc->npred; ++predno)
      if (blkpred(suc, predno) == blk)
         break;
   assert(predno < suc->npred);

   npmove = 0;
   /* ensure phi args go to the same slot as phi with parallel copies */
   for (int i = 0; i < suc->phi.n; ++i) {
      struct instr *phi = &instrtab[suc->phi.p[i]];
      union ref *arg = &phitab.p[phi->l.i][predno];
      struct alloc from, to;

      if (arg->t == RREG) continue;
      assert(arg->t == RTMP);
      DBG("resolve phi @%d, @%d, %%%d <- %%%d\n", blk->id, suc->id, phi - instrtab, arg->i);
      if (instrtab[arg->i].reg) {
         from = areg(instrtab[arg->i].reg - 1);
         DBG("  it had R%d\n", from.a);
      } else {
         from = imap_get(&ra->intervals.temps, arg->i)->alloc;
         assert(from.t != ADEAD);
         DBG("  found %c%d\n", " RS"[from.t], from.a);
         if (from.t == AREG)
            instrtab[arg->i].reg = from.a+1;
      }
      if (phi->reg) {
         to = areg(phi->reg - 1);
         DBG("  phi had R%d\n", to.a);
      } else {
      irdump(ra->fn);
         to = imap_get(&ra->intervals.temps, arg->i)->alloc;
         assert(to.t != ADEAD);
         DBG("  found phi %c%d\n", " RS"[to.t], to.a);
         if (to.t == AREG)
            phi->reg = to.a+1;
      }
      DBG(" > phi move %c%d -> %c%d\n", " RS"[from.t], from.a, " RS"[to.t], to.a);
      if (!n) n = insertblk(ra->fn, blk, suc);
      pmadd(phi->cls, to, from);
   }
   if (n) emitpm(n);
}

static void
rporec(struct block ***rpo, struct block *b)
{
   if (wasvisited(b)) return;
   markvisited(b);
   if (b->s2) rporec(rpo, b->s2);
   if (b->s1) rporec(rpo, b->s1);
   *--*rpo = b;
}

static void
sortrpo(struct function *fn)
{
   static struct block **rpobuf;
   struct block **rpoend, **rpo;
   int i, ndead;

   xbgrow(&rpobuf, fn->nblk);
   rpo = rpoend = rpobuf + fn->nblk,

   startbbvisit();
   fn->entry->id = 0;
   markvisited(fn->entry);
   if (fn->entry->s1) rporec(&rpo, fn->entry->s1);
   if (fn->entry->s2) rporec(&rpo, fn->entry->s2);
   *--rpo = fn->entry;
   ndead = rpo - rpobuf;
   for (i = 1, ++rpo; rpo < rpoend; ++rpo, ++i) {
      rpo[-1]->lnext = rpo[0];
      rpo[0]->lprev = rpo[-1];
      rpo[0]->id = i;
   }
   fn->entry->lprev = rpo[-1];
   rpo[-1]->lnext = fn->entry;
   for (rpo = rpobuf; ndead > 0; --ndead) {
      (*rpo)->lnext = (*rpo)->lprev = NULL;
      freeblk(fn, *rpo);
   }
}

static void fixlive(struct function *fn);

/* generate copies for phi operands to transform into conventional-SSA */
static void
fixcssa(struct function *fn)
{
   struct block *blk = fn->entry;
   do {
      if (!blk->phi.n) continue;
      for (int p = 0; p < blk->npred; ++p) {
         struct block *n, *pred = blkpred(blk, p);
         if (!pred->s2) {
            /* pred only has 1 successor (blk), so insert move directly in it */
            n = pred;
         } else {
            n = insertblk(fn, pred, blk);
            assert(n->jmp.t == Jb && n->s1 == blk);
         }
         for (int i = 0; i < blk->phi.n; ++i) {
            int phi = blk->phi.p[i];
            union ref *args = phitab.p[instrtab[phi].l.i];
            args[p] = insertinstr(n, n->ins.n, mkinstr(Ocopy, instrtab[phi].cls, args[p]));
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
}

static bool
rangeoverlap(struct range a, struct range b)
{
   return (a.from < b.from && b.from < a.to)
       || (b.from < a.from && a.from < b.to);
}

static bool
rangeadj(struct range a, struct range b)
{
   return a.to+1 >= b.from;
}

static void
pushrange(struct interval *lv, struct range r)
{
   if (lv->nrange < 2) lv->_inl[lv->nrange++] = r;
   else if (lv->nrange > 2) xbpush(&lv->_dyn, &lv->nrange, r);
   else {
      struct range *d = NULL;
      xbgrow(&d, 4);
      memcpy(d, lv->_inl, 2*sizeof *d);
      d[lv->nrange++] = r;
      lv->_dyn = d;
   }
}
#define itrange(lv, i) ((lv)->nrange <= 2 ? (lv)->_inl : (lv)->_dyn)[i]

static void
addrange0(struct interval *it, struct range new)
{
   int dst = it->nrange;

   if (dst == 0) { Push: pushrange(it, new); return; }
   /* start from the end, find place by order */
   for (int i = dst - 1; i >= 0; --i) {
      struct range range = itrange(it, i);

      /* fully contained? */
      if (range.from <= new.from && new.to <= range.to)
         return;
      if (range.from > new.from)
         dst = i;
   }
   if (dst == it->nrange) goto Push;
   if (rangeoverlap(new, itrange(it, dst)) || rangeadj(new, itrange(it, dst))) {
      struct range old = itrange(it, dst),
                   *merge = &itrange(it, dst);
      if (new.from < old.from) merge->from = new.from;
      if (new.to > old.to) merge->to = new.to;
   } else {
      /* insert at dst */
      pushrange(it, new);
      for (int j = it->nrange-1; j > dst; --j)
         itrange(it, j) = itrange(it, j-1);
      itrange(it, dst) = new;
   }
   /* more merges? */
   for (struct range *last = &itrange(it, it->nrange-2);
        it->nrange > 1 && (rangeoverlap(last[0], last[1]) || rangeadj(last[0], last[1]));
        --last)
   {
      if (last[1].from < last[0].from)
         last[0].from = last[1].from;
      if (last[1].to > last[0].to)
         last[0].to = last[1].to;

      if (--it->nrange == 2) {
         struct range *tmp = it->_dyn;
         memcpy(it->_inl, tmp, 2*sizeof*tmp);
         xbfree(it->_dyn);
      }
   }
}

static void
addrange(struct lifetimes *intervals, int t, struct range range, int reghint)
{
   struct interval *it;
   it = imap_get(&intervals->temps, t);
   if (!it) {
      it = imap_set(&intervals->temps, t,
            (struct interval){ .rhint = reghint, .fpr = kisflt(insrescls(instrtab[t])) });
   }
   addrange0(it, range);
}

static bool
intervaldef(struct lifetimes *intervals, int t, struct block *blk, int pos, int reghint)
{
   struct interval *it = imap_get(&intervals->temps, t);
   if (it) {
      assert(it->nrange > 0);

      if (itrange(it, 0).from <= pos) /* shorten */
         itrange(it, 0).from = pos;
      else
         addrange0(it, (struct range){pos, blk->inumstart + blk->ins.n+1});
      if (it->rhint < 0) it->rhint = reghint;
   }
   return it != NULL;
}

static void
priliveset(struct bitset *s, size_t siz)
{
   int k = 0;
   DBG("{");
   for (uint i = 0; bsiter(&i, s, siz); ++i) {
      if (k++) DBG(",");
      DBG("%%%d",i);
   }
   DBG("}\n");
}


static void
usereg(struct rega *ra, int reg, struct block *blk, int pos)
{
   struct fixinterval *fxit = alloc(&ra->arena, sizeof *fxit, 0);
   fxit->next = ra->intervals.fixed;
   fxit->range = (struct range) {blk->inumstart, pos};
   fxit->rs = 1<<reg;
   ra->intervals.fixed = fxit;
}

static void
defreg(struct rega *ra, int reg, int pos) {
   for (struct fixinterval *fxit = ra->intervals.fixed; fxit; fxit = fxit->next) {
      if (fxit->rs == 1<<reg) {
         assert(fxit->range.from <= pos);
         fxit->range.from = pos;
         // DBG(">>>REG %s range @%d: %d-%d\n", mctarg->rnames[reg], fxit->range.from.blk, fxit->range.from.ins, fxit->range.to.ins);
         return;
      }
   }
   assert(0&&"def reg not used");
}

static void
buildintervals(struct rega *ra)
{
   extern int ninstr;
   struct block *blk, *last;
   struct bitset **livein = xcalloc(ra->fn->nblk * sizeof *livein);

   for (int i = 0; i < ra->fn->nblk; ++i)
      livein[i] = xcalloc(BSSIZE(ninstr) * sizeof *livein[i]);

   numberinstrs(ra->fn);
   /* visit blocks in reverse, to build lifetime intervals */
   blk = last = ra->fn->entry->lprev;
   do {
      struct instr *ins = NULL;
      struct bitset *live = livein[blk->id];
      size_t bssize = BSSIZE(ninstr);
      struct block *suc = blk->s1;
      // DBG("--- @%d ---\n",blk->id);

      /* live = union of successor.liveIn for each successor of b */
      if (blk->s1) bsunion(live, livein[blk->s1->id], bssize);
      if (blk->s2) bsunion(live, livein[blk->s2->id], bssize);
      // DBG("live in: "), priliveset(live, bssize);

      /* for each phi function phi of successors of b do
       *    live.add(phi.inputOf(b))
       */
      if (suc) do {
         int predno;
         for (predno = 0; blkpred(suc, predno) != blk; ++predno) ;
         for (int i = 0; i < suc->phi.n; ++i) {
            struct instr *phi = &instrtab[suc->phi.p[i]];
            union ref *arg = &phitab.p[phi->l.i][predno];
            assert(arg->t == RTMP);
            // DBG("from phi set live %%%d\n", arg->i);
            bsset(live, arg->i);
         }
      } while (suc != blk->s2 && (suc = blk->s2));

      /* for each opd in live do
       *    intervals[opd].addRange(b.from, b.to)
       */
      for (uint i = 0; bsiter(&i, live, bssize); ++i) {
         // DBG("itretave %%%d\n",i );
         addrange(&ra->intervals, i, (struct range){blk->inumstart, blk->inumstart + blk->ins.n + 2}, -1);
      }

      /* for each operation op of b in reverse order do */
      union ref queue[8] = { blk->jmp.arg[0], blk->jmp.arg[1] };
      goto Branchopd;
      for (int curi, pos ; curi >= 0; --curi) {
         int out = blk->ins.p[curi], reghint;
         ins = &instrtab[out];
         pos = blk->inumstart + 1 + curi;
         /* for each output operand opd of op do
          *    intervals[opd].setFrom(op.id)
          *    live.remove(opd)
          *
          * for each input operand opd of op do
          *    intervals[opd].addRange(b.from, op.id)
          *    live.add(opd)
          */

         reghint = ins && ins->op == Ocopy && ins->l.t == RREG ? ins->l.i : -1;
         if (!intervaldef(&ra->intervals, out, blk, pos, reghint)) {
            if (insrescls(*ins) && ins->op != Omove && !ins->keep && !(ins->op == Ocopy && ins->l.t == RREG)) {
               /* dead */
               *ins = mkinstr(Onop,0,);
            }
         }
         bsclr(live, out); 
         if (ins->op == Omove) {
            assert(ins->l.t == RREG);
            defreg(ra, ins->l.i, pos);
         } else if (ins->op == Ocall) {
            struct call *call = &calltab.p[ins->r.i];
            regset rclob = rsminus(rsunion(gpregset, fpregset), rsunion(mctarg->rglob, mctarg->rcallee));
            ra->fn->isleaf = 0;

            for (int i = 0; i < 2; ++i) {
               if (call->abiret[i].ty.bits) {
                  int reg = call->abiret[i].reg;
                  rclob = rsclr(rclob, reg);
                  defreg(ra, reg, pos);
               }
            }
            for (int j = 0; j < call->narg; ++j) {
               int reg = call->abiarg[j].reg;
               if (reg >= 0) {
                  rclob = rsclr(rclob, reg);
                  usereg(ra, reg, blk, pos);
               }
            }
            if (rclob) {
               struct fixinterval *fxit = alloc(&ra->arena, sizeof *fxit, 0);
               fxit->next = ra->intervals.fixed;
               fxit->range = (struct range) {pos, pos};
               fxit->rs = rclob;
               ra->intervals.fixed = fxit;
               /*DBG("CLOBBER @%d:%d {", blk->id, curi);
               for (int reg = 0; rsiter(&reg, rclob); ++reg) {
                  DBG("%s,",mctarg->rnames[reg]);
               }
               DBG("}\n");*/
            }
         }

         reghint = (ins && ins->op == Omove && ins->l.t == RREG) ? ins->l.i : -1;
         queue[0] = ins->r, queue[1] = ins->l;
         if (0) {
         Branchopd:
            reghint = -1;
            curi = blk->ins.n;
            pos = blk->inumstart + blk->ins.n + 1;
         }
         for (int nqueue = ins && ins->op == Omove ? 1 : 2; nqueue > 0;) {
            union ref r = queue[--nqueue];

            /* do not allocate a reg for a cmp op used as branch argument, since it's a pseudo op */
            if (curi == blk->ins.n && blk->jmp.t == Jb && r.t == RTMP && instrtab[r.i].keep)
               continue;

            if (r.t == RTMP) {
               addrange(&ra->intervals, r.i, (struct range){blk->inumstart, pos}, reghint);
               bsset(live, r.i);
            } else if (r.t == RREG) {
               usereg(ra, r.i, blk, pos);
            } else if (r.t == RADDR) {
               reghint = -1;
               queue[nqueue++] = addrht[r.i].base;
               queue[nqueue++] = addrht[r.i].index;
            }
         }
      }

      /* for each phi function phi of b do
       *    live.remove(phi.output)
       */
      for (int i = 0; i < blk->phi.n; ++i)
         bsclr(live, blk->phi.p[i]);

      /* if b is loop header then
       *    loopEnd = last block of the loop starting at b
       *    for each opd in live do
       *       &ra->intervals[opd].addRange(b.from, loopEnd.to)
       */
      struct block *loopend = NULL;
      for (int i = 0; i < blk->npred; ++i) {
         struct block *pred = blkpred(blk, i);
         if (pred->id > blk->id)
         loopend = loopend && loopend->id > pred->id ? loopend : pred;
      }
      if (loopend) {
         // DBG("i'm loop header - @%d (to @%d)\n", blk->id, loopend->id);
         for (uint opd = 0; bsiter(&opd, live, bssize); ++opd) {
            // DBG("  i have live %%%d\n", opd);
            addrange(&ra->intervals, opd, (struct range){blk->inumstart, loopend->inumstart + loopend->ins.n+1}, -1);
         /*   struct interval *lv = imap_get(&ra->intervals.temps, opd);
            for (int i = 0; i < lv->n; ++i) {
               struct range r = itrange(lv, i); 
               // DBG("  @%d:%d - @%d:%d\n", r.from.blk, r.from.ins, r.to.blk, r.to.ins);
            } */
         }
      }
      // DBG("live out: "), priliveset(live, bssize);
   } while ((blk = blk->lprev) != last);

   for (int i = 0; i < ra->fn->nblk; ++i) free(livein[i]);
   free(livein);

   int var;
   struct interval *lv;
   imap_each(&ra->intervals.temps, var, lv) {
      DBG("lifetime of %%%d: [ ", var);
      for (int i = 0; i < lv->nrange; ++i) {
         struct range r = itrange(lv, i); 
         DBG("%d-%d, ", r.from, r.to);
      }
      DBG("]\n");
   }
   for (struct fixinterval *fx = ra->intervals.fixed; fx; fx = fx->next) {
      DBG("fixed {");
      for (int r = 0; rsiter(&r, fx->rs); ++r)
         DBG("%s,", mctarg->rnames[r]);
      DBG("}: [%d-%d]\n", fx->range.from, fx->range.to);
   }
}

static int
cmppinterval(void *a, void *b)
{
   struct interval **la = a, **lb = b;
   return (int)itrange(*la, 0).from - (int)itrange(*lb, 0).from;
}

static bool
itcontainspos(const struct interval *it, int pos)
{
   for (int i = 0; i < it->nrange; ++i) {
      struct range r = itrange(it, i);
      if (r.from > pos) return 0;      
      if (pos < r.to) return 1;
   }
   return 0;
}

static void
linearscan(struct rega *ra)
{
   struct lifetimes *intervals = &ra->intervals;
   struct interval *unhandled = NULL, *active = NULL, *inactive = NULL, *handled = NULL;
   /* sort intervals */
   {
      extern void qsort(void *p, size_t n, size_t size, int (*)(void *, void *));
      struct interval **unhandleds = xcalloc(sizeof *unhandleds * intervals->temps.mb.n);
      int i = 0, var;
      struct interval *lv;

      imap_each(&intervals->temps, var, lv) {
         unhandleds[i++] = lv;
      }
      if (!i) {
         free(unhandleds);
         return;
      }
      qsort(unhandleds, i, sizeof *unhandleds, cmppinterval);
      unhandled = NULL;
      while (i-- > 0) {
         unhandleds[i]->next = unhandled;
         unhandled = unhandleds[i];
      }
      free(unhandleds);
   }

   /* LINEAR SCAN */
   for (struct interval *current, *next; (current = unhandled); unhandled = next) {
      int pos = itrange(current, 0).from;
      next = current->next;
      /* Expire old intervals */

      /* check for intervals in active that are handled or inactive */
      for (struct interval **lnk = &active, *it = *lnk, *next; (next = it?it->next:0), it; it = next) {
         /* ends before position? */
         //DBG("< im active %%%d\n", intervals->temps.mb.k[it - intervals->temps.v]);
         if (itrange(it, it->nrange-1).to <= pos) {
            /* move from active to handled */
            *lnk = next;
            it->next = handled;
            handled = it;
            if (it->alloc.t == AREG) {
               ra->free |= 1<<it->alloc.a;
               //DBG("   unblock %s\n", mctarg->rnames[it->reg]);
            }
         } else
         /* it does not cover position? */
         if (!itcontainspos(it, pos)) {
            /* move from active to inactive */
            *lnk = next;
            it->next = inactive;
            inactive = it;
            if (it->alloc.t == AREG) {
               ra->free |= 1<<it->alloc.a;
               //DBG("   unblock %s\n", mctarg->rnames[it->reg]);
            }
         } else lnk = &it->next;
      }

      /* check for intervals in inactive that are handled or active */
      for (struct interval **lnk = &inactive, *it = *lnk, *next; (next = it?it->next:0), it; it = next) {
         //DBG("< im inactive %%%d\n", intervals->temps.mb.k[it - intervals->temps.v]);
         /* ends before position? */
         if (itrange(it, it->nrange-1).to <= pos) {
            /* move from inactive to handled */
            *lnk = next;
            it->next = handled;
            handled = it;
         } else
         /* it covers position? */
         if (itcontainspos(it, pos)) {
            /* move from inactive to active */
            *lnk = next;
            it->next = active;
            active = it;
            if (it->alloc.t == AREG) {
               assert(rstest(ra->free, it->alloc.a));
               ra->free &= ~(1<<it->alloc.a);
               //DBG("reblock %s\n", mctarg->rnames[it->reg]);
            }
         } else lnk = &it->next;
      }

      /* find a register for current */
      {
         int this = intervals->temps.mb.k[current - intervals->temps.v];
         regset free = rsminus(ra->free, current->fpr ? gpregset : fpregset);
         struct instr *ins = &instrtab[this];
         int reg = 0;
         int end = itrange(current, current->nrange-1).to;
         // XXX check regs in inactive intervals dont overlap with current
         for (struct fixinterval *fxit = intervals->fixed; fxit; fxit = fxit->next) {
            if (fxit->range.to <= pos) {
               intervals->fixed = fxit->next;
               continue;
            } else if (fxit->range.from >= end)
               break;
//            if (rangeoverlap(it->range, (struct range) {pos, itrange(current, current->n-1).to})) {
            // if (fxit->range.to >= pos && fxit->range.to < itrange(current, current->n-1).from) {
            if (rangeoverlap(fxit->range, (struct range) {pos, end}) && fxit->range.from != end) {
               free = rsminus(free, fxit->rs);
            }
         }
         if (ins->inplace && opnarg[ins->op] == 2) {
            int xreg;
            if (ins->r.t == RREG) free = rsclr(free, ins->r.i);
            else if (ins->r.t == RTMP && (xreg = instrtab[ins->r.i].reg))
               free = rsclr(free, xreg-1);
         }
         assert(free);
         if (current->rhint >= 0)
            DBG("have hint %s for %%%d\n",
                  mctarg->rnames[current->rhint], intervals->temps.mb.k[current - intervals->temps.v]);
         if (current->rhint >= 0 && rstest(free, current->rhint)) {
            DBG(" (used hint)\n");
            reg = current->rhint;
         } else {
            if (ins->op != Ophi && (opnarg[ins->op] == 1 || (opnarg[ins->op] == 2 && ins->inplace))) {
               DBG(" %%%d try %d,%d\n", this, ins->l.t,ins->l.i);
               if (ins->l.t == RREG && rstest(free, reg = ins->l.i))
                  goto GotReg;
               if (ins->l.t == RTMP)
               if ((reg = instrtab[ins->l.i].reg-1) >= 0)
               if (rstest(free, reg))
                  goto GotReg;
            } else if (ins->op == Ophi) {
               union ref *arg = phitab.p[ins->l.i];
               for (int i = 0; i < xbcap(arg); ++i) {
                  if (arg->t == RREG && rstest(free, reg = arg->i)) goto GotReg;
                  if (arg->t == RTMP)
                  if ((reg = instrtab[arg->i].reg-1) >= 0)
                  if (rstest(free, reg))
                     goto GotReg;
               }
            }
            while (!rstest(free, reg)) ++reg;
         }
      GotReg:
         current->alloc = areg(reg);
         ins->reg = reg + 1;
         DBG("%%%d got %s\n", this, mctarg->rnames[reg]);
         ra->free = rsclr(ra->free, reg);
         globusage = rsset(globusage, reg);

         //if current has a register assigned then add current to active
         current->next = active;
         active = current;
      }
   }
}

void
regalloc(struct function *fn)
{
   static union ref *stkslotrefsbuf[64];
   int id;
   static union { char m[sizeof(struct arena) + (1<<10)]; struct arena *_align; } amem;
   struct rega ra = {fn, .arena = (void *)amem.m};
   struct block *blk, *last;
   memset(ra.arena, 0, sizeof *ra.arena);
   ra.arena->cap = sizeof amem.m;

   /* setup */
   if (!fpregset || !gpregset) {
      for (int i = 0; i < MAXREGS; ++i) {
         if (isfpr(i))
            fpregset = rsset(fpregset, i);
         else if (isgpr(i))
            gpregset = rsset(gpregset, i);
      }
   }
   ra.free = rsminus(rsunion(gpregset, fpregset), mctarg->rglob);
   memset(ra.freestk, 0xFF, sizeof ra.freestk);
   globusage = 0;
   fn->stksiz = alignup(fn->stksiz, 8);
   fn->isleaf = 1;
   vinit(&stkslotrefs, stkslotrefsbuf, arraylength(stkslotrefsbuf));

   /* put into reverse post order */
   sortrpo(fn);

   /* fix liveness ranges */
   fixlive(fn);

   /* transform into CSSA */
   fixcssa(fn);
   
   fillblkids(fn);
   
   if (ccopt.dbg.r) {
      efmt("<< Before linear scan >>\n");
      irdump(fn);
   }

   /* linear scan: build lifetime intervals */
   buildintervals(&ra);

   /* linear scan */
   linearscan(&ra);

   /* resolve */

   /* parallel copies for each phi */
   blk = last = fn->entry->lprev;
   do {
      if (blk->id < 0) continue;
      for (int i = 0; i < blk->npred; ++i) {
         lowerphis(&ra, blkpred(blk, i), blk);
      }
      vfree(&blk->phi);
   } while ((blk = blk->lprev) != last);

   {  int var;
      struct interval *lv;
      imap_each(&ra.intervals.temps, var, lv) {
         // instrtab[var].reg = lv->reg+1;
         if (lv->nrange > 2) xbfree(lv->_dyn);
      }
      imap_free(&ra.intervals.temps);
   }

   /* final cleanup */
   id = 0;
   blk = fn->entry;
   do {
      bool allnops = 1;
      blk->id = id++;
      for (int i = 0; i < blk->ins.n; ++i) {
         int t = blk->ins.p[i];
         struct instr *ins = &instrtab[t];

         for (int i = 0; i < 2; ++i) {
            union ref *r = &i[&ins->l];
            int tr;
            if (r->t == RTMP && (tr = instrtab[r->i].reg)) *r = mkref(RREG, tr-1);
         }

         if (!ins->reg && insrescls(*ins) && ins->op != Omove && !ins->keep) {
         /* dead */
         Nop:
           ins->op = Onop;
         } else if (ins->op == Omove && ins->r.t == RREG && ins->l.i == ins->r.i) {
            goto Nop;
         } else if (ins->op == Ocopy && ins->l.t == RREG && ins->reg-1 == ins->l.i) {
            goto Nop;
         } else if (ins->inplace && ins->l.t == RREG && ins->reg && ins->reg-1 != ins->l.i) {
            /* fixup in-place (two-address) instructions */
            allnops = 0;
            insertinstr(blk, i++, mkmove(ins->cls, ins->reg-1, ins->l.i));
            ins->l.i = ins->reg-1;
         } else if (ins->op != Onop) allnops = 0;
      }

      /* remove no-op blocks */
      if (allnops && !blk->s2 && blk->npred > 0) {
         bool delet = 1;
         for (int i = 0; i < blk->npred; ++i) {
            struct block *p = blkpred(blk, i);
            if (p->s2 && !blk->s1)
               delet = 0;
         }
         for (int i = 0; i < blk->npred; ++i) {
            struct block *p = blkpred(blk, i);
            if (!p->s2 && !blk->s1) {
               /* simplify:
                *
                * @p:
                *   ...
                *   b @blk
                * @blk:
                *   NOP
                *   ret
                */
               assert(p->s1 == blk);
               p->jmp.t = Jret;
               p->s1 = NULL;
            } else if (blk->s1) {
               /* simplify:
                *
                * @p:
                *   ...
                *   b %x, @blk, @other
                * @blk:
                *   NOP
                *   b @next
                */
               struct block *next = blk->s1;
               if (p->s1 == blk) p->s1 = next;
               else if (p->s2 == blk) p->s2 = next;
               else continue;
               for (int i = 0; i < next->npred; ++i) {
                  if (blkpred(next, i) == blk) {
                     blkpred(next, i) = p;
                     goto NextPred;
                  }
               }
               addpred(next, p);
            }
         NextPred:;
         }
         if (delet) {
            freeblk(fn, blk);
            --id;
         }
      }
   } while ((blk = blk->lnext) != fn->entry);

   fn->stksiz += ra.maxstk*8;
   if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
   while (stkslotrefs.n) {
      union ref *adr = stkslotrefs.p[--stkslotrefs.n];
      *adr = mkaddr((struct addr) { .base = mkref(RREG, mctarg->bpr), .disp = -fn->stksiz + adr->i });
   }

   if (ccopt.dbg.r) {
      DBG("<< After regalloc >>\n");
      irdump(fn);
   }
   fn->regusage = globusage;
   freearena(ra.arena);
}


static int livelastblk;
struct pendingphi { ushort var, phi; };
static vec_of(struct pendingphi) *pendingphis;
static int npendingphi;
static ushort **curdefs;
static union ref readvar(struct bitset *defined, enum irclass cls, int var, struct block *blk);

/* The algorithm used here to introduce phis for temporaries whose definitions
 * appear later than some of its uses is very similar to that in mem2reg() */

static void
fillphi(struct bitset *defined, union ref phi, enum irclass cls, int var, struct block *blk)
{
   union ref *args = phitab.p[instrtab[phi.i].l.i];
   assert(blk->npred > 0);
   for (int i = 0; i < blk->npred; ++i)
      args[i] = readvar(defined, cls, var, blk);
}

static union ref
readvar(struct bitset *defined, enum irclass cls, int var, struct block *blk)
{
   union ref val;

   if (bstest(defined, var)) return mkref(RTMP, var);

   /* memoed definition */
   if (xbcap(curdefs) > blk->id && xbcap(curdefs[blk->id]) > var && curdefs[blk->id][var])
      return mkref(RTMP, curdefs[blk->id][var]);

   xbgrowz(&curdefs, blk->id + 1);
   if (blk->id > livelastblk) {
      ++npendingphi;
      val = insertphi(blk, cls);
      xbgrowz(&pendingphis, blk->id + 1);
      vpush(&pendingphis[blk->id], ((struct pendingphi) { var, val.i }));
   } else if (blk->npred == 1) {
      val = readvar(defined, cls, var, blkpred(blk, 0));
   } else {
      val = insertphi(blk, cls);
      fillphi(defined, val, cls, var, blk);
   }
   xbgrowz(&curdefs[blk->id], var + 1);
   assert(val.i > 0);
   curdefs[blk->id][var] = val.i;
   return val;
}

static void
liveuse(struct bitset *defined, struct instr *ins, union ref *r, struct block *blk)
{
   int var;
   if (r->t == RADDR) {
      liveuse(defined, ins, &addrht[r->i].base, blk);
      liveuse(defined, ins, &addrht[r->i].index, blk);
      return;
   } else if (r->t != RTMP) return;
   var = r->i;
   if (bstest(defined, var)) return;

   *r = readvar(defined, insrescls(instrtab[r->i]), var, blk);
}

/* regalloc() assumes every use of a temporary is visited before its definition
 * so this function fixes cases where that would not apply by introducing phi functions */
static void
fixlive(struct function *fn)
{
   extern int ninstr;
   struct block *blk = fn->entry;
   struct bitset definedbuf[4] = {0};
   struct bitset *defined = definedbuf;

   if (ninstr >= sizeof(definedbuf)*8)
      defined = xcalloc(sizeof *defined * BSSIZE(ninstr));
   npendingphi = 0;

   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         int var = blk->phi.p[i];
         bsset(defined, var);
      }

      for (int i = 0; i < blk->ins.n; ++i) {
         int var = blk->ins.p[i];
         struct instr *ins = &instrtab[var];
         if (ins->l.t) liveuse(defined, ins, &ins->l, blk);
         if (ins->r.t) liveuse(defined, ins, &ins->r, blk);
         bsset(defined, var);
      }
   } while ((blk = blk->lnext) != fn->entry);

   do {
      vec_of(struct pendingphi) *pphi;

      if (!npendingphi) break;
      if (xbcap(pendingphis) <= blk->id) break;

      pphi = (void *)&pendingphis[blk->id];
      npendingphi -= pphi->n;
      for (int i = 0; i < pphi->n; ++i) {
         fillphi(defined, mkref(RTMP, pphi->p[i].phi), instrtab[pphi->p[i].phi].cls, pphi->p[i].var, blk);
      }
      vfree(pphi);
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.l) {
      DBG("<< After liveness fixup >>\n");
      irdump(fn);
   }
   if (defined != definedbuf) free(defined);
}

/* vim:set ts=3 sw=3 expandtab: */
