#include "ir.h"

static struct bitset globusage[1];
static struct bitset floatregs[1];

#define isfpr(reg) bstest(floatregs, (reg))
#define isgpr(reg) (!isfpr(reg))

enum { ADEAD, AREG, ASTACK } ;
struct alloc { ushort t : 2, a : 14; };
#define afree() ((struct alloc) { ADEAD })
#define areg(r) ((struct alloc) { AREG, (r) })
#define astack(s) ((struct alloc) { ASTACK, (s) })

struct rega {
   union ref regs[MAXREGS]; /* map reg -> value holding reg */
   struct alloc *allocs; /* map tmpidx -> allocation */
   vec_of(ushort) freestk; /* available stack slots */
   int nfreegpr, nfreefpr;
   int maxstk, stktop;
};

static vec_of(union ref *) stkslotrefs;

static void
addstkslotref(union ref inst)
{
   vpush(&stkslotrefs, &instrtab[inst.i].l);
}

static int allocreg(struct rega *ra, enum irclass cls, union ref ref, int excl);

#if 0
#define DBG efmt
#else
#define DBG(...) ((void)0)
#endif

static void
freereg(struct rega *ra, int r)
{
   ra->regs[r] = NOREF;
   if (isfpr(r)) ++ra->nfreefpr;
   else          ++ra->nfreegpr;
}

static void take(struct rega *ra, int reg, union ref ref);

