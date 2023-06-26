#include "ir.h"

/* Implements reverse linear register allocation, quite ad-hoc right now */

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

struct rmap {
   regset free; /* free registers */
   regset blocked; /* registers allocated to themselves (from e.g. isel reg constraints, abi) */
   ushort regs[MAXREGS]; /* map reg -> temp holding reg */
   struct bitset freestk[BSSIZE(MAXSPILL)]; /* free stack slots */
   imap_of(struct alloc) allocs; /* map tmpidx -> allocation */
};

struct rega {
   struct function *fn;
   struct rmap m;
   int maxstk, stktop;
};

static vec_of(union ref *) stkslotrefs;

static union ref
addstkslotref(union ref inst)
{
   vpush(&stkslotrefs, &instrtab[inst.i].l);
   return inst;
}

static int allocreg(struct rega *ra, enum irclass cls, union ref ref, uvlong excl);

#if 0
#define DBG(...) if(ccopt.dbg.r) efmt(__VA_ARGS__)
static void
dumpallocs(const char *s, struct rmap *m)
{
   int var;
   struct alloc *alloc;

   DBG("%s map [", s);
   imap_each(&m->allocs, var, alloc) {
      if (!alloc->t) continue;
      DBG(" %%%d -> %c%d,", var, "XRS"[alloc->t], alloc->a);
   }
   DBG("]\n");
}
#else
#define DBG(...) ((void)0)
#define dumpallocs(...)
#endif

static void
freereg(struct rega *ra, int r)
{
   ra->m.free = rsset(ra->m.free, r);
   ra->m.blocked = rsclr(ra->m.blocked, r);
}

static void take(struct rega *ra, int reg, union ref ref);

static int
allocstk(struct rega *ra, int var)
{
   int s = -1;

   for (int i = 0; i < BSSIZE(MAXSPILL); ++i) {
      if (ra->m.freestk[i].u == 0) continue;
      s = i*64 + lowestsetbit(ra->m.freestk[i].u);
   }
   if (s != -1) {
      bsclr(ra->m.freestk, s);
      if (ra->stktop < s) ra->stktop = s+1;
   } else {
      s = ra->stktop++;
   }
   if (ra->maxstk < s+1) ra->maxstk = s+1;
   (void)imap_set(&ra->m.allocs, var, astack(s));
   return s;
}

static void
freestk(struct rega *ra, int slot)
{
   DBG("FREE stk %d\n",slot);
   if (slot < MAXSPILL)
      bsset(ra->m.freestk, slot);
   else if (slot == ra->stktop - 1)
      --ra->stktop;
}

static void
def(struct rega *ra, struct instr *ins, struct block *blk, int curi)
{
   int reg = -1, var;
   struct alloc *alloc;
   if (ins->op != Omove && ins->op != Ocall) {
      var = ins - instrtab;
      DBG("def %%%d\n",var);
      alloc = imap_get(&ra->m.allocs, var);
      if (!alloc || alloc->t == ADEAD) {
         return;
      } else if (alloc->t == AREG) {
         reg = alloc->a;
         DBG("-- free %s for %%%d\n", mctarg->rnames[alloc->a], var);
         assert(!rstest(ra->m.free, reg));
         assert(ra->m.regs[reg] == var);
         ins->reg = reg+1;
      } else if (alloc->t == ASTACK && ins->op != Ophi) {
         /* unspill, insert 'store [slot], reg' */
         struct alloc astk = *alloc;
         struct instr store;
         int reg = -1;

         if ((ins->op == Ocopy || ins->inplace) && ins->l.t == RREG) {
            int hint = ins->l.i;
            if (rstest(ra->m.free, hint) && kisflt(insrescls(*ins)) == isfpr(hint)) {
               take(ra, reg = hint, mkref(RTMP, var));
               assert(ra->m.regs[reg] == var && !rstest(ra->m.free, hint));
            }
         }
         if (reg < 0)
            reg = allocreg(ra, insrescls(*ins), mkref(RTMP, var), 0);
         store = mkinstr(Ostore1 + ilog2(cls2siz[insrescls(*ins)]), 0,
                         mkref(RICON, astk.a*8), mkref(RREG, reg));
         DBG("-- unspill %%%d s%d -> %s\n", var, astk.a, mctarg->rnames[reg]);
         addstkslotref(insertinstr(blk, ++curi, store));
         freestk(ra, astk.a);
         ins->reg = reg+1;
         def(ra, ins, blk, curi); /* and free the reg again */
         return;
      }
      if (alloc && (alloc->t != ASTACK || ins->op != Ophi)) *alloc = afree();
   } else if (ins->op == Omove) {
      assert(ins->l.t == RREG); /* move to a register */
      assert(rstest(ra->m.blocked, ins->l.i));
      reg = ins->l.i;
      DBG("-- free %s\n", mctarg->rnames[ins->l.i]);
   } else {
      struct call *call = &calltab.p[ins->r.i];
      for (int i = 0; i < 2; ++i) {
         if (!call->abiret[i].ty.cls) break;
         freereg(ra, call->abiret[i].reg);
      }
      return;
   }
   if (reg != -1) freereg(ra, reg);
}

