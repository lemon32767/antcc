#include "ir.h"
#include "u_bits.h"

/** Implements linear scan register allocation **/
/* Some references:
 * Linear Scan Register Allocation on SSA Form (Wimmer 2010)
     - https://c9x.me/compile/bib/Wimmer10a.pdf
 * Linear Scan Register Allocation in the Context of SSA Form
   and Register Constraints (Mössenböck 2002)
     - https://bernsteinbear.com/assets/img/linear-scan-ra-context-ssa.pdf
 */

#if 1
#define DBG(...) if(ccopt.dbg.r) bfmt(ccopt.dbgout, __VA_ARGS__)
#else
#define DBG(...) ((void)0)
#endif

/* The algorithm used here to introduce phis for temporaries whose definitions
 * appear later than some of its uses is very similar to that in mem2reg() */

static int livelastblk;
typedef struct { ushort var, phi; } PendingPhi;
static vec_of(PendingPhi) *pendingphis;
static int npendingphi;
static ushort **curdefs;
static Ref readvar(BitSet *defined, enum irclass cls, int var, Block *blk);

static void
fillphi(BitSet *defined, Ref phi, enum irclass cls, int var, Block *blk)
{
   Ref *args = phitab.p[instrtab[phi.i].l.i];
   assert(blk->npred > 0);
   for (int i = 0; i < blk->npred; ++i)
      args[i] = readvar(defined, cls, var, blk);
}

