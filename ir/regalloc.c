#include "ir.h"

/** Implements linear scan register allocation **/

#if 1
#define DBG(...) if(ccopt.dbg.r) efmt(__VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

/* The algorithm used here to introduce phis for temporaries whose definitions
 * appear later than some of its uses is very similar to that in mem2reg() */

static int livelastblk;
struct pendingphi { ushort var, phi; };
static vec_of(struct pendingphi) *pendingphis;
static int npendingphi;
static ushort **curdefs;
static union ref readvar(struct bitset *defined, enum irclass cls, int var, struct block *blk);

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
   assert(cls && "?");

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

   if (BSSIZE(ninstr) >= arraylength(definedbuf))
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

static regset gpregset, fpregset;

#define isfpr(reg) in_range((reg), mctarg->fpr0, mctarg->fpr0 + mctarg->nfpr - 1)
#define isgpr(reg) in_range((reg), mctarg->gpr0, mctarg->gpr0 + mctarg->nfpr - 1)

/* an allocated physical register or stack slot */
enum { ADEAD, AREG, ASTACK };
struct alloc { ushort t : 2, a : 14; };
#define afree() ((struct alloc) { ADEAD })
#define areg(r) ((struct alloc) { AREG, (r) })
#define astack(s) ((struct alloc) { ASTACK, (s) })

enum { MAXSPILL = 512 };

/* half-closed instr range [from, to) */
struct range { ushort from, to; };

/* a temporary's lifetime interval */
struct interval {
   struct interval *next; /* for linked list of active,inactive,handled sets in linear scan */
   struct alloc alloc;
   schar rhint : 7, /* register hint */
         fpr : 1; /* needs float register? */

   /* sorted ranges array */
   uchar nrange;
   union {
      struct range _inl[2];
      struct range *_dyn;
   };
};

struct intervals {
   int count; /* number of actual intervals */
   int ntemps; /* size of temps */
   struct interval *temps; /* map of tmp -> interval */
   struct fixinterval {
      struct fixinterval *next;
      regset rs;
      struct range range;
   } *fixed; /* linked list of fixed intervals, always sorted */
};

struct rega {
   struct function *fn;
   struct arena **arena;
   regset free; /* free registers */
   struct bitset freestk[BSSIZE(MAXSPILL)]; /* free stack slots */
   int maxstk, /* highest stack slot used */
       stktop;
   struct intervals intervals;
};

/* materialization of stack slot references is deferred until the end because
 * the offset from base pointer depends on how many slots we end up allocating */
static vec_of(union ref *) stkslotrefs;

static void
addstkslotref(int instr, uint off)
{
   union ref *ref = &instrtab[instr].l;
   *ref = mkref(RICON, off);
   vpush(&stkslotrefs, ref);
}

static struct alloc
allocstk(struct rega *ra)
{
   int s = -1;

   for (int i = 0; i < BSSIZE(MAXSPILL); ++i) {
      if (ra->freestk[i].u != 0) {
         s = i*64 + lowestsetbit(ra->freestk[i].u);
         break;
      }
   }
   if (s != -1) {
      bsclr(ra->freestk, s);
      if (ra->stktop < s) ra->stktop = s+1;
   } else {
      s = ra->stktop++;
   }
   if (ra->maxstk < s+1) ra->maxstk = s+1;
   //imap_get(&ra->intervals.temps, var)->alloc = astack(s);
   return astack(s);
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

#define mkmove(k, rd, rs) mkinstr(Omove, k, mkref(RREG, rd), mkref(RREG, rs))

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
      mv = mkinstr(Ostore1+ilog2(cls2siz[k]), 0, .r = mkref(RREG, src.a));
      addstkslotref(insertinstr(blk, curi, mv).i, dst.a*8);
   } else if (dst.t == AREG && src.t == ASTACK) {
      switch (mv.cls = k) {
      default: assert(0);
      case KI4: mv.op = Oloads4; break;
      case KI8: mv.op = Oloadi8; break;
      case KPTR: mv.op = targ_64bit ? Oloadi8 : Oloads4; break;
      case KF4: mv.op = Oloadf4; break;
      case KF8: mv.op = Oloadf8; break;
      }
      mv.reg = dst.a+1;
      addstkslotref(insertinstr(blk, curi, mv).i, src.a*8);
   } else assert(0);
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