static void
take(struct rega *ra, int reg, union ref ref) {
   DBG("-- take %s for %c%d\n", mctarg->rnames[reg], "R%"[ref.t==RTMP], ref.i);
   assert(rstest(ra->m.free, reg) && "taken");
   if (ref.t == RTMP) {
      (void)imap_set(&ra->m.allocs, ref.i, areg(reg));
      ra->m.regs[reg] = ref.i;
   } else {
      ra->m.blocked = rsset(ra->m.blocked, reg);
   }
   ra->m.free = rsclr(ra->m.free, reg);
   globusage = rsset(globusage, reg);
}

static int
allocreg(struct rega *ra, enum irclass cls, union ref ref, uvlong excl)
{
   regset rs;
   int reg;

   assert(cls);
   if (kisint(cls)) {
      rs = gpregset;
   } else if (kisflt(cls)) {
      rs = fpregset;
   } else assert(0);
   rs = rsminus(rs, mctarg->rglob);
   rs = rsand(rs, ra->m.free);
   rs = rsminus(rs, excl);

   assert(rs != 0 && "no more reg");
   if (rsminus(rs, mctarg->rcallee) != 0)
      reg = lowestsetbit(rsminus(rs, mctarg->rcallee));
   else
      reg = lowestsetbit(rs);
   take(ra, reg, ref);
   return reg;
}

static void
spill(struct rega *ra, int reg, struct block *blk, int curi) {
   int var, s;
   struct instr load;
   struct alloc *alloc;

   if (rstest(ra->m.free, reg)) return;
   var = ra->m.regs[reg];
   assert(!rstest(ra->m.blocked, reg));
   assert((alloc = imap_get(&ra->m.allocs, var)) && !memcmp(alloc, &areg(reg), sizeof *alloc));
   s = allocstk(ra, var);
   DBG("-- spill %%%d %s -> s%d\n", var, mctarg->rnames[reg], s);
   instrtab[var].reg = 0;
   /* insert 'reg = load [slot]' */
   load = mkinstr(Oloads1 + 2*ilog2(cls2siz[insrescls(instrtab[var])]),
                  insrescls(instrtab[var]), mkref(RICON, s*8));
   load.reg = reg+1;
   addstkslotref(insertinstr(blk, ++curi, load));
   freereg(ra, reg);
}

#define mkmove(k, rd, rs) mkinstr(Omove, k, mkref(RREG, rd), mkref(RREG, rs))

static void
forcetake(struct rega *ra, int reg, union ref ref, struct block *blk, int curi) {
   int var;
   struct alloc *alloc;

   if (ref.t == RREG && rstest(ra->m.blocked, ref.i)) {
      assert(!rstest(ra->m.free, reg));
      return;
   }
   if (ref.t == RTMP && !rstest(ra->m.free, reg) && ra->m.regs[reg] == ref.i) return;
   if (rstest(ra->m.free, reg)) {
      take(ra, reg, ref);
      return;
   }
   assert(!rstest(ra->m.blocked, ref.i));
   var = ra->m.regs[reg];
   alloc = imap_get(&ra->m.allocs, var);
   assert(alloc && alloc->a == reg);
   *alloc = afree();
   /* try to move temp to another register */
   if (rsand(ra->m.free, isgpr(reg) ? gpregset : fpregset) != 0) {
      /* the register of the current instruction (if any) was already free'd (by def), so
       * we need to explictly exclude it from the pool of rename registers
       * e.g.: given 'R0 = copy R1'; if R1 => %x, we need to prevent renaming %x => R0
       */
      regset excl = instrtab[blk->ins.p[curi]].reg;
      int rename = allocreg(ra, isgpr(reg) ? KI4 : KF4, mkref(RTMP, ra->m.regs[reg]),
                            excl ? rsset(0, excl-1) : 0);
      if (ccopt.dbg.r)DBG("-- rename %%%d %s -> %s\n", var, mctarg->rnames[reg], mctarg->rnames[rename]);
      /* introduce move from rename -> original (since we allocate backwards) */
      insertinstr(blk, ++curi, mkmove(insrescls(instrtab[var]), reg, rename));
      ra->m.regs[rename] = var;
      globusage = rsset(globusage, rename);
      (void)imap_set(&ra->m.allocs, var, areg(rename));
      ra->m.free = rsset(ra->m.free, reg);
   } else {
      spill(ra, reg, blk, curi);
      ra->m.free = rsset(ra->m.free, reg);
   }
   take(ra, reg, ref);
}

