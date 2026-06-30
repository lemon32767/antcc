#include "ir.h"
#include "u_bits.h"

static const uchar loadszcls[] = {
   [Oloads8  - Oloads8] = 1|KI32<<4, [Oloadu8  - Oloads8] = 1|KI32<<4,
   [Oloads16 - Oloads8] = 2|KI32<<4, [Oloadu16 - Oloads8] = 2|KI32<<4,
   [Oloads32 - Oloads8] = 4|KI32<<4, [Oloadu32 - Oloads8] = 4|KI32<<4,
   [Oloadi64 - Oloads8] = 8|KI64<<4,
   [Oloadf32 - Oloads8] = 4|KF32<<4,
   [Oloadf64 - Oloads8] = 8|KF64<<4,
};
static const uchar load2ext[] = {
   [Oloads8  - Oloads8] = Oexts8,  [Oloadu8  - Oloads8] = Oextu8,
   [Oloads16 - Oloads8] = Oexts16, [Oloadu16 - Oloads8] = Oextu16,
   [Oloads32 - Oloads8] = Oexts32, [Oloadu32 - Oloads8] = Oextu32,
   [Oloadi64 - Oloads8] = Ocopy,
};
static const uchar storesz[] = {
   [Ostorei8  - Ostorei8] = 1,
   [Ostorei16 - Ostorei8] = 2,
   [Ostorei32 - Ostorei8] = 4,
   [Ostorei64 - Ostorei8] = 8,
   [Ostoref32 - Ostorei8] = 4,
   [Ostoref64 - Ostorei8] = 8,
};
#define loadsz(o) (loadszcls[(o) - Oloads8] & 0xF)
#define loadcls(o) (loadszcls[(o) - Oloads8] >> 4)
#define load2ext(o) (load2ext[(o) - Oloads8])
#define storesz(o) (storesz[(o) - Ostorei8])

/* Implements algorithm in 'Simple and Efficient Construction of Static Single Assignment' (Braun et al) */

typedef struct SSABuilder {
   Arena **arena;
   struct Var {
      uchar cls, ext;
      ushort i;
      Ref curdefs[]; /* map blk -> def */
   } **vars;
   BitSet *sealed, /* set of sealed blocks */
          *marked; /* blocks marked, for 'Marker Algorithm' in the paper */
   int lastvisit;
   int nblk;
} SSABuilder;
typedef struct Var Var;

static Ref readvar(SSABuilder *, Var *, enum irclass, Block *);

static Ref
deltrivialphis(SSABuilder *sb, Var *var, Block *blk, Ref phiref)
{
   assert(instrtab[phiref.i].op == Ophi);
   Ref *args = phitab.p[instrtab[phiref.i].l.i];
   Ref same = {0};
   for (int i = 0; i < blk->npred; ++i) {
      if (args[i].bits == same.bits || args[i].bits == phiref.bits) {
         continue; /* unique value or self-reference */
      } if (same.bits != 0) {
         return phiref; /* non-trivial */
      }
      same = args[i];
   }

   if (same.bits == 0)
      same = UNDREF; /* the phi is unreachable or in the start block */

   /* replace uses */
   replcuses(phiref, same);
   for (int i = blk->id; i < sb->nblk; ++i) {
      if (var->curdefs[i].bits == phiref.bits)
         var->curdefs[i] = same;
   }

   /* stops infinite recursion and marks phi for removal */
   instrtab[phiref.i].op = Onop;

   /* recursively try to remove all phi users as they might have become trivial */
   for (IRUse *use = instruse[phiref.i], *next; use; use = next) {
      next = use->next;
      if (use->u != USERJUMP && instrtab[use->u].op == Ophi && use->u != phiref.i) {
         Ref it = mkref(RTMP, use->u);
         Ref vphi2 = deltrivialphis(sb, var, use->blk, it);
         if (vphi2.bits != it.bits) {
            same = vphi2;
         }
      }
   }
   deluses(phiref.i);

   return same;
}

