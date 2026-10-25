#include "ir.h"
#include "u_bits.h"

/* Implements algorithm in 'Simple and Efficient Construction of Static Single Assignment' (Braun et al) */

typedef struct SSABuilder {
   Arena **arena;
   struct Var {
      uchar cls;
      ushort i;
      Ref curdefs[]; /* map blk -> def */
   } **vars;
   BitSet *sealed, /* set of sealed blocks */
          *marked; /* blocks marked, for 'Marker Algorithm' in the paper */
   int lastvisit;
   int nblk;
} SSABuilder;
typedef struct Var Var;

static Ref readvar(SSABuilder *, Var *, Block *);

static Ref
deltrivialphis(SSABuilder *sb, Var *var, Block *blk, Ref phiref) {
   assert(instrtab[phiref.i].op == Ophi);
   Ref *args = phiargs(phiref.i);
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

   struct trivuser { Block *blk; int u; };
   DEF_SVEC(struct trivuser, 8, trivusers);
   for (IRUse *use = instruse[phiref.i]; use; use = use->next)
      if (use->u != USERJUMP && instrtab[use->u].op == Ophi && use->u != phiref.i)
         vpush(&trivusers, ((struct trivuser) { use->blk, use->u }));

   /* replace uses */
   replcuses(phiref, same, NULL, REPLC_ALL);
   for (int i = blk->id; i < sb->nblk; ++i) {
      if (var->curdefs[i].bits == phiref.bits)
         var->curdefs[i] = same;
   }

   /* stops infinite recursion and marks phi for removal */
   instrtab[phiref.i].op = Onop;

   /* recursively try to remove all phi users as they might have become trivial */
   for (int i = 0; i < trivusers.n; ++i) {
      Ref it = mkref(RTMP, trivusers.p[i].u);
      if (instrtab[trivusers.p[i].u].op != Ophi) continue; /* already collapsed */
      Ref vphi2 = deltrivialphis(sb, var, trivusers.p[i].blk, it);
      if (vphi2.bits != it.bits) {
         same = vphi2;
      }
   }
   vfree(&trivusers);

   return same;
}

static Ref
addphiargs(SSABuilder *sb, Var *var, Block *blk, Ref phiref) {
   Ref *args = phiargs(phiref.i);
   for (int i = 0; i < blk->npred; ++i) {
      Block *pred = blkpred(blk, i);
      args[i] = readvar(sb, var, pred);
      adduse(blk, phiref.i, args[i]);
   }
   instrtab[phiref.i].r.bits = 0;
   return deltrivialphis(sb, var, blk, phiref);
}

static void
writevar(SSABuilder *sb, Var *var, Block *blk, Ref val) {
   if (val.t == RTMP) assert(instrtab[val.i].op != Onop);
   var->curdefs[blk->id] = val;
}