/* mark a use for *ref, possibly allocating a register for it, considering it won't clash with `other` */
static void
use(struct rega *ra, struct block *blk, int curi, enum op op, int hint, union ref *ref, union ref other)
{
   struct instr *ins;
   uvlong excl = other.t == RREG ? 1ull<<other.i : 0;
   struct alloc *alloc;
   int reg;

   if (ref->t == RADDR) {
      struct addr addr = addrht[ref->i];
      if (addr.base.bits) use(ra, blk, curi, 0, hint, &addr.base, addr.index);
      if (addr.index.bits) use(ra, blk, curi, 0, hint, &addr.index, NOREF);
      *ref = mkaddr(addr);
      return;
   } else if (ref->t == RREG) {
      int reg = ref->i;
      assert(hint<0 || !rstest(excl, hint));
      if (op == -Jret) {
         /* since return marks an exit block, if any temporary is occuping a
          * return register we mark it as dead since it cannot be live after
          * the return, so can't rename it like forcetake would try to do */
         if (!rstest(ra->m.free, reg)) {
            (void)imap_set(&ra->m.allocs, ra->m.regs[reg], afree());
            ra->m.free = rsset(ra->m.free, reg);
         }
      }
      forcetake(ra, reg, *ref, blk, curi);
      return;
   }
   if (ref->t != RTMP) return;

   ins = &instrtab[ref->i];
   if (!ins->cls) return;
   if (!(alloc = imap_get(&ra->m.allocs, ref->i)) || alloc->t != AREG) {
      int s = -1;
      if (alloc && alloc->t == ASTACK) {
         /* ensure spill isn't overwritten by dest
          * e.g. in R0 = add %s, 7 => can't spill %s to R0 */
         struct instr *use = &instrtab[blk->ins.p[curi]];
         s = alloc->a;
         if (use->reg) excl = rsset(excl, use->reg-1);
      }

      assert(ins->op != Ocall);
      if (ins->r.t == RREG && ins->inplace) excl |= 1ull<<ins->r.i;
      if ((hint == -1 || !rstest(ra->m.free, hint)) && ins->op == Ocopy && ins->l.t == RREG)
         /* for '%x = copy Rx', hint %x to use Rx */
         hint = ins->l.i;

      if (hint != -1 && !rstest(excl, hint) && rstest(ra->m.free, hint)
       && kisflt(insrescls(*ins)) == isfpr(hint)) {
         take(ra, hint, *ref);
         reg = hint;
      } else {
         reg = allocreg(ra, insrescls(*ins), *ref, excl);
      }

      if (s >= 0) {
         /* unspill, insert 'store [slot], reg' */
         DBG("-- unspill %%%d s%d -> %s\n", ref->i, s, mctarg->rnames[reg]);
         struct instr store = mkinstr(Ostore1 + ilog2(cls2siz[insrescls(*ins)]), 0,
                                      mkref(RICON, s*8),
                                      mkref(RREG, reg));
         addstkslotref(insertinstr(blk, ++curi, store));
         freestk(ra, s);
      }
   } else {
      reg = alloc->a;
   }
   /* do not patch ref if it's used in a phi
    * or if it's cond branch arg, emit() wants to know what instr it is */
   if (op != Ophi)
   if (ref != blk->jmp.arg || blk->jmp.t != Jb)
      *ref = mkref(RREG, reg);
}