static Ref
addphiargs(SSABuilder *sb, Var *var, enum irclass cls, Block *blk, Ref phiref)
{
   Ref *args = phitab.p[instrtab[phiref.i].l.i];
   for (int i = 0; i < blk->npred; ++i) {
      Block *pred = blkpred(blk, i);
      args[i] = readvar(sb, var, cls, pred);
      adduse(blk, phiref.i, args[i]);
   }
   instrtab[phiref.i].r.bits = 0;
   return deltrivialphis(sb, var, blk, phiref);
}

static void
writevar(SSABuilder *sb, Var *var, Block *blk, Ref val)
{
   if (val.t == RTMP) assert(instrtab[val.i].op != Onop);
   var->curdefs[blk->id] = val;
}

enum { RPENDINGPHI = 7 };
static Ref
readvarrec(SSABuilder *sb, Var *var, enum irclass cls, Block *blk)
{
   Ref val;
   assert(blk->npred > 0);
   if (!bstest(sb->sealed, blk->id)) { /* unsealed block */
      /* add pending phi */
      val = insertphi(blk, cls);
      instrtab[val.i].r = mkref(RPENDINGPHI, var->i);
   } else if (blk->npred == 1) { /* trivial case when block has a single predecessor */
      val = readvar(sb, var, cls, blkpred(blk, 0));
   } else if (!bstest(sb->marked, blk->id)) {
      /* Marker Algorithm to optimize the common case where a control flow join doesn't
       * need a phi function for the variable: mark the block in case we recursively reach
       * it again, collect defs from predecessors, only create a phi if they differ.
       * This avoids creating temporary trivial phi functions in acyclic data flow
       */
      bsset(sb->marked, blk->id);
      for (int i = 0; i < blk->npred; ++i) {
         Block *pred = blkpred(blk, i);
         Ref it = readvar(sb, var, cls, pred);
         if (!bstest(sb->marked, blk->id)) {
            /* recursion reached this blk again, use its phi */
            /* must have called writevar */
            return var->curdefs[blk->id];
         }
         if (i == 0) val = it;
         else if (it.bits != val.bits) /* found a different definition, must add phi */
            goto AddPhi;
      }
      bsclr(sb->marked, blk->id);
   } else { /* reached block while recursing */
   AddPhi:
      val = insertphi(blk, cls);
      writevar(sb, var, blk, val);
      val = addphiargs(sb, var, cls, blk, val);
      bsclr(sb->marked, blk->id);
      return val;
   }
   writevar(sb, var, blk, val);
   return val;
}

static Ref
readvar(SSABuilder *sb, Var *var, enum irclass cls, Block *blk)
{
   if (var->curdefs[blk->id].bits)
      return var->curdefs[blk->id];
   if (blk->npred == 0) /* entry block, var is read before being written to */
      return UNDREF;
   return readvarrec(sb, var, cls, blk);
}

static bool
trysealrec(SSABuilder *sb, Block *blk)
{
Recur:
   if (bstest(sb->sealed, blk->id)) return 1;
   if (blk->id > sb->lastvisit) return 0;
   markvisited(blk);
   for (int i = 0; i < blk->npred; ++i) {
      Block *p = blkpred(blk, i);
      if (wasvisited(p)) continue;
      if (p->id > sb->lastvisit) return 0;
   }

   bsset(sb->sealed, blk->id);
   for (int i = 0; i < blk->phi.n; ++i) {
      Instr *ins = &instrtab[blk->phi.p[i]];
      if (ins->r.t == RPENDINGPHI)
         addphiargs(sb, sb->vars[ins->r.i], ins->cls, blk, mkref(RTMP, blk->phi.p[i]));
   }
   if (blk->s1 && !blk->s2) {
      blk = blk->s1;
      goto Recur;
   } else if (blk->s1 && blk->s2) {
      trysealrec(sb, blk->s1);
      blk = blk->s2;
      goto Recur;
   }
   return 1;
}