/* remove phis by inserting parallel moves */
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
         from = ra->intervals.temps[arg->i].alloc;
         assert(from.t != ADEAD);
         DBG("  found %c%d\n", " RS"[from.t], from.a);
         if (from.t == AREG)
            instrtab[arg->i].reg = from.a+1;
      }
      if (phi->reg) {
         to = areg(phi->reg - 1);
         DBG("  phi had R%d\n", to.a);
      } else {
         to = ra->intervals.temps[phi - instrtab].alloc;
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

static inline bool
rangeoverlap(struct range a, struct range b)
{
   return a.from < b.to && b.from < a.to;
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

static bool
intervaloverlap(struct interval *a, struct interval *b)
{
   for (int i = 0, j = 0; i < a->nrange && j < b->nrange; ) {
      struct range r1 = itrange(a, i), r2 = itrange(b, j);
      if (rangeoverlap(r1, r2)) return 1;
      if (r1.to <= r2.from) ++i;
      else ++j;
   }
   return 0;
}

static bool
intervaldef(struct intervals *intervals, int t, struct block *blk, int pos, int reghint)
{
   struct interval *it = &intervals->temps[t];
   if (it->nrange) {
      assert(itrange(it, 0).from <= pos);
      itrange(it, 0).from = pos;
      return 1;
   }
   return 0;
}

static void
addrange(struct intervals *intervals, int t, struct range new, int reghint)
{
   struct interval *it = &intervals->temps[t];
   struct range *fst;
   int n;

   if (!it->nrange) {
      ++intervals->count;
      it->rhint = reghint;
      it->fpr = kisflt(insrescls(instrtab[t]));
      pushrange(it, new);
      return;
   }

   fst = &itrange(it, 0);
   /* fully covered by first range? */
   if (fst->from <= new.from && fst->to >= new.to) return;
   /* overlaps with first range ? */
   if (fst->from <= new.to && new.to < fst->to) {
      fst->from = new.from;
   } else {
      /* put new range at the start */
      pushrange(it, new);
      memmove(&itrange(it, 1), &itrange(it, 0), sizeof(struct range) * (it->nrange - 1));
      itrange(it, 0) = new;
   }

   /* new range might cover existing ranges (loop header lives),
    * check and succesively merge */
   fst = &itrange(it, 0);
   n = 0;
   for (int i = 1; i < it->nrange; ++i) {
      struct range other = itrange(it, i);
      if (fst->to >= other.from) {
         fst->to = fst->to > other.to ? fst->to : other.to;
         ++n;
      } else break;
   }

   if (n > 0) {
      for (int i = 1; i + n < it->nrange; ++i)
         itrange(it, i) = itrange(it, i+n);
      if (it->nrange > 2 && it->nrange - n <= 2) {
         struct range *dyn = it->_dyn;
         memcpy(it->_inl, dyn, (it->nrange - n) * sizeof *dyn);
         xbfree(dyn);
      }
      it->nrange -= n;
   }
}

static void
usereg(struct rega *ra, int reg, struct block *blk, int pos)
{
   struct fixinterval *fxit;
   if (rstest(mctarg->rglob, reg)) return; /* regalloc never allocates globally live regs, so don't need intervals for those */
   fxit = alloc(ra->arena, sizeof *fxit, 0);
   fxit->next = ra->intervals.fixed;
   fxit->range = (struct range) {blk->inumstart, pos};
   fxit->rs = 1<<reg;
   ra->intervals.fixed = fxit;
}

static void
defreg(struct rega *ra, int reg, int pos) {
   if (rstest(mctarg->rglob, reg)) return;
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

/* lifetime interval construction: https://c9x.me/compile/bib/Wimmer10a.pdf */
static void
buildintervals(struct rega *ra)
{
   extern int ninstr;
   struct block *blk, *last;
   struct bitset **livein = alloc(ra->arena, ra->fn->nblk * sizeof *livein, 0);
   size_t bssize = BSSIZE(ninstr);
   for (int i = 0; i < ra->fn->nblk; ++i)
      livein[i] = allocz(ra->arena, bssize * sizeof *livein[i], 0);
   ra->intervals.temps = allocz(ra->arena, ninstr * sizeof *ra->intervals.temps, 0);
   ra->intervals.ntemps = ninstr;

   numberinstrs(ra->fn);
   /* visit blocks in reverse, to build lifetime intervals */
   blk = last = ra->fn->entry->lprev;
   do {
      struct instr *ins = NULL;
      struct bitset *live = livein[blk->id];
      struct block *suc = blk->s1;
      // DBG("--- @%d ---\n",blk->id);

      /* live = union of successor.liveIn for each successor of b */
      if (blk->s1) bsunion(live, livein[blk->s1->id], bssize);
      if (blk->s2) bsunion(live, livein[blk->s2->id], bssize);

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
          */
         reghint = ins && ins->op == Ocopy && ins->l.t == RREG ? ins->l.i : -1;
         if (!intervaldef(&ra->intervals, out, blk, pos, reghint)) {
            if (insrescls(*ins) && ins->op != Omove && !ins->keep && !(ins->op == Ocopy && ins->l.t == RREG)) {
               /* dead */
               *ins = mkinstr(Onop,0,);
            }
         }
         bsclr(live, out);

         /* gather fixed intervals */
         if (ins->op == Omove) {
            assert(ins->l.t == RREG);
            defreg(ra, ins->l.i, pos);
         } else if (ins->op == Ocall) {
            struct call *call = &calltab.p[ins->r.i];
            regset rclob = (gpregset | fpregset) &~ (mctarg->rglob | mctarg->rcallee);
            ra->fn->isleaf = 0;

            for (int i = 0; i < 2; ++i) {
               if (call->abiret[i].ty.bits) {
                  int reg = call->abiret[i].reg;
                  rsclr(&rclob, reg);
                  defreg(ra, reg, pos);
               }
            }
            if (rclob) {
               struct fixinterval *fxit = alloc(ra->arena, sizeof *fxit, 0);
               fxit->next = ra->intervals.fixed;
               fxit->range = (struct range) {pos, pos};
               fxit->rs = rclob;
               ra->intervals.fixed = fxit;
            }
            for (int j = call->narg - 1; j >= 0; --j) {
               int reg = call->abiarg[j].reg;
               if (reg >= 0) {
                  usereg(ra, reg, blk, pos);
               }
            }
         }


         /* for each input operand opd of op do
          *    intervals[opd].addRange(b.from, op.id)
          *    live.add(opd)
          */
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
   } while ((blk = blk->lprev) != last);

   for (int var = 0; var < ninstr; ++var) {
      struct interval *it = &ra->intervals.temps[var];
      if (!it->nrange) continue;
      DBG("lifetime of %%%d: ", var);
      for (int i = 0; i < it->nrange; ++i) {
         struct range r = itrange(it, i);
         DBG("[%d,%d)%s", r.from, r.to, i < it->nrange-1 ? ", " : "");
      }
      DBG("\n");
   }
   for (struct fixinterval *fx = ra->intervals.fixed; fx; fx = fx->next) {
      DBG("fixed {");
      for (int r = 0; rsiter(&r, fx->rs); ++r)
         DBG("%s,", mctarg->rnames[r]);
      DBG("}: [%d,%d)\n", fx->range.from, fx->range.to);
   }
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

/* quicksort */
static void
sortintervals(struct interval **xs, int lo, int hi)
{
   assert(lo >= 0 && hi >= 0);
   while (lo < hi) {
      /* partition */
      int i = lo - 1, p = hi + 1,
          pivot = itrange(xs[lo], 0).from;
      for (;;) {
         struct interval *tmp;
         do ++i; while (itrange(xs[i], 0).from < pivot);
         do --p; while (itrange(xs[p], 0).from > pivot);
         if (i >= p) break;
         /* swap */
         tmp = xs[i];
         xs[i] = xs[p];
         xs[p] = tmp;
      }
      /* recur */
      if (p + 1 >= hi) {
         hi = p;
      } else {
         if (lo < p)
            sortintervals(xs, lo, p);
         lo = p + 1;
      }
   }
}

static void
linearscan(struct rega *ra)
{
   struct intervals *intervals = &ra->intervals;
   int nunhandled = 0;
   struct interval **unhandled = NULL;
   struct interval *active = NULL, *inactive = NULL, *handled = NULL;

   if (!intervals->count) return;
   /* sort intervals */
   {
      extern int ninstr;
      unhandled = alloc(ra->arena, sizeof *unhandled * intervals->count, 0);

      for (int i = 0; i < ninstr; ++i) {
         if (!intervals->temps[i].nrange) continue;
         unhandled[nunhandled++] = &intervals->temps[i];
      }
      assert(nunhandled == intervals->count);
      sortintervals(unhandled, 0, nunhandled-1);
   }

   /* LINEAR SCAN */
   for (struct interval **pcurrent = unhandled; nunhandled-- > 0; ++pcurrent) {
      struct interval *current = *pcurrent;
      int pos = itrange(current, 0).from;

      /* Expire old intervals */

      /* check for intervals in active that are handled or inactive */
      for (struct interval **lnk = &active, *it = *lnk, *next; (next = it?it->next:0), it; it = next) {
         //DBG("< im active %%%d\n", intervals->temps.mb.k[it - intervals->temps.v]);
         /* ends before position? */
         if (itrange(it, it->nrange-1).to <= pos) {
            /* move from active to handled */
            *lnk = next;
            it->next = handled;
            handled = it;
            if (it->alloc.t == AREG) {
               ra->free |= 1<<it->alloc.a;
               //DBG("   unblock %s\n", mctarg->rnames[it->reg]);
            } else if (it->alloc.t == ASTACK) {
               freestk(ra, it->alloc.a);
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
               DBG(" >> %%%zd unblock %s\n", it-ra->intervals.temps, mctarg->rnames[it->alloc.a]);
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
            if (it->alloc.t == ASTACK) {
               freestk(ra, it->alloc.a);
            }
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
               DBG(" << %%%zd reblock %s\n", it-ra->intervals.temps, mctarg->rnames[it->alloc.a]);
            }
         } else lnk = &it->next;
      }

      /* find a register for current */
      {
         int this = current - intervals->temps;
         regset free = ra->free & (current->fpr ? fpregset : gpregset);
         struct instr *ins = &instrtab[this];
         int reg = 0;
         int end = itrange(current, current->nrange-1).to;

         /* exclude regs from overlapping fixed intervals */
         for (struct fixinterval *fxit = intervals->fixed; fxit; fxit = fxit->next) {
            if (fxit->range.to <= pos) {
               intervals->fixed = fxit->next;
               continue;
            } else if (fxit->range.from >= end) {
               break;
            }

            for (int i = 0; i < current->nrange; ++i) {
               if (rangeoverlap(fxit->range, itrange(current, i))) {
                  free &=~ fxit->rs;
               }
            }
         }
         /* exclude regs from overlapping inactive intervals */
         for (struct interval *it = inactive; it; it = it->next) {
            if (it->alloc.t == AREG && intervaloverlap(it, current)) {
               rsclr(&free, it->alloc.a);
            }
         }
         /* for 2-address instrs, exclude reg from 2nd arg */
         if (ins->inplace && opnarg[ins->op] == 2) {
            int xreg;
            if (ins->r.t == RREG) rsclr(&free, ins->r.i);
            else if (ins->r.t == RTMP && (xreg = instrtab[ins->r.i].reg))
               rsclr(&free, xreg-1);
         }

         if (!free) {
            /* spill */
            current->alloc = allocstk(ra);
            DBG("%%%d got stk%d\n", this, current->alloc.a);
            /* move current to active */
            current->next = active;
            active = current;
            continue;
         }

         /* have free regs, try to use hint */
         if (current->rhint >= 0)
            DBG("have hint %s for %%%zd\n",
                  mctarg->rnames[current->rhint], current - intervals->temps);
         if (current->rhint >= 0 && rstest(free, current->rhint)) {
            DBG(" (used hint)\n");
            reg = current->rhint;
            goto GotReg;
         } else {
            /* for two-address instructions, try to use the reg of left arg */
            if (ins->op != Ophi && (opnarg[ins->op] == 1 || (opnarg[ins->op] == 2 && ins->inplace))) {
               DBG(" %%%d try %d,%d\n", this, ins->l.t,ins->l.i);
               if (ins->l.t == RREG && rstest(free, reg = ins->l.i))
                  goto GotReg;
               if (ins->l.t == RTMP)
               if ((reg = instrtab[ins->l.i].reg-1) >= 0)
               if (rstest(free, reg))
                  goto GotReg;
            /* for phi, try to use reg of any arg */
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

            /* prefer caller-saved registers */
            if (free &~ mctarg->rcallee) free &=~ mctarg->rcallee;

            for (reg = 0; !rstest(free, reg); ++reg);
         }
      GotReg:
         current->alloc = areg(reg);
         ins->reg = reg + 1;
         DBG("%%%d got %s\n", this, mctarg->rnames[reg]);
         rsclr(&ra->free, reg);
         rsset(&ra->fn->regusage, reg);

         //if current has a register assigned then add current to active
         current->next = active;
         active = current;
      }
   }
}

/* replace temps with physical regs, add loads & stores for spilled temps */
static bool
devirt(struct rega *ra, struct block *blk)
{
   bool allnops = 1;
   struct function *fn = ra->fn;
   for (int curi = 0; curi < blk->ins.n; ++curi) {
      int temp = blk->ins.p[curi];
      struct instr *ins = &instrtab[temp];
      struct interval *it;
      struct alloc *alloc;
      struct addr newaddr;
      union ref *argref[4];
      int curi0;
      int naddr = 0;
      int nargref = 0;
      int nspill = 0;

      /* devirtualize ref args */
      for (int i = 0; i < 2; ++i) {
         union ref *r = &i[&ins->l];
         if (r->t == RADDR) {
            struct addr *a = &addrht[r->i];
            ++naddr;
            newaddr = *a;
            argref[nargref++] = &newaddr.base;
            argref[nargref++] = &newaddr.index;
         } else {
            argref[nargref++] = r;
         }
      }
      assert(naddr < 2);

      for (int i = 0; i < nargref; ++i) {
         static uchar cls2load[] = {
            [KI4] = Oloads4, [KI8] = Oloadi8, [KF4] = Oloadf4, [KF8] = Oloadf8, [KPTR] = 0
         };
         cls2load[KPTR] = targ_64bit ? Oloadi8 : Oloads4;
         union ref *r = argref[i];
         int tr;
         if (r->t == RTMP) {
            alloc = (it = &ra->intervals.temps[r->i]) && it->nrange ? &it->alloc : NULL;
            if (alloc->t == ASTACK && ins->op == Omove) {
               /* move [reg], [stk] -> [reg] = load [stk] */
               assert(r == &ins->r && ins->l.t == RREG);
               ins->reg = ins->l.i+1;
               ins->op = cls2load[ins->cls];
               ins->r = NOREF;
               addstkslotref(temp, alloc->a*8);
            } else if (alloc->t == ASTACK && ins->op == Ocopy && r == &ins->l) {
               /* [reg] = copy [stk] -> [reg] = load [stk] */
               assert(ins->reg);
               ins->op = cls2load[ins->cls];
               addstkslotref(temp, alloc->a*8);
            } else if (alloc->t == ASTACK) {
               /* ref was spilled, gen load to scratch register and use it */
               struct instr ld = {.cls = insrescls(instrtab[r->i])};
               int reg = kisint(ld.cls) ? mctarg->gprscratch : mctarg->fprscratch;
               bool dosave;
               /* pick scratch register, or any register that doesn't conflict with this instr's srcs/dst */
               if (nspill > 0) {
                  for (reg = kisflt(ld.cls) ? mctarg->fpr0 : mctarg->gpr0;; ++reg) {
                     if (reg == ins->reg-1) continue;
                     for (int j = 0; j < i; ++j)
                        if (argref[j]->t == RREG && argref[j]->i == reg) continue;
                     break;
                  }
                  /* if not the designated scratch register, we need to save+restore */
                  if ((dosave = rstest(fn->regusage, reg) || rstest(mctarg->rcallee, reg))) {
                     insertinstr(blk, curi++, mkinstr(Oxsave, 0, .l = mkref(RREG, reg)));
                  }
               }
               ld.reg = reg+1;
               ld.op = cls2load[ld.cls];
               addstkslotref(insertinstr(blk, curi++, ld).i, alloc->a*8);
               *r = mkref(RREG, reg);
               if (nspill > 0 && dosave) {
                  insertinstr(blk, curi+1, mkinstr(Oxrestore, 0, .l = mkref(RREG, reg)));
               }
               ++nspill;
            } else if ((tr = instrtab[r->i].reg)) {
               assert(alloc && alloc->t == AREG && alloc->a == tr-1);
               *r = mkref(RREG, tr-1);
            }
         }
      }
      if (nspill > 0) assert(ins->op != Ocall);
      if (naddr) {
         union ref *r = ins->l.t == RADDR ? &ins->l : &ins->r;
         *r = mkaddr(newaddr);
      }

      /* devirtualize destination */
      curi0 = curi;
      alloc = temp < ra->intervals.ntemps && (it = &ra->intervals.temps[temp]) && it->nrange ? &it->alloc : NULL;
      if (alloc && alloc->t == ASTACK) {
         enum irclass cls = insrescls(*ins);
         int store = Ostore1 + ilog2(cls2siz[cls]);
         /* t was spilled, gen store */
         if (ins->op == Ocopy && ins->l.t != RADDR) {
            ins->op = store;
            ins->r = ins->l;
            addstkslotref(temp, alloc->a*8);
         } else {
            bool dosave = 0;
            int reg = kisint(insrescls(*ins)) ? mctarg->gprscratch : mctarg->fprscratch;
            if (nspill > 0) {
               for (reg = kisflt(cls) ? mctarg->fpr0 : mctarg->gpr0;; ++reg) {
                  for (int j = 0; j < nargref; ++j)
                     if (argref[j]->t == RREG && argref[j]->i == reg) goto NotThis;
                  break;
               NotThis:;
               }
               /* if not the designated scratch register, we need to save+restore */
               if ((dosave = rstest(fn->regusage, reg) || rstest(mctarg->rcallee, reg))) {
                  insertinstr(blk, curi++, mkinstr(Oxsave, 0, .l = mkref(RREG, reg)));
                  curi0 = curi;
               }
            }
            ins->reg = reg+1;
            addstkslotref(
                  insertinstr(blk, ++curi, mkinstr(store, 0, .r = mkref(RREG, reg))).i,
                  alloc->a*8);
            if (nspill > 0 && dosave) {
               insertinstr(blk, ++curi, mkinstr(Oxrestore, 0, .l = mkref(RREG, reg)));
            }
         }
      }
      if (!ins->reg && insrescls(*ins) && ins->op != Omove && !ins->keep && !in_range(ins->op, Ostore1, Ostore8)) {
         /* dead */
      Nop:
        ins->op = Onop;
      } else if (ins->op == Omove && ins->r.t == RREG && ins->l.i == ins->r.i) {
         /* move r1,r2  /  r1=r2 */
         goto Nop;
      } else if (ins->op == Ocopy && ins->l.t == RREG && ins->reg-1 == ins->l.i) {
         /* r1 = copy r2  /  r1=r2 */
         goto Nop;
      } else if (ins->op != Onop) {
         allnops = 0;
      }
      if (ins->inplace && ins->l.t == RREG && ins->reg && ins->reg-1 != ins->l.i) {
         /* fixup in-place (two-address) instructions */
         allnops = 0;
         insertinstr(blk, curi0, mkmove(ins->cls, ins->reg-1, ins->l.i));
         ++curi;
         ins->l.i = ins->reg-1;
      }
      if (!ins->reg && in_range(ins->op, Oloads1, Oloadf8)) {
         assert(ins->keep);
         ins->reg = kisint(ins->cls) ? mctarg->gprscratch+1 : mctarg->fprscratch+1;
      }
   }

   return allnops;
}

static void
fini(struct rega *ra)
{
   int id = 0;
   struct function *fn = ra->fn;
   struct block *blk = fn->entry;

   do {
      bool allnops;

      blk->id = id++;
      allnops = devirt(ra, blk);

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
}

void
regalloc(struct function *fn)
{
   static union ref *stkslotrefsbuf[64];
   struct rega ra = {fn, .arena = fn->arena};
   struct block *blk, *last;

   /* setup */
   if (!fpregset || !gpregset) {
      for (int r = 0; r < MAXREGS; ++r) {
         if (isfpr(r))
            rsset(&fpregset, r);
         else if (isgpr(r))
            rsset(&gpregset, r);
      }
   }
   ra.free = (gpregset | fpregset) &~ (mctarg->rglob | (1ull<<mctarg->gprscratch) | (1ull<<mctarg->fprscratch));
   memset(ra.freestk, 0xFF, sizeof ra.freestk);
   fn->regusage = 0;
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

   /* linear scan: assign physical registers and stack slots */
   linearscan(&ra);

   /* get out of SSA */
   blk = last = fn->entry->lprev;
   do {
      if (blk->id < 0) continue;
      for (int i = 0; i < blk->npred; ++i) {
         lowerphis(&ra, blkpred(blk, i), blk);
      }
      vfree(&blk->phi);
   } while ((blk = blk->lprev) != last);

   /* devirtualize & final cleanup */
   fini(&ra);

   for (struct interval *it = ra.intervals.temps; ra.intervals.count > 0; ++it) {
      if (it->nrange > 2) xbfree(it->_dyn);
      if (it->nrange > 0) --ra.intervals.count;
   }

   fn->stksiz += ra.maxstk*8;
   if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
   while (stkslotrefs.n) {
      union ref *adr = stkslotrefs.p[--stkslotrefs.n];
      *adr = mkaddr((struct addr) { .base = mkref(RREG, mctarg->bpr), .disp = -fn->stksiz + adr->i });
   }
   vfree(&stkslotrefs);

   if (ccopt.dbg.r) {
      DBG("<< After regalloc >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