static void
doins(struct rega *ra, struct block *blk, struct instr *ins, int curi)
{
   int hint0 = -1, hint1 = -1;
   if (ins->op != Ocopy && !imap_get(&ra->m.allocs, ins - instrtab) && ins->skip) { /* unused */
      *ins = mkinstr(Onop, 0,);
      return;
   }
   def(ra, ins, blk, curi);
   if (ins->op != Ocall) {
      if (ins->op == Ocopy || ins->inplace) hint0 = ins->reg - 1;
      if (ins->op == Omove) {
         if (ins->l.t == RREG) hint1 = ins->l.i;
         /* MOV Rx,Rx is used by isel to indicate a clobber,
          * so it should be a def point for Rx but not a use point */
         if (ins->r.bits != ins->l.bits)
            use(ra, blk, curi, ins->op, hint1, &ins->r, NOREF);
      } else {
         if (ins->l.bits) use(ra, blk, curi, ins->op, hint0, &ins->l, ins->r);
         if (ins->r.bits) use(ra, blk, curi, ins->op, hint1, &ins->r, NOREF);
      }
   } else {
      struct call *call = &calltab.p[ins->r.i];
      regset rspill = rsminus(rsunion(gpregset, fpregset), rsunion(mctarg->rglob, mctarg->rcallee));

      dumpallocs("CALL", &ra->m);
      if (call->abiret[0].ty.bits) rspill = rsclr(rspill, call->abiret[0].reg);
      if (call->abiret[1].ty.bits) rspill = rsclr(rspill, call->abiret[1].reg);
      for (int r = 0; rsiter(&r, rspill); ++r) {
         spill(ra, r, blk, curi);
      }
      for (int j = 0; j < call->narg; ++j) {
         short reg = call->abiarg[j].reg;
         if (reg >= 0) {
            forcetake(ra, reg, mkref(RREG, reg), blk, curi);
         }
      }

      use(ra, blk, curi, ins->op, hint0, &ins->l, NOREF);
   }
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
   assert(npmove < MAXREGS);
   pmove[npmove++] = (struct pmove) { k, PMTOMOVE, dst, src };
}

static void
emitmove(enum irclass k, struct alloc dst, struct alloc src, struct block *blk, int curi)
{
   struct instr mv;
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
      mv.l = mkref(RICON, dst.a*8);
      mv.reg = src.a;
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

   for (j = 0; j < npmove; ++j)
      if (!memcmp(&pmove[j].dst, &pmove[i].src, sizeof pmove->dst)) {
         break;
      }
   if (j == npmove) goto Done;
   switch (pmove[j].stat) {
   default: assert(0);
   case PMMOVING:
      c = j;
   Swap:
      assert(pmove[i].src.t == AREG && pmove[i].dst.t == AREG);
      insertinstr(blk, curi,
            mkinstr(Oswap, *k, mkref(RREG, pmove[i].dst.a), mkref(RREG, pmove[i].src.a)));
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
   for (int i = 0; i < npmove; ++i)
      if (pmove[i].stat == PMTOMOVE) {
         pmrec(i, blk, curi, &(enum irclass) { pmove[i].k });
      }
}

struct bbrm { struct rmap in, out; };

static void
lowerphis(struct function *fn, struct bbrm *bbrm, struct block *blk, struct block *suc)
{
   int predno;
   struct alloc *alloc;
   struct rmap *out = &bbrm[blk->id].out;
   struct rmap *in = &bbrm[suc->id].in;
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
         alloc = imap_get(&out->allocs, arg->i);
         assert(alloc && alloc->t != ADEAD);
         from = *alloc;
         DBG("  found %c%d\n", " RS"[from.t], from.a);
         if (from.t == AREG)
            instrtab[arg->i].reg = from.a+1;
      }
      if (phi->reg) {
         to = areg(phi->reg - 1);
         DBG("  phi had R%d\n", to.a);
      } else {
         alloc = imap_get(&in->allocs, phi - instrtab);
         assert(alloc && alloc->t != ADEAD);
         DBG("  found phi %c%d\n", " RS"[from.t], from.a);
         to = *alloc;
         if (to.t == AREG)
            phi->reg = to.a+1;
      }
      DBG(" > phi move %c%d -> %c%d\n", " RS"[from.t], from.a, " RS"[to.t], to.a);
      if (!n) n = insertblk(fn, blk, suc);
      pmadd(phi->cls, to, from);
   }
   if (n) emitpm(n);
}