static void
tryseal(SSABuilder *sb, Block *blk)
{
   startbbvisit();
   trysealrec(sb, blk);
}

void
mem2reg(Function *fn)
{
   SSABuilder sb = { fn->passarena, .nblk = fn->nblk };

   FREQUIRE(FNUSE);

   sb.sealed = allocz(sb.arena, BSSIZE(fn->nblk) * sizeof *sb.sealed, 0);
   sb.marked = allocz(sb.arena, BSSIZE(fn->nblk) * sizeof *sb.sealed, 0);
   extern int ninstrtab;
   sb.vars = allocz(sb.arena, ninstrtab * sizeof *sb.vars, 0);
   sortrpo(fn);

   static void *DEAD = "X";
   Block *blk = fn->entry;
   /* gather variables */
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         enum op ext = Ocopy;
         enum irclass k = 0;
         int sz = 0;
         int var = blk->ins.p[i], nwrite = 0, nread = 0;
         Instr *ins = &instrtab[var];
         
         /* find allocas only used in loads/stores of uniform size */
         if (!oisalloca(ins->op) || ins->keep/*!volatile*/) continue;
         for (IRUse *use = instruse[var]; use; use = use->next) {
            if (use->u == USERJUMP) goto Skip;
            Instr *m = &instrtab[use->u];
            if (m->keep) /*volatile*/ goto Skip;
            if (oisload(m->op) && (!sz || sz == loadsz(m->op))) {
               ++nwrite;
               sz = loadsz(m->op);
               k = loadcls(m->op);
               if (sz < 4) ext = load2ext(m->op);
            } else if (oisstore(m->op) && m->l.bits == mkref(RTMP, var).bits
              && (!sz || sz == storesz(m->op))) {
               ++nread;
               sz = storesz(m->op);
            } else goto Skip;
         }
         if (nwrite && nread) { /* vars with no reads or no writes are dead */
            Var *v = allocz(sb.arena, sizeof *v + sb.nblk*sizeof *v->curdefs, 0);
            v->ext = ext;
            v->cls = k;
            v->i = var;
            sb.vars[var] = v;
         } else {
            sb.vars[var] = DEAD;
         }
         /* remove alloca */
         delinstr(blk, i--);
      Skip:;
      }
   } while ((blk = blk->lnext) != fn->entry);

   /* perform SSA construction */
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         Instr *ins = &instrtab[blk->ins.p[i]];
         Var *var;
         if (!oisloadstore(ins->op)) continue;
         if (ins->l.t != RTMP || !(var = sb.vars[ins->l.i])) continue;
         if (oisstore(ins->op)) {
            if (var != DEAD)
               writevar(&sb, var, blk, ins->r);
            *ins = mkinstr0(Onop,0);
         } else if (var == DEAD) {
            *ins = mkinstr1(Ocopy, ins->cls, UNDREF);
         } else {
            enum irclass k = ins->cls;
            enum op ext = var->ext;
            Ref val = readvar(&sb, var, k, blk);
            adduse(blk, blk->ins.p[i], val);
            if (ext != Ocopy && isnumcon(val)) {
               /* fold constant expression */
               val = irunop(NULL, ext, k, val);
               assert(val.bits);
               ext = Ocopy;
            }
            *ins = mkinstr1(ext, k, val);
         }
      }

      assert(sb.lastvisit == blk->id);
      tryseal(&sb, blk);
      ++sb.lastvisit;
   } while ((blk = blk->lnext) != fn->entry);

   /* remove phis marked for deletion */
   do {
      for (int i = 0; i < blk->phi.n; ++i)
         if (instrtab[blk->phi.p[i]].op == Onop)
            delphi(blk, i--);
   } while ((blk = blk->lnext) != fn->entry);

   if (ccopt.dbg.m) {
      bfmt(ccopt.dbgout, "<< After mem2reg >>\n");
      irdump(fn);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
