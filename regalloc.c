#include "common.h"
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
   imap_of(struct alloc) allocs; /* map tmpidx -> allocation */
   int nfreegpr, nfreefpr;
};

static void
def(struct rega *ra, struct instr *ins)
{
   int reg = -1, var;
   struct alloc *alloc;
   if (ins->op != Omove) {
      var = ins - instrtab;
      // efmt("def %%%d\n",var);
      if ((alloc = imap_get(&ra->allocs, var))) {
         if (alloc->t == AREG) {
            reg = alloc->a;
            // efmt("-- free %s for %%%d\n", mctarg->rnames[alloc->a], var);
            assert(ra->regs[reg].bits == mkref(RTMP, var).bits);
         } else assert(0);
         *alloc = afree();
      }
   } else {
      reg = ins->l.i;
      assert(ins->l.t == RREG);
      // efmt("-- free %s\n", mctarg->rnames[ins->l.i]);
   }
   if (reg != -1) {
      ra->regs[reg] = NOREF;
      if (isfpr(reg)) ++ra->nfreefpr;
      else            ++ra->nfreegpr;
   }
}

static void
take(struct rega *ra, int reg, union ref ref) {
   // efmt("-- take %s for %d %d\n", mctarg->rnames[reg], ref.t, ref.i);
   assert(!ra->regs[reg].t && "taken");
   if (ref.t == RTMP)
      imap_set(&ra->allocs, ref.i, areg(reg));
   ra->regs[reg] = ref;
   bsset(globusage, reg);
   if (isfpr(reg)) --ra->nfreefpr;
   else            --ra->nfreegpr;
}

static int
nextreg(struct rega *ra, enum irclass cls, union ref ref, int excl)
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
   alloc = imap_get(&ra->allocs, var);
   assert(alloc && alloc->a == reg);
   *alloc = afree();
   /* try to move temp to another register */
   if (isgpr(reg) ? ra->nfreegpr > 0 : ra->nfreefpr > 0) {
      /* the register of the current instruction (if any) was already free'd (by def), so
       * we need to explictly exclude it from the pool of rename registers
       * e.g.: given 'R0 = copy R1'; if R1 => %x, we need to prevent renaming %x => R0
       */
      int excl = instrtab[blk->ins.p[curi]].reg-1;
      int rename = nextreg(ra, isgpr(reg) ? KI4 : KF4, ra->regs[reg], excl);
      if (ccopt.dbg.r) efmt("-- rename %%%d %s -> %s\n", var, mctarg->rnames[reg], mctarg->rnames[rename]);
      /* introduce move from rename -> original (since we allocate backwards) */
      insertinstr(blk, ++curi, mkmove(instrtab[var].cls, reg, rename));
      instrtab[var].reg = rename+1;
      ra->regs[rename] = mkref(RTMP, var);
      bsset(globusage, rename);
      imap_set(&ra->allocs, var, areg(rename));
      ra->regs[reg].bits = 0;
   } else {
      assert(!"spill");
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
      struct addr *addr = &addrtab.p[ref->i];
      if (addr->base.t) use(ra, blk, curi, 0, hint, &addr->base, addr->index);
      if (addr->index.t) use(ra, blk, curi, 0, hint, &addr->index, NOREF);
      return;
   } else if (ref->t == RREG) {
      forcetake(ra, ref->i, *ref, blk, curi);
   }
   if (ref->t != RTMP) return;

   ins = &instrtab[ref->i];
   if (oisalloca(ins->op)) return;
   if (!ins->cls) return;
   if (!ins->reg) {
      if (op == -1) /* cond branch */
         if (oiscmp(ins->op) && ref->i == blk->ins.p[blk->ins.n-1])
            /* result of comparison instr is only used to conditionally branch,
             * doesn't usually need a reg (this should be handled by isel) */
            return;
      assert(ins->op != Ocall);
      if (hint == -1 && ins->op == Ocopy && ins->l.t == RREG) /* for '%x = copy Rx', hint %x to use Rx */
         hint = ins->l.i;
      if (hint != -1 && hint != excl && !ra->regs[hint].t) {
         take(ra, hint, *ref);
         ins->reg = hint + 1;
      } else {
         ins->reg = nextreg(ra, ins->cls, *ref, excl) + 1;
      }
      *ref = mkref(RREG, ins->reg-1);
   }
}

void
regalloc(struct function *fn)
{
   struct instr *ins;
   struct block *last = fn->entry->lprev, *blk;
   struct rega ra = {0};
   ra.nfreegpr = mctarg->ngpr - popcnt(mctarg->rglob->u);
   ra.nfreefpr = mctarg->fpr;
   for (int i = 0; i < MAXREGS; ++i)
      if (in_range(i, mctarg->fpr0, mctarg->fpr0 + mctarg->nfpr - 1))
            bsset(floatregs, i);

   /* a dumb linear register allocator that visits instructions physically backwards
    * starting at the end of the function, when encountering a use of a new
    * temporary, it allocates a register for it. when encountering the definition
    * of a temporary, it frees up its register
    */
   blk = last;
   do {
      for (int i = 0; i < 2; ++i) {
         if (!blk->jmp.arg[i].t) break;
         use(&ra, blk, blk->ins.n-1, (blk->jmp.t != Jb) - 1,
                  blk->jmp.t == Jret ? fn->abiret[i].reg : -1,
                  &blk->jmp.arg[i], blk->jmp.arg[!i]);
      }

      for (int i = blk->ins.n - 1; i >= 0; --i) {
         int hint0 = -1, hint1 = -1;
         ins = &instrtab[blk->ins.p[i]];
         if (!ins->reg && ins->skip) { /* unused */
            *ins = mkinstr(Onop, 0,);
            continue;
         }
         def(&ra, ins);
         if (ins->op != Ocall) {
            if (ins->op == Ocopy) hint0 = ins->reg - 1;
            if (ins->op == Omove) {
               hint1 = ins->l.i;
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
            use(&ra, blk, i, ins->op, hint0, &ins->l, NOREF);
         }
         if (ins->op == Ocopy && !ins->reg)
            *ins = mkinstr(Onop, 0,);
         if (ins->inplace && ins->reg) {
            assert(ins->l.t == RREG);
            if (ins->reg-1 != ins->l.i) {
               /* an in-place operation where the destination does not
                * match the first operand, so we need to add a move */
               insertinstr(blk, i, mkmove(ins->cls, ins->reg-1, ins->l.i));
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
               struct instr mov = mkinstr(Omove, ins->cls, mkref(RREG, ins->reg-1), phi->ref[i]);
               insertinstr(phi->blk[i], phi->blk[i]->ins.n, mov);
            }
         }
         def(&ra, ins);
      }
   } while ((blk = blk->lprev) != last);

   do vfree(&blk->phi); while ((blk = blk->lprev) != last);

   if (ccopt.dbg.r) {
      efmt("after regalloc:\n");
      irdump(fn, fn->name);
   }
   bscopy(fn->regusage, globusage, 1);
}

/* vim:set ts=3 sw=3 expandtab: */