static Ref
readvar(BitSet *defined, enum irclass cls, int var, Block *blk)
{
   Ref val;

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
      vpush(&pendingphis[blk->id], ((PendingPhi) { var, val.i }));
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
liveuse(BitSet *defined, Instr *ins, Ref *r, Block *blk)
{
   int var;
   if (r->t == RADDR) {
      liveuse(defined, ins, &addrtab.p[r->i].base, blk);
      liveuse(defined, ins, &addrtab.p[r->i].index, blk);
      return;
   } else if (r->t != RTMP) return;
   var = r->i;
   if (bstest(defined, var)) return;

   *r = readvar(defined, insrescls(instrtab[r->i]), var, blk);
}

/* regalloc() assumes every use of a temporary is visited before its definition
 * so this pass fixes cases where that would not apply by introducing phi functions */
static void
fixlive(Function *fn)
{
   extern int ninstr;
   Block *blk = fn->entry;
   BitSet definedbuf[4] = {0};
   BitSet *defined = definedbuf;

   if (BSSIZE(ninstr) >= countof(definedbuf))
      defined = xcalloc(sizeof *defined * BSSIZE(ninstr));
   npendingphi = 0;

   do {
      for (int i = 0; i < blk->phi.n; ++i) {
         int var = blk->phi.p[i];
         bsset(defined, var);
      }

      for (int i = 0; i < blk->ins.n; ++i) {
         int var = blk->ins.p[i];
         Instr *ins = &instrtab[var];
         if (ins->l.t) liveuse(defined, ins, &ins->l, blk);
         if (ins->r.t) liveuse(defined, ins, &ins->r, blk);
         bsset(defined, var);
      }
   } while ((blk = blk->lnext) != fn->entry);

   do {
      vec_of(PendingPhi) *pphi;

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
      bfmt(ccopt.dbgout, "<< After liveness fixup >>\n");
      irdump(fn);
   }
   if (defined != definedbuf) free(defined);
}

static regset gpregset, fpregset;

#define isfpr(reg) in_range((reg), mctarg->fpr0, mctarg->fpr0 + mctarg->nfpr - 1)
#define isgpr(reg) in_range((reg), mctarg->gpr0, mctarg->gpr0 + mctarg->nfpr - 1)

/* an allocated physical register or stack slot */
enum { ADEAD, AREG, ASTACK };
typedef union { struct { ushort t : 2, a : 14; }; ushort bits; } Alloc;
#define afree() ((Alloc) { .t=ADEAD })
#define areg(r) ((Alloc) { .t=AREG, .a=(r) })
#define astack(s) ((Alloc) { .t=ASTACK, .a=(s) })

enum { MAXSPILL = 512 };

/* half-closed instr range [from, to) */
typedef struct { ushort from, to; } Range;

/* a temporary's lifetime interval */
typedef struct Interval {
   struct Interval *next; /* for linked list of active,inactive,handled sets in linear scan */
   Alloc alloc;
   schar rhint : 7; /* register hint */
   bool fpr : 1; /* needs float register? */
   uint cost; /* spilling cost estimate */

   /* sorted ranges array */
   ushort nrange;
   union {
      Range _rinl[2];
      Range *_rdyn;
   };
} Interval;

/* fixed intervals represent register constraints */
typedef struct FixInterval {
   struct FixInterval *next;
   regset rs;
   Range range;
   /* unlike Interval, there is only one range because it's unnecessary to take
    * gaps into account, since they are already "allocated"; the same
    * register(s) can appear in different disjoint FixIntervals */
} FixInterval;

typedef struct RegAlloc {
   Function *fn;
   Arena **arena;

   int intercount; /* number of actual intervals */
   int ninter; /* size of inter */
   Interval *inter; /* map of tmp -> interval */
   FixInterval *fixed; /* linked list of fixed intervals, always sorted */

   BitSet freestk[BSSIZE(MAXSPILL)]; /* free stack slots */
   int maxstk, /* highest stack slot used */
       stktop;
} RegAlloc;

#define stkslotref(fn, off)                             \
      mkaddr((IRAddr){.base = mkref(RREG, mctarg->bpr), \
                      .disp = -(fn)->stksiz - 8 - (off)})

/* Parallel moves algorithm from QBE
 * <https://c9x.me/git/qbe.git/tree/rega.c?id=e493a7f23352f51acc0a1e12284ab19d7894488a#n201> */

enum pmstat { PMTOMOVE, PMMOVING, PMDONE };
typedef struct {
   Function *fn;
   int npmove;
   struct PMove {
      uchar k; /* enum irclass */
      uchar stat; /* enum pmstat */
      Alloc dst, src;
   } pmove[MAXREGS];
} PMState;

static void
pmadd(PMState *pms, enum irclass k, Alloc dst, Alloc src)
{
   if (!memcmp(&dst, &src, sizeof dst)) return;
   assert(pms->npmove < MAXREGS);
   pms->pmove[pms->npmove++] = (struct PMove) { k, PMTOMOVE, dst, src };
}

#define mkmove(k, rd, rs) mkinstr(Omove, k, mkref(RREG, rd), mkref(RREG, rs))
static void
emitmove(Function *fn, enum irclass k, Alloc dst, Alloc src, Block *blk, int curi)
{
   Instr mv = {.keep = 1};
   int reg;
   if (dst.t == AREG && src.t == AREG) {
      insertinstr(blk, curi, mkmove(k, dst.a, src.a));
      return;
   }
   if (src.t == ASTACK) {
      switch (mv.cls = k) {
      default: assert(0);
      case KI32: mv.op = Oloads32; break;
      case KI64: mv.op = Oloadi64; break;
      case KPTR: mv.op = targ_64bit ? Oloadi64 : Oloads32; break;
      case KF32: mv.op = Oloadf32; break;
      case KF64: mv.op = Oloadf64; break;
      }
      if (dst.t == AREG)
         reg = dst.a;
      else
         reg = kisint(k) ? mctarg->gprscratch : mctarg->fprscratch;
      mv.reg = reg+1;
      mv.l = stkslotref(fn, src.a*8);
      insertinstr(blk, curi++, mv);
   } else reg = src.a;
   if (dst.t == ASTACK) {
      mv = mkinstr(cls2store[k], 0, stkslotref(fn, dst.a*8), mkref(RREG, reg));
      insertinstr(blk, curi, mv);
   }
}

static int
pmrec(PMState *pms, int i, Block *blk, int curi, enum irclass *k)
{
   struct PMove *pm = &pms->pmove[i];
   if (pm->dst.bits == pm->src.bits) {
      pm->stat = PMDONE;
      return -1;
   }

   /* widen when necessary */
   assert(kisint(pm->k) == kisint(*k));
   if (cls2siz[pm->k] > cls2siz[*k])
      *k = pm->k;

   int j, c;
   for (j = 0; j < pms->npmove; ++j) {
      if (pms->pmove[j].dst.bits == pm->src.bits)
         break;
   }
   if (j == pms->npmove) goto Done;
   switch (pms->pmove[j].stat) {
   default: assert(0);
   case PMMOVING:
      c = j;
   Swap:
      if (pm->src.t == AREG && pm->dst.t == AREG) {
         insertinstr(blk, curi,
               mkinstr(Oswap, *k, mkref(RREG, pm->dst.a), mkref(RREG, pm->src.a), .keep = 1));
      } else if (pm->src.t != pm->dst.t) {
         Alloc reg, stk, regtmp;
         if (pm->src.t == AREG)
            reg = pm->src, stk = pm->dst;
         else
            stk = pm->src, reg = pm->dst;
         assert(reg.t == AREG && stk.t == ASTACK);
         regtmp = areg(kisint(*k) ? mctarg->gprscratch : mctarg->fprscratch);
         emitmove(pms->fn, *k, regtmp, stk, blk, curi++);
         insertinstr(blk, curi++,
               mkinstr(Oswap, *k, mkref(RREG, reg.a), mkref(RREG, regtmp.a), .keep = 1));
         emitmove(pms->fn, *k, stk, regtmp, blk, curi++);
      } else {
         /* FIXME using scratch gpr and fpr for this is hackish */
         assert(pm->src.t == ASTACK && pm->dst.t == ASTACK);
         int r1 = mctarg->gprscratch, r2 = mctarg->fprscratch;
         enum irclass k1 = siz2intcls[cls2siz[*k]], k2 = KF32 + (cls2siz[*k] == 8);
         emitmove(pms->fn, k1, areg(r1), pm->src, blk, curi++);
         emitmove(pms->fn, k2, areg(r2), pm->dst, blk, curi++);
         emitmove(pms->fn, k1, pm->dst, areg(r1), blk, curi++);
         emitmove(pms->fn, k2, pm->src, areg(r2), blk, curi++);
      }
      break;
   case PMTOMOVE:
      pm->stat = PMMOVING;
      c = pmrec(pms, j, blk, curi, k);
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
      emitmove(pms->fn, *k, pm->dst, pm->src, blk, curi);
      break;
   }

   pm->stat = PMDONE;
   return c;
}

static void
emitpm(PMState *pms, Block *blk)
{
   int curi = blk->ins.n;
   for (int i = 0; i < pms->npmove; ++i) {
      if (pms->pmove[i].stat == PMTOMOVE) {
         pmrec(pms, i, blk, curi, &(enum irclass) { pms->pmove[i].k });
      }
   }
}

/* remove phis by inserting parallel moves */
static void
lowerphis(RegAlloc *ra, Block *blk, Block *suc)
{
   int predno;
   Block *n = NULL;

   if (!blk->npred && blk != ra->fn->entry) {
      assert(!blk->phi.n);
      blk->ins.n = 0;
      return;
   }
   if (!blk->s2) n = blk;

   for (predno = 0; predno < suc->npred; ++predno)
      if (blkpred(suc, predno) == blk)
         break;
   assert(predno < suc->npred);

   PMState pms;
   pms.fn = ra->fn;
   pms.npmove = 0;
   /* ensure phi args go to the same slot as phi with parallel copies */
   for (int i = 0; i < suc->phi.n; ++i) {
      Instr *phi = &instrtab[suc->phi.p[i]];
      Ref *arg = &phitab.p[phi->l.i][predno];
      Alloc from, to;

      if (arg->t == RREG) continue;
      assert(arg->t == RTMP);
      DBG("resolve phi @%d, @%d, %%%d <- %%%d\n", blk->id, suc->id, phi - instrtab, arg->i);
      if (instrtab[arg->i].reg) {
         from = areg(instrtab[arg->i].reg - 1);
         DBG("  it had R%d\n", from.a);
      } else {
         from = ra->inter[arg->i].alloc;
         assert(from.t != ADEAD);
         DBG("  found %c%d\n", " RS"[from.t], from.a);
         if (from.t == AREG)
            instrtab[arg->i].reg = from.a+1;
      }
      if (phi->reg) {
         to = areg(phi->reg - 1);
         DBG("  phi had R%d\n", to.a);
      } else {
         to = ra->inter[phi - instrtab].alloc;
         if (to.t == ADEAD) {
            DBG("  skip dead phi\n");
            continue;
         }
         DBG("  found phi %c%d\n", " RS"[to.t], to.a);
         if (to.t == AREG)
            phi->reg = to.a+1;
      }
      DBG(" > phi move %c%d -> %c%d\n", " RS"[from.t], from.a, " RS"[to.t], to.a);
      if (!n) n = insertblk(ra->fn, blk, suc);
      pmadd(&pms, phi->cls, to, from);
   }
   if (n) emitpm(&pms, n);
}

/* generate copies for phi operands to transform into conventional-SSA */
static void
fixcssa(Function *fn)
{
   Block *blk = fn->entry;
   do {
      if (!blk->phi.n) continue;
      for (int p = 0; p < blk->npred; ++p) {
         Block *n, *pred = blkpred(blk, p);
         if (!pred->s2) {
            /* pred only has 1 successor (blk), so insert move directly in it */
            n = pred;
         } else {
            n = insertblk(fn, pred, blk);
            assert(n->jmp.t == Jb && n->s1 == blk);
         }
         for (int i = 0; i < blk->phi.n; ++i) {
            int phi = blk->phi.p[i];
            Ref *args = phitab.p[instrtab[phi].l.i];
            args[p] = insertinstr(n, n->ins.n, mkinstr(Ocopy, instrtab[phi].cls, args[p]));
         }
      }
   } while ((blk = blk->lnext) != fn->entry);

   fn->prop &= ~FNBLKID;
}

static inline bool
rangeoverlap(Range a, Range b)
{
   return a.from < b.to && b.from < a.to;
}

static void
pushrange(Interval *it, Range r)
{
   if (it->nrange < 2) it->_rinl[it->nrange++] = r;
   else if (it->nrange > 2) xbpush(&it->_rdyn, &it->nrange, r);
   else {
      Range *d = NULL;
      xbgrow(&d, 4);
      memcpy(d, it->_rinl, 2*sizeof *d);
      d[it->nrange++] = r;
      it->_rdyn = d;
   }
}
#define itrange(it, i) ((it)->nrange <= 2 ? (it)->_rinl : (it)->_rdyn)[i]

static inline int
intervalbeg(Interval *it)
{
   assert(it->nrange);
   return itrange(it, 0).from;
}

static inline int
intervalend(Interval *it)
{
   assert(it->nrange);
   return itrange(it, it->nrange-1).to;
}

static bool
intersoverlap(Interval *a, Interval *b)
{
   for (int i = 0, j = 0; i < a->nrange && j < b->nrange; ) {
      Range r1 = itrange(a, i), r2 = itrange(b, j);
      if (rangeoverlap(r1, r2)) return 1;
      if (r1.to <= r2.from) ++i;
      else ++j;
   }
   return 0;
}

static inline void
incrcost(Interval *it, Block *blk)
{
   /* treat each loop as executing instr 8 times */
   it->cost += 1 << (blk->loop * 3);
}

static bool
intervaldef(RegAlloc *ra, int t, Block *blk, int pos, int reghint)
{
   Interval *it = &ra->inter[t];
   if (it->nrange) {
      ushort *beg = &itrange(it, 0).from;
      assert(*beg <= pos);
      incrcost(it, blk);
      *beg = pos;
      return 1;
   }
   return 0;
}

static void
addrange(RegAlloc *ra, int t, Range new, int reghint)
{
   Interval *it = &ra->inter[t];
   Range *fst;
   int n;

   if (!it->nrange) {
      ++ra->intercount;
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
      memmove(&itrange(it, 1), &itrange(it, 0), sizeof(Range) * (it->nrange - 1));
      itrange(it, 0) = new;
   }

   /* new range might cover existing ranges (loop header lives),
    * check and succesively merge */
   fst = &itrange(it, 0);
   n = 0;
   for (int i = 1; i < it->nrange; ++i) {
      Range other = itrange(it, i);
      if (fst->to >= other.from) {
         fst->to = fst->to > other.to ? fst->to : other.to;
         ++n;
      } else break;
   }

   if (n > 0) {
      for (int i = 1; i + n < it->nrange; ++i)
         itrange(it, i) = itrange(it, i+n);
      if (it->nrange > 2 && it->nrange - n <= 2) {
         Range *dyn = it->_rdyn;
         memcpy(it->_rinl, dyn, (it->nrange - n) * sizeof *dyn);
         xbfree(dyn);
      }
      it->nrange -= n;
   }
}

static void
usereg(RegAlloc *ra, int reg, Block *blk, int pos)
{
   FixInterval *fxit;
   if (rstest(mctarg->rglob, reg)) return; /* regalloc never allocates globally live regs, so don't need intervals for those */
   for (FixInterval *prev = NULL, *fxit = ra->fixed; fxit; prev = fxit, fxit = fxit->next) {
      if (fxit->range.from > pos) break;
      if (fxit->rs == BIT(reg) && fxit->range.from <= pos && pos < fxit->range.to) {
         /* contained by existing interval */
         fxit->range.from = blk->inumstart;
         /* insert at head */
         //DBG(">>>extend REG %s range %d-%d\n", mctarg->rnames[reg], fxit->range.from, fxit->range.to);
         if (prev) {
            prev->next = fxit->next;
            fxit->next = ra->fixed;
            ra->fixed = fxit;
         }
         return;
      }
   }
   fxit = alloc(ra->arena, sizeof *fxit, 0);
   fxit->next = ra->fixed;
   fxit->range = (Range) {blk->inumstart, pos};
   fxit->rs = BIT(reg);
   ra->fixed = fxit;
}

static bool
defreg(RegAlloc *ra, int reg, int pos) {
   if (rstest(mctarg->rglob, reg)) return 1;
   for (FixInterval *prev = NULL, *fxit = ra->fixed; fxit; prev = fxit, fxit = fxit->next) {
      if (fxit->rs == BIT(reg)) {
         if (fxit->range.from <= pos) {
            fxit->range.from = pos;
            FixInterval **at = &ra->fixed;
            if ((*at)->range.from > pos) {
               /* keep sorted */
               //DBG("moved %s\n", mctarg->rnames[reg]);
               if (prev) prev->next = fxit->next;
               while ((*at)->range.from < pos) at = &(*at)->next;
               fxit->next = *at;
               *at = fxit;
            }
            //DBG(">>>def REG %s range %d-%d\n", mctarg->rnames[reg], fxit->range.from, fxit->range.to);
            return 1;
         }
         break;
      }
   }
   return 0;
}

/* lifetime interval construction */
static void
buildintervals(RegAlloc *ra)
{
   extern int ninstr;
   Block *blk, *last;
   BitSet **livein = alloc(ra->arena, ra->fn->nblk * sizeof *livein, 0);
   size_t bssize = BSSIZE(ninstr);
   struct Loop {
      struct Loop *next;
      Block *hdr, *end;
      /* list of loops */
   } *loops = NULL;
   for (int i = 0; i < ra->fn->nblk; ++i)
      livein[i] = allocz(ra->arena, bssize * sizeof *livein[i], 0);
   ra->inter = allocz(ra->arena, ninstr * sizeof *ra->inter, 0);
   ra->ninter = ninstr;

   uint n = numberinstrs(ra->fn);
   assert((ushort)n == n && "too many instrs for Range");
   /* visit blocks in reverse, to build lifetime intervals */
   blk = last = ra->fn->entry->lprev;
   do {
      BitSet *live = livein[blk->id];
      /* live = union of successor.liveIn for each successor of b */
      if (blk->s1) bsunion(live, livein[blk->s1->id], bssize);
      if (blk->s2) bsunion(live, livein[blk->s2->id], bssize);

      /* for each phi function phi of successors of b do
       *    live.add(phi.inputOf(b))
       */
      for (Block *suc = blk->s1; suc; suc = blk->s2) {
         int predno;
         for (predno = 0; blkpred(suc, predno) != blk; ++predno) ;
         for (int i = 0; i < suc->phi.n; ++i) {
            Instr *phi = &instrtab[suc->phi.p[i]];
            Ref *arg = &phitab.p[phi->l.i][predno];
            assert(arg->t == RTMP);
            bsset(live, arg->i);
            incrcost(&ra->inter[arg->i], blk);
         }
         if (suc == blk->s2) break;
      }

      /* for each opd in live do
       *    intervals[opd].addRange(b.from, b.to)
       */
      for (uint i = 0; bsiter(&i, live, bssize); ++i) {
         addrange(ra, i, (Range){blk->inumstart, blk->inumstart + blk->ins.n + 2}, -1);
      }

      /* for each operation op of b in reverse order do */
      Instr *ins = NULL;
      Ref queue[8] = { blk->jmp.arg[0], blk->jmp.arg[1] };
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
         if (!intervaldef(ra, out, blk, pos, reghint)) {
            if (insrescls(*ins) && ins->op != Omove && !ins->keep && !(ins->op == Ocopy && ins->l.t == RREG)) {
               /* dead */
               *ins = mkinstr(Onop,0,);
            }
         }
         bsclr(live, out);

         /* gather fixed intervals */
         if (ins->op == Omove) {
            assert(ins->l.t == RREG);
            if (ins->l.bits == ins->r.bits) {/* special case `move Rx,Rx`: clobber reg, not a real use */
               usereg(ra, ins->l.i, blk, pos);
               assert(defreg(ra, ins->l.i, pos));
            } else if (!defreg(ra, ins->l.i, pos)) {
               if (ins->keep) { /* clobber here */
                  usereg(ra, ins->l.i, blk, pos);
                  assert(defreg(ra, ins->l.i, pos));
               } else {
                  /* dead register use. for example if
                   *   move RCX, %1
                   *   %2 = shl 1, RCX
                   * and %2 is dead, the move to RCX can be killed */
                  *ins = mkinstr(Onop,0,);
               }
            } else {
               rsset(&ra->fn->regusage, ins->l.i);
            }
            if (ins->l.bits == ins->r.bits)
               continue;
         } else if (ins->op == Ocall) {
            IRCall *call = &calltab.p[ins->r.i];
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
               FixInterval *fxit = alloc(ra->arena, sizeof *fxit, 0);
               fxit->next = ra->fixed;
               fxit->range = (Range){pos, pos};
               fxit->rs = rclob;
               ra->fixed = fxit;
            }
            for (int j = call->narg - 1; j >= 0; --j) {
               ABIArg abi = call->abiarg[j];
               if (!abi.isstk) {
                  usereg(ra, abi.reg, blk, pos);
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
            Ref r = queue[--nqueue];

            /* do not allocate a reg for a cmp op used as branch argument, since it's a pseudo op */
            if (curi == blk->ins.n && blk->jmp.t == Jb && r.t == RTMP && instrtab[r.i].keep)
               continue;

            if (r.t == RTMP) {
               assert(instrtab[r.i].op != Onop);
               incrcost(&ra->inter[r.i], blk);
               addrange(ra, r.i, (Range){blk->inumstart, pos}, reghint);
               bsset(live, r.i);
            } else if (r.t == RREG) {
               usereg(ra, r.i, blk, pos);
            } else if (r.t == RADDR) {
               reghint = -1;
               queue[nqueue++] = addrtab.p[r.i].base;
               queue[nqueue++] = addrtab.p[r.i].index;
            }
         }
      }

      /* for each phi function phi of b do
       *    live.remove(phi.output)
       */
      for (int i = 0; i < blk->phi.n; ++i) {
         int phi = blk->phi.p[i];
         bsclr(live, phi);
         for (int i = 0; i < blk->npred; ++i)
            incrcost(&ra->inter[phi], blkpred(blk, i));
      }

      /* if b is loop header then
       *    loopEnd = last block of the loop starting at b
       *    for each opd in live do
       *       intervals[opd].addRange(b.from, loopEnd.to)
       */
      Block *loopend = NULL;
      for (int i = 0; i < blk->npred; ++i) {
         Block *pred = blkpred(blk, i);
         if (pred->id > blk->id)
            loopend = loopend && loopend->id > pred->id ? loopend : pred;
      }
      if (loopend) {
         if (loops) DBG("@lp @%d\n", blk->id);
         for (struct Loop *l = loops; l; l = l->next) {
            /* a nested loop might end later than loopend, which lengthens this outer loop. */
            /* XXX is this correct? more loop analysis might be required? */
            if (l->hdr->id > loopend->id) break;
            DBG("  check <@%d-@%d>\n", l->hdr->id, l->end->id);
            if (l->hdr->id > blk->id && l->hdr->id < loopend->id && l->end->id > loopend->id)
               loopend = l->end;
         }
         DBG("loop header @%d (to @%d)\n", blk->id, loopend->id);
         /* append to loop list */
         loops = alloccopy(ra->arena, &(struct Loop){loops, blk, loopend}, sizeof *loops, 0);
         for (uint opd = 0; bsiter(&opd, live, bssize); ++opd) {
            // DBG("  i have live %%%d\n", opd);
            addrange(ra, opd, (Range){blk->inumstart, loopend->inumstart + loopend->ins.n+1}, -1);
         }
      }
   } while ((blk = blk->lprev) != last);

   if (ccopt.dbg.r) {
      for (int var = 0; var < ninstr; ++var) {
         Interval *it = &ra->inter[var];
         if (!it->nrange) continue;
         DBG("lifetime of %%%d: ", var);
         for (int i = 0; i < it->nrange; ++i) {
            Range r = itrange(it, i);
            DBG("[%d,%d)%s", r.from, r.to, i < it->nrange-1 ? ", " : "");
         }
         DBG("  spill cost: %d\n", it->cost);
      }
      for (FixInterval *fx = ra->fixed; fx; fx = fx->next) {
         DBG("fixed {");
         for (int r = 0, f=1; rsiter(&r, fx->rs); ++r, f=0)
            DBG(&" %s"[f], mctarg->rnames[r]);
         DBG("}: [%d,%d)\n", fx->range.from, fx->range.to);
      }
   }
}

static bool
itcontainspos(Interval *it, int pos)
{
   for (int i = 0; i < it->nrange; ++i) {
      Range r = itrange(it, i);
      if (r.from > pos) return 0;
      if (pos < r.to) return 1;
   }
   return 0;
}

/* quicksort */
static void
sortintervals(Interval **xs, int lo, int hi)
{
   assert(lo >= 0 && hi >= 0);
   while (lo < hi) {
      /* partition */
      int i = lo - 1, p = hi + 1,
          pivot = intervalbeg(xs[lo]);
      for (;;) {
         Interval *tmp;
         do ++i; while (intervalbeg(xs[i]) < pivot);
         do --p; while (intervalbeg(xs[p]) > pivot);
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

static Alloc
allocstk(RegAlloc *ra)
{
   uint s = 0;
   if (bsiter(&s, ra->freestk, BSSIZE(MAXSPILL))) {
      bsclr(ra->freestk, s);
      if (ra->stktop < s) ra->stktop = s+1;
   } else {
      s = ra->stktop++;
   }
   if (ra->maxstk < s+1) ra->maxstk = s+1;
   return astack(s);
}

static void
freestk(RegAlloc *ra, int slot)
{
   DBG("FREE stk %d\n",slot);
   if (slot < MAXSPILL)
      bsset(ra->freestk, slot);
   else if (slot == ra->stktop - 1)
      --ra->stktop;
}

#define interval2temp(it) (int)(it - ra->inter)

static void
linearscan(RegAlloc *ra)
{
   if (!ra->intercount) return;

   /* sort intervals */
   Interval **unhandled = alloc(ra->arena, sizeof *unhandled * ra->intercount, 0);
   int nunhandled = 0;
   for (int i = 0; i < ra->ninter; ++i) {
      if (!ra->inter[i].nrange) continue;
      unhandled[nunhandled++] = &ra->inter[i];
   }
   assert(nunhandled == ra->intercount);
   sortintervals(unhandled, 0, nunhandled-1);

   regset freeregs = (gpregset | fpregset) &~ (mctarg->rglob | (1ull<<mctarg->gprscratch) | (1ull<<mctarg->fprscratch));
   memset(ra->freestk, 0xFF, sizeof ra->freestk);

   /* LINEAR SCAN */
   Interval *actives[2] = {0}, /* gpr set and fpr set */
                   *inactives[2] = {0},
                   *spilled = NULL, **spilled_tail = &spilled;
   for (Interval **pcurrent = unhandled; nunhandled > 0; ++pcurrent, --nunhandled) {
      Interval *current = *pcurrent;
      int pos = intervalbeg(current);

      Interval **active = &actives[current->fpr], **inactive = &inactives[current->fpr],
                      **lnk, *it, *next;
      /* Expire old intervals */
      /* check for intervals in active that are handled or inactive */
      for (lnk = active, it = *lnk; (next = it?it->next:0), it; it = next) {
         assert(it->alloc.t == AREG);
         /* ends before position? */
         if (intervalend(it) <= pos) {
            /* move from active to handled */
            *lnk = next;
            //DBG("   unblock %s %X\n", mctarg->rnames[it->alloc.a], ra->free);
            rsset(&freeregs, it->alloc.a);
         } else if (!itcontainspos(it, pos)) { /* it does not cover position? */
            /* move from active to inactive */
            *lnk = next;
            it->next = *inactive;
            *inactive = it;
            rsset(&freeregs, it->alloc.a);
            DBG(" >> %%%zd unblock %s\n", interval2temp(it), mctarg->rnames[it->alloc.a]);
         } else lnk = &it->next;
      }
      /* check for intervals in inactive that are handled or active */
      for (lnk = inactive, it = *lnk; (next = it?it->next:0), it; it = next) {
         assert(it->alloc.t == AREG);
         /* ends before position? */
         if (intervalend(it) <= pos) {
            /* move from inactive to handled */
            *lnk = next;
         } else if (itcontainspos(it, pos)) { /* it covers position? */
            /* move from inactive to active */
            *lnk = next;
            it->next = *active;
            *active = it;
            assert(it->alloc.t == AREG);
            assert(rstest(freeregs, it->alloc.a));
            rsclr(&freeregs, it->alloc.a);
            DBG(" << %%%zd reblock %s\n", interval2temp(it), mctarg->rnames[it->alloc.a]);
         } else lnk = &it->next;
      }

      /** find a register for current **/

      int this = interval2temp(current);
      regset avail = freeregs & (current->fpr ? fpregset : gpregset),
             fixexcl = 0, excl = 0;
      Instr *ins = &instrtab[this];
      int reg = 0;

      /* exclude regs from overlapping fixed intervals */
      int end = intervalend(current);
      for (FixInterval *last = NULL, *fxit = ra->fixed; fxit;
           last = fxit, fxit = fxit->next) {
         if (last) assert(last->range.from <= fxit->range.from && "unsorted fixintervals");
         if (fxit->range.to <= pos) {
            ra->fixed = fxit->next;
            continue;
         } else if (fxit->range.from >= end) {
            break;
         }

         for (int i = 0; i < current->nrange; ++i) {
            if (rangeoverlap(fxit->range, itrange(current, i))) {
               fixexcl |= fxit->rs;
            }
         }
      }
      /* exclude regs from overlapping inactive intervals */
      for (Interval *it = *inactive; it; it = it->next) {
         if (it->alloc.t == AREG && intersoverlap(it, current)) {
            rsset(&excl, it->alloc.a);
         }
      }
      /* for 2-address instrs, exclude reg from 2nd arg (unless arg#1 == arg#2) */
      if (ins->inplace && opnarg[ins->op] == 2) {
         int xreg;
         if (ins->r.t == RREG) rsset(&excl, ins->r.i);
         else if (ins->r.t == RTMP && (xreg = instrtab[ins->r.i].reg)) {
            if (ins->r.bits != ins->l.bits)
               rsset(&fixexcl, xreg-1);
         }
      }
      excl |= fixexcl;
      avail &= ~excl;

      if (!avail) { /* no regs left, must spill */
         Interval **ptospill = NULL, *tospill = current;
         /* heuristic: look for longest-lived active interval with lower spill cost */
         int curend = intervalend(current);
         for (lnk = active, it = *lnk; (next = it ? it->next : 0), it; it = next) {
            int end = intervalend(it);
            if (it->cost < tospill->cost && end > curend && !rstest(fixexcl, it->alloc.a)) {
               ptospill = lnk;
               tospill = it;
               reg = tospill->alloc.a;
            }
            lnk = &it->next;
         }

         /* insert in spilled, keep sorted */
         if (ptospill) {
            *ptospill = tospill->next; /* remove from active */
            int from = intervalbeg(tospill);
            lnk = &spilled;
            /* XXX potentially slow linear search */
            while (*lnk && intervalbeg(*lnk) < from)
               lnk = &(*lnk)->next;
            tospill->next = *lnk;
            *lnk = tospill;
         } else { /* tospill == current, so we can just append and keep it sorted */
            *spilled_tail = tospill;
            tospill->next = NULL;
         }
         if (!tospill->next) /* update spilled list tail */
            spilled_tail = &tospill->next;

         assert(spilled != NULL);
         if (tospill == current) {
            DBG("spilled %%%d\n", this);
            continue;
         } else {
            instrtab[interval2temp(tospill)].reg = 0;
            DBG("%%%d takes %s from %%%d (spilled)\n", this, mctarg->rnames[reg],
                  interval2temp(tospill));
            goto GotReg;
         }
      }

      /* have free regs, try to use hint */
      if (current->rhint >= 0)
         DBG("have hint %s for %%%zd\n",
               mctarg->rnames[current->rhint], interval2temp(current));
      if (current->rhint >= 0 && rstest(avail, current->rhint)) {
         DBG(" (used hint)\n");
         reg = current->rhint;
         goto GotReg;
      } else {
         /* for two-address instructions, try to use the reg of left arg */
         if (ins->op != Ophi && (opnarg[ins->op] == 1 || (opnarg[ins->op] == 2 && ins->inplace))) {
            DBG(" %%%d try %d,%d\n", this, ins->l.t,ins->l.i);
            if (ins->l.t == RREG && rstest(avail, reg = ins->l.i))
               goto GotReg;
            if (ins->l.t == RTMP)
            if ((reg = instrtab[ins->l.i].reg-1) >= 0)
            if (rstest(avail, reg))
               goto GotReg;
         /* for phi, try to use reg of any arg */
         } else if (ins->op == Ophi) {
            Ref *arg = phitab.p[ins->l.i];
            for (int i = 0; i < xbcap(arg); ++i) {
               if (arg->t == RREG && rstest(avail, reg = arg->i)) goto GotReg;
               if (arg->t == RTMP)
               if ((reg = instrtab[arg->i].reg-1) >= 0)
               if (rstest(avail, reg))
                  goto GotReg;
            }
         }

         /* no hints to use */
         if (avail &~ mctarg->rcallee) /* prefer caller-saved regs */
            avail &=~ mctarg->rcallee;
         /* and pick first available reg */
         reg = lowestsetbit(avail);
      }
   GotReg:
      current->alloc = areg(reg);
      ins->reg = reg + 1;
      DBG("%%%d got %s\n", this, mctarg->rnames[reg]);
      rsclr(&freeregs, reg);
      rsset(&ra->fn->regusage, reg);

      /* add current to active */
      current->next = *active;
      *active = current;
   }

   if (ccopt.dbg.r) {
      DBG("regusage: ");
      for (int r = 0; r < MAXREGS; ++r) {
         if (rstest(ra->fn->regusage, r)) DBG(" %s", mctarg->rnames[r]);
      }
      DBG("\n");
   }
   /* allocate stack slots for spilled intervals
    * this is like another (simplified) linear scan pass */
   Interval *active = NULL;
   int prevpos = -1;
   if (spilled) DBG("spilled:\n");
   for (Interval *current = spilled, *next; current; current = next) {
      int pos = intervalbeg(current);
      DBG("  %%%zd: [%d,%d)\n", interval2temp(current), pos, intervalend(current));
      assert(pos >= prevpos && "unsorted spilled?");
      prevpos = pos;
      /* Expire old intervals */
      Interval **lnk, *it, *lnext;
      for (lnk = &active, it = *lnk; (lnext = it ? it->next : 0), it; it = lnext) {
         /* ends before position? */
         if (intervalend(it) <= pos) {
            /* move from active to handled */
            *lnk = lnext;
            freestk(ra, it->alloc.a);
         } else lnk = &it->next;
      }
      /* allocate a stack slot for current and move to active */
      current->alloc = allocstk(ra);
      DBG("    got stk%d\n", current->alloc.a);
      next = current->next;
      current->next = active;
      active = current;
   }
}

static bool
isstoreimm(Ref r)
{
   if (r.t == RTMP) return 1; /* register OK */
   if (isintcon(r)) switch (target.arch) {
   case ISxxx: assert(0);
   /* TODO don't hard code this architecture dependent dispatch */
   case ISx86_64: return concls(r) == KI32; /* x86: MOV [addr], imm32 */
   case ISaarch64: return r.i == 0; /* arm doesn't have STR <imm>, but has zero register */
   }
   return 0;
}

/* replace temps with physical regs, add loads & stores for spilled temps */
static bool
devirt(RegAlloc *ra, Block *blk)
{
   bool allnops = 1;
   Function *fn = ra->fn;
   Alloc spillsave[4] = {0};
   memset(ra->freestk, 0, BSSIZE(MAXSPILL) * sizeof *ra->freestk);

   for (int curi = 0; curi < blk->ins.n; ++curi) {
      int temp = blk->ins.p[curi];
      Instr *ins = &instrtab[temp];
      Interval *it;
      Alloc *alloc;
      IRAddr newaddr;
      Ref *argref[4];
      int curi0;
      int naddr = 0;
      int nargref = 0;
      int nspill = 0;

      /** devirtualize operands **/
      for (int i = 0; i < 2; ++i) {
         Ref *r = &i[&ins->l];
         if (r->t == RADDR) {
            IRAddr *a = &addrtab.p[r->i];
            ++naddr;
            newaddr = *a;
            argref[nargref++] = &newaddr.base;
            argref[nargref++] = &newaddr.index;
         } else {
            argref[nargref++] = r;
         }
      }
      for (int i = 0; i < nargref; ++i) {
         Ref *r = argref[i];
         int tr;
         if (r->t == RTMP && (it = &ra->inter[r->i])->nrange > 0) {
            alloc = &it->alloc;
            if (alloc->t == ASTACK && ins->op == Omove && kisint(ins->cls) == kisint(instrtab[r->i].cls)) {
               /* move [reg], [stk] -> [reg] = load [stk] */
               assert(r == &ins->r && ins->l.t == RREG);
               ins->reg = ins->l.i+1;
               ins->op = cls2load[instrtab[r->i].cls];
               ins->l = stkslotref(fn, alloc->a*8);
               ins->r = NOREF;
            } else if (alloc->t == ASTACK && ins->op == Ocopy && r == &ins->l && ins->reg && kisint(ins->cls) == kisint(instrtab[r->i].cls)) {
               /* [reg] = copy [stk] -> [reg] = load [stk] */
               ins->op = cls2load[instrtab[r->i].cls];
               ins->l = stkslotref(fn, alloc->a*8);
            } else if (alloc->t == ASTACK) {
               /* ref was spilled, gen load to scratch register and use it */
               Instr ld = {.cls = insrescls(instrtab[r->i])};
               int reg = kisint(ld.cls) ? mctarg->gprscratch : mctarg->fprscratch;
               bool dosave = 0;
               /* pick scratch register, or any register that doesn't conflict with this instr's srcs/dst */
               if (nspill > 0) {
                  regset avail = (kisflt(ld.cls) ? fpregset : gpregset) &~ mctarg->rglob;
                  if (ins->reg) rsclr(&avail, ins->reg-1);
                  for (int j = 0; j < nargref; ++j) {
                     Interval *it;
                     if (argref[j]->t == RREG) rsclr(&avail, argref[j]->i);
                     else if (argref[j]->t == RTMP) {
                        it = &ra->inter[argref[j]->i];
                        if (it->alloc.t == AREG) rsclr(&avail, it->alloc.a);
                     }
                  }
                  assert(avail != 0);
                  if (avail &~ (fn->regusage | mctarg->rcallee)) avail &= ~(fn->regusage | mctarg->rcallee);
                  reg = lowestsetbit(avail);
                  /* if not the designated scratch register, we need to save+restore */
                  if (rstest(fn->regusage, reg) || rstest(mctarg->rcallee, reg)) {
                     dosave = 1;
                     if (!spillsave[nspill-1].t) spillsave[nspill-1] = allocstk(ra);
                     emitmove(fn, isgpr(reg) ? KPTR : KF64, spillsave[nspill-1], areg(reg), blk, curi++);
                  }
               }
               ld.reg = reg+1;
               ld.op = cls2load[ld.cls];
               ld.l = stkslotref(fn, alloc->a*8);
               insertinstr(blk, curi++, ld);
               *r = mkref(RREG, reg);
               if (nspill > 0 && dosave) {
                  emitmove(fn, isgpr(reg) ? KPTR : KF64, areg(reg), spillsave[nspill-1], blk, curi+1);
               }
               ++nspill;
            } else if ((tr = instrtab[r->i].reg)) {
               assert(alloc && alloc->t == AREG && alloc->a == tr-1);
               *r = mkref(RREG, tr-1);
            }
         }
      }
      if (nspill > 1) assert(ins->op != Ocall);
      if (naddr) {
         Ref *r = ins->l.t == RADDR ? &ins->l : &ins->r;
         *r = mkaddr(newaddr);
      }

      /* devirtualize destination */
      curi0 = curi;
      alloc = temp < ra->ninter && (it = &ra->inter[temp]) && it->nrange ? &it->alloc : NULL;
      if (alloc && alloc->t == ASTACK) {
         enum irclass cls = insrescls(*ins);
         int store = cls2store[cls];
         /* t was spilled, gen store */
         if (ins->op == Ocopy && (ins->l.t == RREG || isstoreimm(ins->l))) {
            ins->op = store;
            ins->r = ins->l;
            ins->l = stkslotref(fn, alloc->a*8);
         } else {
            bool dosave = 0;
            int reg = kisint(insrescls(*ins)) ? mctarg->gprscratch : mctarg->fprscratch;
            if (nspill > 0) {
               regset avail = (kisflt(cls) ? fpregset : gpregset) &~ mctarg->rglob;
               for (int j = 0; j < nargref; ++j) {
                  if (argref[j]->t == RREG) rsclr(&avail, argref[j]->i);
               }
               assert(avail != 0);
               if (avail &~ (fn->regusage | mctarg->rcallee)) avail &= ~(fn->regusage | mctarg->rcallee);
               /* if not the designated scratch register, we need to save+restore */
               reg = lowestsetbit(avail);
               if (rstest(fn->regusage, reg) || rstest(mctarg->rcallee, reg)) {
                  dosave = 1;
                  if (!spillsave[nspill-1].t) spillsave[nspill-1] = allocstk(ra);
                  emitmove(fn, isgpr(reg) ? KPTR : KF64, spillsave[nspill-1], areg(reg), blk, curi++);
                  curi0 = curi;
               }
            }
            ins->reg = reg+1;
            insertinstr(blk, ++curi,
                  mkinstr(store, 0, stkslotref(fn, alloc->a*8), mkref(RREG, reg)));
            if (nspill > 0 && dosave) {
               emitmove(fn, isgpr(reg) ? KPTR : KF64, areg(reg),
                     spillsave[nspill-1], blk, ++curi);
            }
         }
      }
      if (!ins->reg && insrescls(*ins) && ins->op != Omove && !ins->keep && !oisstore(ins->op)) {
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
      if (!ins->reg && in_range(ins->op, Oloads8, Oloadf64)) {
         assert(ins->keep);
         ins->reg = kisint(ins->cls) ? mctarg->gprscratch+1 : mctarg->fprscratch+1;
      }
   }

   if (allnops) vfree(&blk->ins);
   return allnops;
}

static void
fini(RegAlloc *ra)
{
   int id = 0;
   Function *fn = ra->fn;
   Block *blk = fn->entry;

   do {
      blk->id = id++;
      bool allnops = devirt(ra, blk);
      if (allnops && !blk->s2 && blk->npred > 0) { /* remove no-op blocks */
         bool delet = 1;
         for (int i = 0; i < blk->npred; ++i) {
            Block *p = blkpred(blk, i);
            if (p == blk || (p->s2 && !blk->s1))
               delet = 0;
         }
         for (int i = 0; i < blk->npred; ++i) {
            Block *p = blkpred(blk, i);
            if (!p->s2 && !blk->s1) {
               /* simplify:
                *
                * @p:
                *   ...
                *   b @blk
                * @blk:
                *   NOP
                *   ret/trap
                */
               assert(p->s1 == blk);
               p->jmp.t = blk->jmp.t;
               p->s1 = NULL;
            } else if (blk->s1 && blk->s1 != blk) {
               /* simplify:
                *
                * @p:
                *   ...
                *   b %x, @blk, @other
                * @blk:
                *   NOP
                *   b @next
                */
               Block *next = blk->s1;
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
      } else if (allnops) {
         vfree(&blk->ins);
      }
   } while ((blk = blk->lnext) != fn->entry);
}

void
regalloc(Function *fn)
{
   RegAlloc ra = {fn, .arena = fn->passarena};
   Block *blk, *last;

   /* setup */
   if (!fpregset || !gpregset) {
      for (int r = 0; r < MAXREGS; ++r) {
         if (isfpr(r))
            rsset(&fpregset, r);
         else if (isgpr(r))
            rsset(&gpregset, r);
      }
   }
   fn->regusage = 0;
   fn->stksiz = alignup(fn->stksiz, 8);
   fn->isleaf = 1;

   /* put into reverse post order */
   sortrpo(fn);

   /* fix liveness ranges */
   fixlive(fn);

   /* transform into CSSA */
   fixcssa(fn);

   fillblkids(fn);
   fillloop(fn);

   if (ccopt.dbg.r) {
      bfmt(ccopt.dbgout, "<< Before linear scan >>\n");
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
   fn->stksiz += ra.maxstk*8;
   if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);

   for (Interval *it = ra.inter; ra.intercount > 0; ++it) {
      if (it->nrange > 2) xbfree(it->_rdyn);
      if (it->nrange > 0) --ra.intercount;
   }

   if (ccopt.dbg.r) {
      bfmt(ccopt.dbgout, "<< After regalloc >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