static void
resolve(struct function *fn, struct bbrm *bbrm, struct block *blk, struct block *suc)
{
   ushort var;
   struct alloc *alloc, *alloc2;
   struct rmap *out = &bbrm[blk->id].out, *in = &bbrm[suc->id].in;
   struct block *n = NULL;

   DBG("resolve(@%d, @%d)\n",blk->id, suc->id);

   npmove = 0;
   if (!blk->s2) n = blk;
   imap_each(&in->allocs, var, alloc) {
      if (alloc->t == ADEAD) continue;
      if ((alloc2 = imap_get(&out->allocs, var)) && alloc2->t != ADEAD
       && memcmp(alloc2, alloc, sizeof *alloc) != 0) {
         DBG("resolve @%d, @%d, %%%d\n", blk->id, suc->id, var);
         DBG(" > move %c%d -> %c%d\n", " RS"[alloc2->t], alloc2->a, " RS"[alloc->t], alloc->a);
         if (!n) {
            n = insertblk(fn, blk, suc);
            n->id = blk->id; /* use same bbrm */
         }
         pmadd(insrescls(instrtab[var]), *alloc, *alloc2);
      }
      if (!instrtab[var].reg && alloc->t == AREG)
         instrtab[var].reg = alloc->a + 1;
   }
   if (n) emitpm(n);

   dumpallocs("in", in);
   dumpallocs("out", out);
}

static void fixlive(struct function *fn);