static void
def(struct rega *ra, struct instr *ins, struct block *blk, int curi)
{
   int reg = -1, var;
   struct alloc *alloc;
   if (ins->op != Omove && ins->op != Ocall) {
      var = ins - instrtab;
      DBG("def %%%d\n",var);
      alloc = &ra->allocs[var];
      if (alloc->t == ADEAD) {
         return;
      } else if (alloc->t == AREG) {
         reg = alloc->a;
         DBG("-- free %s for %%%d\n", mctarg->rnames[alloc->a], var);
         assert(ra->regs[reg].bits == mkref(RTMP, var).bits);
      } else  if (alloc->t == ASTACK) {
         /* unspill, insert 'store [slot], reg' */
         struct alloc astk = *alloc;
         struct instr store;
         int reg = -1;

         if ((ins->op == Ocopy || ins->inplace) && ins->l.t == RREG) {
            int hint = ins->l.i;
            if (!ra->regs[hint].t) {
               take(ra, reg = hint, mkref(RTMP, var));
               assert(ra->regs[reg].bits == mkref(RTMP, var).bits);
            }
         }
         if (reg < 0)
            reg = allocreg(ra, insrescls(*ins), mkref(RTMP, var), -1);
         store = mkinstr(Ostore1 + ilog2(cls2siz[insrescls(*ins)]), 0,
                         mkref(RICON, astk.a*8), mkref(RREG, reg));
         DBG("-- unspill %%%d s%d -> %s\n", var, astk.a, mctarg->rnames[ins->reg+1]);
         addstkslotref(insertinstr(blk, ++curi, store));
         vpush(&ra->freestk, astk.a);
         ins->reg = reg+1;
         def(ra, ins, blk, curi); /* and free the reg again */
         return;
      }
      *alloc = afree();
   } else if (ins->op == Omove) {
      reg = ins->l.i;
      assert(ins->l.t == RREG);
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
   assert(!ra->regs[reg].t && "taken");
   if (ref.t == RTMP)
      ra->allocs[ref.i] = areg(reg);
   ra->regs[reg] = ref;
   assert(ra->regs[reg].bits);
   bsset(globusage, reg);
   if (isfpr(reg)) --ra->nfreefpr;
   else            --ra->nfreegpr;
}

static int
allocreg(struct rega *ra, enum irclass cls, union ref ref, int excl)
{
   int r0, rend, reg;

   assert(cls);
   if (kisint(cls)) {
      r0 = mctarg->gpr0;
      rend = mctarg->gpr0 + mctarg->ngpr;
   } else if (kisflt(cls)) {
      r0 = mctarg->fpr0;
      rend = mctarg->fpr0 + mctarg->nfpr;
   } else assert(0);
   for (reg = r0; reg < rend; ++reg) {
      if (bstest(mctarg->rglob, reg)) continue;
      if (reg != excl && !ra->regs[reg].t) {
         take(ra, reg, ref);
         return reg;
      }
   }
   assert(!"no more reg");
}

static int
allocstk(struct rega *ra, int var)
{
   int s;

   if (ra->freestk.n) {
      s = ra->freestk.p[--ra->freestk.n];
      ra->allocs[var] = astack(s);
   } else {
      if (ra->stktop+1 >= ra->maxstk) ra->maxstk = ra->stktop+1;
      s = ra->stktop++;
   }
   ra->allocs[var] = astack(s);
   return s;
}

static void
spill(struct rega *ra, int reg, struct block *blk, int curi) {
   int var, s;
   struct instr load;

   if (!ra->regs[reg].t) return;
   var = ra->regs[reg].i;
   assert(ra->regs[reg].t == RTMP && *(ushort *)&ra->allocs[var] == *(ushort *)&areg(reg));
   s = allocstk(ra, var);
   DBG("-- spill %%%d %s -> s%d\n", var, mctarg->rnames[reg], s);
   instrtab[var].reg = 0;
   /* insert 'reg = load [slot]' */
   load = mkinstr(Oloads1 + 2*ilog2(cls2siz[insrescls(instrtab[var])]), insrescls(instrtab[var]), mkref(RICON, s*8));
   load.reg = reg+1;
   addstkslotref(insertinstr(blk, ++curi, load));
   freereg(ra, reg);
}

#define mkmove(k, rd, rs) mkinstr(Omove, k, mkref(RREG, rd), mkref(RREG, rs))

static void
forcetake(struct rega *ra, int reg, union ref ref, struct block *blk, int curi) {
   int var;
   struct alloc *alloc;

   if (ra->regs[reg].bits == ref.bits) return;
   if (!ra->regs[reg].t) {
      take(ra, reg, ref);
      return;
   }
   assert(ra->regs[reg].t == RTMP);
   var = ra->regs[reg].i;
   alloc = &ra->allocs[var];
   assert(alloc->a == reg);
   *alloc = afree();
   /* try to move temp to another register */
   if (isgpr(reg) ? ra->nfreegpr > 0 : ra->nfreefpr > 0) {
      /* the register of the current instruction (if any) was already free'd (by def), so
       * we need to explictly exclude it from the pool of rename registers
       * e.g.: given 'R0 = copy R1'; if R1 => %x, we need to prevent renaming %x => R0
       */
      int excl = instrtab[blk->ins.p[curi]].reg-1;
      int rename = allocreg(ra, isgpr(reg) ? KI4 : KF4, ra->regs[reg], excl);
      if (ccopt.dbg.r)DBG("-- rename %%%d %s -> %s\n", var, mctarg->rnames[reg], mctarg->rnames[rename]);
      /* introduce move from rename -> original (since we allocate backwards) */
      insertinstr(blk, ++curi, mkmove(insrescls(instrtab[var]), reg, rename));
      instrtab[var].reg = rename+1;
      ra->regs[rename] = mkref(RTMP, var);
      bsset(globusage, rename);
      *alloc = areg(rename);
      ra->regs[reg].bits = 0;
   } else {
      spill(ra, reg, blk, curi);
   }
   take(ra, reg, ref);
}

/* mark a use for *ref, possibly allocating a register for it, considering it won't clash with `other` */
static void
use(struct rega *ra, struct block *blk, int curi, enum op op, int hint, union ref *ref, union ref other)
{
   struct instr *ins;
   int excl = other.t == RREG ? other.i : -1;

   if (ref->t == RMORE) {
      struct addr addr = addrht[ref->i];
      if (addr.base.t) use(ra, blk, curi, 0, hint, &addr.base, addr.index);
      if (addr.index.t) use(ra, blk, curi, 0, hint, &addr.index, NOREF);
      *ref = mkaddr(addr);
      return;
   } else if (ref->t == RREG) {
      assert(ref->i != excl);
      forcetake(ra, ref->i, *ref, blk, curi);
   }
   if (ref->t != RTMP) return;

   ins = &instrtab[ref->i];
   if (oisalloca(ins->op)) return;
   if (!ins->cls) return;
   if (!ins->reg) {
      int s = -1;
      if (ra->allocs[ref->i].t == ASTACK)
         s = ra->allocs[ref->i].a;

      assert(ins->op != Ocall);
      if (hint == -1 && ins->op == Ocopy && ins->l.t == RREG) /* for '%x = copy Rx', hint %x to use Rx */
         hint = ins->l.i;
      if (hint != -1 && hint != excl && !ra->regs[hint].t) {
         take(ra, hint, *ref);
         ins->reg = hint + 1;
      } else {
         ins->reg = allocreg(ra, insrescls(*ins), *ref, excl) + 1;
      }
      if (s >= 0) {
         /* unspill, insert 'store [slot], reg' */
         DBG("-- unspill %%%d s%d -> %s\n", ref->i, s, mctarg->rnames[ins->reg-1]);
         struct instr store = mkinstr(Ostore1 + ilog2(cls2siz[insrescls(*ins)]), 0,
                                      mkref(RICON, s*8),
                                      mkref(RREG, ins->reg-1));
         addstkslotref(insertinstr(blk, ++curi, store));
         vpush(&ra->freestk, s);
      }
   }
   /* do not patch ref if it's cond branch arg, emit() wants to know what instr it is */
   if (ref != blk->jmp.arg || blk->jmp.t != Jb)
      *ref = mkref(RREG, ins->reg-1);
}

void
regalloc(struct function *fn)
{
   extern int ninstr;
   struct instr *ins;
   struct block *last = fn->entry->lprev, *blk;
   static union ref *stkslotrefsbuf[64];
   struct rega ra = {0};

   vinit(&stkslotrefs, stkslotrefsbuf, arraylength(stkslotrefsbuf));
   ra.allocs = xcalloc((ninstr*2 < MAXINSTR ? ninstr*2 : MAXINSTR) * sizeof(struct alloc));
   ra.nfreegpr = mctarg->ngpr - popcnt(mctarg->rglob->u);
   ra.nfreefpr = mctarg->nfpr;
   for (int i = 0; i < MAXREGS; ++i) {
      if (in_range(i, mctarg->fpr0, mctarg->fpr0 + mctarg->nfpr - 1))
         bsset(floatregs, i);
      if (bstest(mctarg->rglob, i))
         ra.regs[i] = mkref(RREG, i);
   }
   bszero(globusage, 1);
   fn->stksiz = alignup(fn->stksiz, 8);

   /* a dumb linear register allocator that visits instructions physically backwards
    * starting at the end of the function, when encountering a use of a new
    * temporary, it allocates a register for it. when encountering the definition
    * of a temporary, it frees up its register
    */
   blk = last;
   do {
      for (int i = 0; i < 2; ++i) {
         if (!blk->jmp.arg[i].t) break;
         /* do not allocate a reg for a cmp op used a branch argument, since it's a pseudo op */
         if (blk->jmp.t == Jb && blk->jmp.arg[i].t == RTMP && oiscmp(instrtab[blk->jmp.arg[i].i].op))
            break;
         use(&ra, blk, blk->ins.n-1, (blk->jmp.t != Jb) - 1,
                  blk->jmp.t == Jret ? fn->abiret[i].reg : -1,
                  &blk->jmp.arg[i], blk->jmp.arg[!i]);
      }

      for (int i = blk->ins.n - 1; i >= 0; --i) {
         int hint0 = -1, hint1 = -1;
         ins = &instrtab[blk->ins.p[i]];
         if (ins->op != Ocopy && !ra.allocs[ins - instrtab].t && ins->skip) { /* unused */
            *ins = mkinstr(Onop, 0,);
            continue;
         }
         def(&ra, ins, blk, i);
         if (ins->op != Ocall) {
            if (ins->op == Ocopy || ins->inplace) hint0 = ins->reg - 1;
            if (ins->op == Omove) {
               if (ins->l.t == RREG) hint1 = ins->l.i;
               /* MOV Rx,Rx is used by isel to indicate a clobber,
                * so it should be a def point for Rx but not a use point */
               if (ins->r.bits != ins->l.bits)
                  use(&ra, blk, i, ins->op, hint1, &ins->r, NOREF);
            } else {
               if (ins->l.t) use(&ra, blk, i, ins->op, hint0, &ins->l, ins->r);
               if (ins->r.t) use(&ra, blk, i, ins->op, hint1, &ins->r, NOREF);
            }
         } else {
            struct call *call = &calltab.p[ins->r.i];
            struct bitset rspill[1] = {0};

            for (int r = mctarg->gpr0; r < mctarg->gpr0 + mctarg->ngpr; ++r)
               if (!bstest(mctarg->rglob, r) && !bstest(mctarg->rcallee, r))
                  bsset(rspill, r);
            for (int r = mctarg->fpr0; r < mctarg->fpr0 + mctarg->nfpr; ++r)
               if (!bstest(mctarg->rglob, r) && !bstest(mctarg->rcallee, r))
                  bsset(rspill, r);

            if (call->abiret[0].ty.bits) bsclr(rspill, call->abiret[0].reg);
            if (call->abiret[1].ty.bits) bsclr(rspill, call->abiret[1].reg);
            for (uint r = 0; bsiter(&r, rspill, 1); ++r) {
               spill(&ra, r, blk, i);
            }
            for (int j = 0; j < call->narg; ++j) {
               short reg = call->abiarg[j].reg;
               if (reg >= 0) {
                  forcetake(&ra, reg, mkref(RREG, reg), blk, i);
               }
            }

            use(&ra, blk, i, ins->op, hint0, &ins->l, NOREF);
         }
         if (ins->op == Ocopy && !ins->reg)
            *ins = mkinstr(Onop, 0,);
         if (ins->inplace && ins->reg) {
            assert(ins->l.t == RREG);
            if (ins->reg-1 != ins->l.i) {
               /* an in-place operation where the destination does not
                * match the first operand, so we need to add a move */
               insertinstr(blk, i, mkmove(insrescls(*ins), ins->reg-1, ins->l.i));
               ins->l.i = ins->reg-1;
            }
         }
      }
      for (int i = blk->phi.n - 1; i >= 0; --i) {
         struct phi *phi;
         ins = &instrtab[blk->phi.p[i]];
         assert(ins->op == Ophi);
         phi = &phitab.p[ins->l.i];
         if (ins->reg) {
            /* introduce necessary moves in each pred,
             * XXX this doesn't work for backwards branches */
            for (int i = 0; i < phi->n; ++i) {
               struct instr mov = mkinstr(Omove, insrescls(*ins), mkref(RREG, ins->reg-1), phi->ref[i]);
               insertinstr(phi->blk[i], phi->blk[i]->ins.n, mov);
            }
         }
         def(&ra, ins, NULL, 0);
      }
   } while ((blk = blk->lprev) != last);

   do vfree(&blk->phi); while ((blk = blk->lprev) != last);

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
   bscopy(fn->regusage, globusage, 1);
   free(ra.allocs);
   vfree(&ra.freestk);
}

/* vim:set ts=3 sw=3 expandtab: */