enum { RPENDINGPHI = 7 };
static Ref
readvarrec(SSABuilder *sb, Var *var, Block *blk) {
   Ref val;
   assert(blk->npred > 0);
   if (!bstest(sb->sealed, blk->id)) { /* unsealed block */
      /* add pending phi */
      val = insertphi(blk, var->cls);
      instrtab[val.i].r = mkref(RPENDINGPHI, var->i);
   } else if (blk->npred == 1) { /* trivial case when block has a single predecessor */
      val = readvar(sb, var, blkpred(blk, 0));
   } else if (!bstest(sb->marked, blk->id)) {
      /* Marker Algorithm to optimize the common case where a control flow join doesn't
       * need a phi function for the variable: mark the block in case we recursively reach
       * it again, collect defs from predecessors, only create a phi if they differ.
       * This avoids creating temporary trivial phi functions in acyclic data flow
       */
      bsset(sb->marked, blk->id);
      for (int i = 0; i < blk->npred; ++i) {
         Block *pred = blkpred(blk, i);
         Ref it = readvar(sb, var, pred);
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
      val = insertphi(blk, var->cls);
      writevar(sb, var, blk, val);
      val = addphiargs(sb, var, blk, val);
      bsclr(sb->marked, blk->id);
      return val;
   }
   writevar(sb, var, blk, val);
   return val;
}

static Ref
readvar(SSABuilder *sb, Var *var, Block *blk) {
   if (var->curdefs[blk->id].bits)
      return var->curdefs[blk->id];
   if (blk->npred == 0) /* entry block, var is read before being written to */
      return UNDREF;
   return readvarrec(sb, var, blk);
}

static bool
trysealrec(SSABuilder *sb, Block *blk) {
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
         addphiargs(sb, sb->vars[ins->r.i], blk, mkref(RTMP, blk->phi.p[i]));
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
tryseal(SSABuilder *sb, Block *blk) {
   startbbvisit();
   trysealrec(sb, blk);
}

/*
 * delredundantphis:
 * This approach to remove redundant phi cycles is based on cranelift's remove_constant_phi
 * pass: <https://github.com/bytecodealliance/wasmtime/blob/v50.0.0/cranelift/codegen/src/remove_constant_phis.rs>
 * This uses an iterative forward dataflow analysis. Algorithm:
 *
 *   1. Create a lattice of abstract values where all phis start as 'Bottom' (NOREF) and other
 *      values map to themselves. A phi with one distinct non-cyclic input maps to 'value',
 *      and those with more than one distinct input, map to 'Many' (AMANY).
 *
 *   2. foreach block in RPO: foreach phi: join the phi's abstract value with its inputs', such that
 *       : Bottom & x = x
 *       : x & Bottom = x
 *       : value1 & value2 = Many  where value1 != value2
 *       : value  & value  = value
 *       : Many   & _      = Many = _ & Many
 *
 *   3. Repeat #2 until a fixpoint
 *
 *   4. Delete redundant phis ('Bottom' and single value) and replace their uses
 *
 * I believe it is equivalent in producing a minimal SSA form to the SCC algorithm described in
 * the Braun 2013 paper.
 */

static const Ref AMANY = {.bits = -999};
static Ref
getabstract(Ref *abstracts, Ref r) {
   if (r.t != RTMP) return r;
   assert(instrtab[r.i].op != Onop);
   if (instrtab[r.i].op != Ophi) return r;
   return abstracts[r.i];
}
static Ref
joinabstract(Ref a, Ref b) {
   if (!a.bits) return b;
   if (!b.bits) return a;
   if (a.bits == AMANY.bits || b.bits == AMANY.bits) return AMANY;
   if (a.bits == b.bits) return a;
   return AMANY;
}

static void
delredundantphis(SSABuilder *sb, Function *fn) {
   extern int ninstrtab;
   Ref *abstracts = allocz(sb->arena, ninstrtab * sizeof *abstracts, 0);
   int candidate = 0;
   for (bool chg = 1; chg;) {
      chg = 0;
      for (Block *b = fn->entry->lnext; b != fn->entry; b = b->lnext) {
         if (!b->phi.n) continue;
         for (int i = 0; i < b->phi.n; ++i) {
            int p = b->phi.p[i];
            if (instrtab[p].op == Onop) continue;
            Ref abstract = abstracts[p];
            for (int j = 0; j < b->npred; ++j) {
               Ref arg = phiargs(p)[j];
               abstract = joinabstract(abstract, getabstract(abstracts, arg));
            }
            if (abstract.bits != abstracts[p].bits) {
               abstracts[p] = abstract;
               chg = 1;
               candidate |= abstract.bits != AMANY.bits;
            }
         }
      }
      if (!candidate) return;
   }

   for (Block *b = fn->entry->lnext; b != fn->entry; b = b->lnext) {
      if (!b->phi.n) continue;
      for (int i = 0; i < b->phi.n; ++i) {
         int p = b->phi.p[i];
         if (instrtab[p].op == Onop) continue;
         Ref abstract = abstracts[p];
         if (abstract.bits == AMANY.bits) continue;
         replcuses(mkref(RTMP, p), abstract.bits ? abstract : UNDREF, NULL, REPLC_ALL);
         instrtab[p].op = Onop;
      }
   }
}

void
mem2reg(Function *fn) {
   SSABuilder sb = { fn->passarena, .nblk = fn->nblk };

   FREQUIRE(FNUSE);

   sb.sealed = anewbitset(sb.arena, fn->nblk);
   sb.marked = anewbitset(sb.arena, fn->nblk);
   extern int ninstrtab;
   sb.vars = allocz(sb.arena, ninstrtab * sizeof *sb.vars, 0);
   sortrpo(fn);

   static void *DEAD = "X";
   Block *blk = fn->entry;
   /* gather variables */
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         enum irclass k = 0;
         int sz = 0;
         int var = blk->ins.p[i], nwrite = 0, nread = 0;
         Instr *ins = &instrtab[var];

         /* find allocas only used in loads/stores of uniform size */
         if (!oisalloca(ins->op) || (ins->f&INSKEEP)) continue;
         for (IRUse *use = instruse[var]; use; use = use->next) {
            if (use->u == USERJUMP) goto Skip;
            Instr *m = &instrtab[use->u];
            if (m->f&INSVOLATILE) goto Skip;
            if (oisload(m->op) && (!sz || sz == loadstoresz(m->op))) {
               ++nwrite;
               sz = loadstoresz(m->op);
               k = loadstorecls(m->op);
            } else if (oisstore(m->op) && m->l.bits == mkref(RTMP, var).bits
              && (!sz || sz == loadstoresz(m->op))) {
               ++nread;
               sz = loadstoresz(m->op);
            } else goto Skip;
         }
         if (nwrite && nread) { /* vars with no reads or no writes are dead */
            Var *v = allocz(sb.arena, sizeof *v + sb.nblk*sizeof *v->curdefs, 0);
            v->cls = k;
            v->i = var;
            sb.vars[var] = v;
            /* var is undef at allocation point, this matters when declared inside loops */
            v->curdefs[blk->id] = UNDREF;
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
      for (int i = 0, t; i < blk->ins.n; ++i) {
         Instr *ins = &instrtab[t = blk->ins.p[i]];
         Var *var;
         if (!oisloadstore(ins->op)) continue;
         if (ins->l.t != RTMP || !(var = sb.vars[ins->l.i])) continue;
         if (oisstore(ins->op)) {
            if (var != DEAD && loadstorecls(ins->op) != var->cls) { /* needs a bit cast */
               *ins = mkinstr1(Ocopy, var->cls, ins->r);
               writevar(&sb, var, blk, mkref(RTMP, t));
            } else {
               if (var != DEAD)
                  writevar(&sb, var, blk, ins->r);
               deluse(blk, t, ins->r);
               *ins = mkinstr0(Onop,0);
            }
         } else if (var == DEAD) {
            *ins = mkinstr1(Ocopy, ins->cls, UNDREF);
         } else { /* load */
            enum irclass k = ins->cls;
            Ref val = readvar(&sb, var, blk);
            adduse(blk, t, val);
            enum op ext = loadstoresz(ins->op) < 4 ? load2ext(ins->op) : Ocopy;
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

   /* remove redundant phis created by dataflow cycles */
   delredundantphis(&sb, fn);

   /* remove phis marked for deletion */
   do {
      for (int i = 0; i < blk->phi.n; ++i)
         if (instrtab[blk->phi.p[i]].op == Onop)
            delphi(blk, i--);
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