void
regalloc(struct function *fn)
{
   int id;
   struct block *last = fn->entry->lprev, *blk;
   static union ref *stkslotrefsbuf[64];
   struct bbrm *bbrm;
   int nbbrm;
   struct rega ra = {fn};

   /* setup */
   if (!fpregset || !gpregset) {
      for (int i = 0; i < MAXREGS; ++i) {
         if (isfpr(i))
            fpregset = rsset(fpregset, i);
         else if (isgpr(i))
            gpregset = rsset(gpregset, i);
      }
   }
   ra.m.blocked = mctarg->rglob;
   ra.m.free = rsminus(rsunion(gpregset, fpregset), ra.m.blocked);
   memset(ra.m.freestk, 0xFF, sizeof ra.m.freestk);
   globusage = 0;
   fn->stksiz = alignup(fn->stksiz, 8);
   fn->isleaf = 1;
   vinit(&stkslotrefs, stkslotrefsbuf, arraylength(stkslotrefsbuf));

   /* fix liveness ranges */
   fixlive(fn);

   /* generate copies for phi operands to transform into CSSA */
   blk = fn->entry;
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
   fillblkids(fn);
   bbrm = xcalloc(sizeof *bbrm * (nbbrm = fn->nblk));

   /* visit blocks in reverse, allocating registers in a greedy manner */
   blk = last;
   do {
      int curi;
      DBG("do @%d\n", blk->id);
      memcpy(bbrm[blk->id].out.regs, ra.m.regs, sizeof ra.m.regs);
      imap_copy(&bbrm[blk->id].out.allocs, &ra.m.allocs);
      dumpallocs("out", &ra.m);

      if (blk->s1 && blk->s1->phi.n) {
         /* if the block is a predecessor to some phis, introduce use points
          * for the corresponding arguments to each phi */
         struct block *s = blk->s1;
         int predno = 0;
         for (; predno < s->npred; ++predno)
            if (blkpred(s, predno) == blk)
               break;
         assert(predno < s->npred);
         for (int i = s->phi.n - 1; i >= 0; --i) {
            struct instr *phi = &instrtab[s->phi.p[i]];
            union ref *arg = &phitab.p[phi->l.i][predno];;
            use(&ra, blk, blk->ins.n, Ophi, phi->reg-1, arg, NOREF);
         }
      }

      curi = blk->ins.n - 1;
      for (int i = 0; i < 2; ++i) {
         if (!blk->jmp.arg[i].bits) break;
         /* do not allocate a reg for a cmp op used a branch argument, since it's a pseudo op */
         if (blk->jmp.t == Jb && blk->jmp.arg[i].t == RTMP && oiscmp(instrtab[blk->jmp.arg[i].i].op))
            break;
         use(&ra, blk, curi, -blk->jmp.t,
                  blk->jmp.t == Jret ? fn->abiret[i].reg : -1,
                  &blk->jmp.arg[i], blk->jmp.arg[!i]);
      }

      for (; curi >= 0; --curi) {
         struct instr *ins = &instrtab[blk->ins.p[curi]];
         if (ins->op == Ocall) fn->isleaf = 0;
         doins(&ra, blk, ins, curi);
      }

      for (int i = blk->phi.n - 1; i >= 0; --i) {
         struct instr *phi = &instrtab[blk->phi.p[i]];
         def(&ra, phi, blk, -1);
      }

      memcpy(bbrm[blk->id].in.regs, ra.m.regs, sizeof ra.m.regs);
      imap_copy(&bbrm[blk->id].in.allocs, &ra.m.allocs);;
   } while ((blk = blk->lprev) != last);

   if (1&&ccopt.dbg.r) {
      efmt("<< regalloc before resolve\n");
      irdump(fn);
   }

   /* resolve allocation mismatches across each edge */
   blk = last = fn->entry->lprev;
   do {
      if (blk->id < 0) continue;
      if (blk->s1) resolve(fn, bbrm, blk, blk->s1);
      if (blk->s2) resolve(fn, bbrm, blk, blk->s2);
   } while ((blk = blk->lprev) != last);

   /* parallel copies for each phi */
   blk = last = fn->entry->lprev;
   do {
      if (blk->id < 0) continue;
      for (int i = 0; i < blk->npred; ++i) {
         lowerphis(fn, bbrm, blkpred(blk, i), blk);
      }
      vfree(&blk->phi);
   } while ((blk = blk->lprev) != last);

   /* final cleanup */
   id = 0;
   blk = fn->entry;
   do {
      bool allnops = 1;
      blk->id = id++;
      for (int i = 0; i < blk->ins.n; ++i) {
         struct instr *ins = &instrtab[blk->ins.p[i]];
         if (!ins->reg && insrescls(*ins) && ins->op != Omove && !ins->keep) {
         /* dead */
         Nop:
           *ins = mkinstr(Onop,0,);
         } else if (ins->op == Omove && ins->r.t == RREG && ins->l.i == ins->r.i) {
            goto Nop;
         } else if (ins->op == Ocopy && ins->l.t == RREG && ins->reg-1 == ins->l.i) {
            goto Nop;
         } else if (ins->inplace && ins->l.t == RREG && ins->reg-1 != ins->l.i) {
            /* fixup in-place (two-address) instructions */
            allnops = 0;
            insertinstr(blk, i++, mkmove(ins->cls, ins->reg-1, ins->l.i));
            ins->l.i = ins->reg-1;
         } else if (ins->op != Onop) allnops = 0;
      }

      /* remove no-op blocks with only 1 pred + 1 succ OR 1 pred w/ 1 succ + 0 succ*/
      if (allnops && blk->npred == 1 && !blk->s2) {
         struct block *p = blkpred(blk, 0);
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
         DelBlk:
            vfree(&blk->phi);
            vfree(&blk->ins);
            blk->lnext->lprev = blk->lprev;
            blk->lprev->lnext = blk->lnext;
            --id;
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
            goto DelBlk;
         }
      }
   } while ((blk = blk->lnext) != fn->entry);
   fn->nblk = id;

   fn->stksiz += ra.maxstk*8;
   if (fn->stksiz > 1<<24) error(NULL, "'%s' stack frame too big", fn->name);
   while (stkslotrefs.n) {
      union ref *adr = stkslotrefs.p[--stkslotrefs.n];
      *adr = mkaddr((struct addr) { .base = mkref(RREG, mctarg->bpr), .disp = -fn->stksiz + adr->i });
   }
   if (ccopt.dbg.r) {
      efmt("<< After regalloc >>\n");
      irdump(fn);
   }
   for (int i = 0; i < nbbrm; ++i) {
      imap_free(&bbrm[i].in.allocs);
      imap_free(&bbrm[i].out.allocs);
   }
   imap_free(&ra.m.allocs);
   free(bbrm);
   fn->regusage = globusage;
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
      if (pphi->n) --npendingphi;
      for (int i = 0; i < pphi->n; ++i) {
         fillphi(defined, mkref(RTMP, pphi->p[i].phi), instrtab[pphi->p[i].phi].cls, pphi->p[i].var, blk);
      }
      vfree(pphi);
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.l) {
      efmt("<< After liveness fixup >>\n");
      irdump(fn);
   }
   if (defined != definedbuf) free(defined);
}

/* vim:set ts=3 sw=3 expandtab: */
