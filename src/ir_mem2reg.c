#include "ir.h"
#include "u_bits.h"
#include <stdlib.h> /* qsort */

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
   imap_of(Ref *) curdefs; /* map of var to (map of block to def of var) */
   BitSet *sealed, /* set of sealed blocks */
          *marked; /* blocks marked, for 'Marker Algorithm' in the paper */
   int lastvisit;
   int nblk;
} SSABuilder;

static Ref readvar(SSABuilder *, int var, enum irclass, Block *);

static Ref
deltrivialphis(SSABuilder *sb, int var, Block *blk, Ref phiref)
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
   Ref **pcurdefs = imap_get(&sb->curdefs, var);
   assert (pcurdefs);
   for (int i = blk->id; i < sb->nblk; ++i) {
      if ((*pcurdefs)[i].bits == phiref.bits)
         (*pcurdefs)[i] = same;
   }

   /* stops infinite recursion and marks phi for removal */
   instrtab[phiref.i].op = Onop;

   /* recursively try to remove all phi users as they might have become trivial */
Redo:
   for (IRUse *use = instruse[phiref.i]; use; use = use->next) {
      if (use->u != USERJUMP && instrtab[use->u].op == Ophi && use->u != phiref.i) {
         Ref it = mkref(RTMP, use->u);
         Ref vphi2 = deltrivialphis(sb, var, use->blk, it);
         if (vphi2.bits != it.bits) {
            same = vphi2;
            /* deletion happened so phiref use may have changed */
            goto Redo;
         }
      }
   }
   deluses(phiref.i);

   return same;
}

static Ref
addphiargs(SSABuilder *sb, int var, enum irclass cls, Block *blk, Ref phiref)
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
writevar(SSABuilder *sb, int var, Block *blk, Ref val)
{
   Ref **pcurdefs;
   if (!(pcurdefs = imap_get(&sb->curdefs, var))) {
      pcurdefs = imap_set(&sb->curdefs, var, allocz(sb->arena, sb->nblk * sizeof(Ref), 0));
   }
   if (val.t == RTMP) assert(instrtab[val.i].op != Onop);
   (*pcurdefs)[blk->id] = val;
}

enum { RPENDINGPHI = 7 };
static Ref
readvarrec(SSABuilder *sb, int var, enum irclass cls, Block *blk)
{
   Ref val;
   assert(blk->npred > 0);
   if (!bstest(sb->sealed, blk->id)) { /* unsealed block */
      /* add pending phi */
      val = insertphi(blk, cls);
      instrtab[val.i].r = mkref(RPENDINGPHI, var);
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
            Ref **pcurdefs = imap_get(&sb->curdefs, var);
            /* must have called writevar */
            assert(*pcurdefs && (*pcurdefs)[blk->id].bits);
            return (*pcurdefs)[blk->id];
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
readvar(SSABuilder *sb, int var, enum irclass cls, Block *blk)
{
   Ref **pcurdefs;
   if ((pcurdefs = imap_get(&sb->curdefs, var)) && (*pcurdefs)[blk->id].bits)
      return (*pcurdefs)[blk->id];
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
         addphiargs(sb, ins->r.i, ins->cls, blk, mkref(RTMP, blk->phi.p[i]));
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

static int
blkfindins(Block *blk, int ins)
{
   for (int i = 0; i < blk->phi.n; ++i)
      if (blk->phi.p[i] == ins) return -1;
   for (int i = 0; i < blk->ins.n; ++i)
      if (blk->ins.p[i] == ins) return i;
   assert(0 && "ins not in blk");
}

static int
rcmpuse(const void *a, const void *b)
{
   /* postorder sort */
   const IRUse *ua = *(IRUse **)a, *ub = *(IRUse **)b;
   Block *blk = ua->blk;
   if (ua->blk != ub->blk) return ub->blk->id - ua->blk->id;
   assert(ua->u != USERJUMP && ub->u != USERJUMP);
   return blkfindins(blk, ub->u) - blkfindins(blk, ua->u);
}

void
mem2reg(Function *fn)
{
   SSABuilder sb = { fn->passarena, .nblk = fn->nblk };

   FREQUIRE(FNUSE);

   sb.sealed = allocz(sb.arena, BSSIZE(fn->nblk) * sizeof *sb.sealed, 0);
   sb.marked = allocz(sb.arena, BSSIZE(fn->nblk) * sizeof *sb.sealed, 0);
   sortrpo(fn);

   Block *blk = fn->entry;
   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         enum irclass k = 0;
         int sz = 0;
         enum op ext = Ocopy;
         int var = blk->ins.p[i];
         Instr *ins = &instrtab[var];
         IRUse *use;
         
         /* find allocas only used in loads/stores of uniform size */
         if (!oisalloca(ins->op) || !(use = instruse[var])) continue;
         IRUse *usesbuf[64];
         vec_of(IRUse *) uses = VINIT(usesbuf, countof(usesbuf));
         do {
            if (use->u == USERJUMP) goto Skip;
            Instr *m = &instrtab[use->u];
            if (use->blk->id < sb.lastvisit) {
               /* alloca appears after some of its uses; happens rarely, only
                * generated with code with gotos into loops. breaks this algo
                * due to reprocessing sealed blocks */
               goto Skip;
            }
            if (oisload(m->op) && (!sz || sz == loadsz(m->op))) {
               sz = loadsz(m->op);
               k = loadcls(m->op);
               if (sz < 4) ext = load2ext(m->op);
            } else if (oisstore(m->op) && m->l.bits == mkref(RTMP, var).bits
              && (!sz || sz == storesz(m->op))) {
               sz = storesz(m->op);
            } else goto Skip;
            vpush(&uses, use);
         } while ((use = use->next));
         
         /* visit uses in rpo */
         qsort(uses.p, uses.n, sizeof *uses.p, rcmpuse);
         for (int i = uses.n-1; i >= 0; --i) {
            use = uses.p[i];
            ins = &instrtab[use->u];

            if (oisstore(ins->op)) {
               writevar(&sb, var, use->blk, ins->r);
               *ins = mkinstr0(Onop,0);
            } else if (oisload(ins->op)) {
               Ref val = readvar(&sb, var, k = ins->cls, use->blk);
               adduse(use->blk, use->u, val);
               if (ext != Ocopy && isintcon(val)) { /* fold constant int extension */
                  val = irunop(NULL, ext, k, val);
                  assert(val.bits);
                  ext = Ocopy;
               }
               *ins = mkinstr1(ext, k, val);
            }
         }
         /* remove alloca */
         delinstr(blk, i--);
         
      Skip:
         vfree(&uses);
      }

      assert(sb.lastvisit == blk->id);
      ++sb.lastvisit;
      tryseal(&sb, blk);
   } while ((blk = blk->lnext) != fn->entry);

   do {
      /* remove phis marked for deletion */
      for (int i = 0; i < blk->phi.n; ++i)
         if (instrtab[blk->phi.p[i]].op == Onop)
            delphi(blk, i--);
   } while ((blk = blk->lnext) != fn->entry);

   imap_free(&sb.curdefs);
   if (ccopt.dbg.m) {
      bfmt(ccopt.dbgout, "<< After mem2reg >>\n");
      irdump(fn);
   }
}


/* vim:set ts=3 sw=3 expandtab: */
