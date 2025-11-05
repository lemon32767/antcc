#include "ir.h"

static const uchar loadszcls[] = {
   [Oloads1 - Oloads1] = 1|KI4<<4, [Oloadu1 - Oloads1] = 1|KI4<<4,
   [Oloads2 - Oloads1] = 2|KI4<<4, [Oloadu2 - Oloads1] = 2|KI4<<4,
   [Oloads4 - Oloads1] = 4|KI4<<4, [Oloadu4 - Oloads1] = 4|KI4<<4,
   [Oloadi8 - Oloads1] = 8|KI8<<4,
   [Oloadf4 - Oloads1] = 4|KF4<<4,
   [Oloadf8 - Oloads1] = 8|KF8<<4,
};
static const uchar load2ext[] = {
   [Oloads1 - Oloads1] = Oexts1, [Oloadu1 - Oloads1] = Oextu1,
   [Oloads2 - Oloads1] = Oexts2, [Oloadu2 - Oloads1] = Oextu2,
   [Oloads4 - Oloads1] = Oexts4, [Oloadu4 - Oloads1] = Oextu4,
   [Oloadi8 - Oloads1] = Ocopy,
};
#define loadsz(o) (loadszcls[(o) - Oloads1] & 0xF)
#define loadcls(o) (loadszcls[(o) - Oloads1] >> 4)
#define load2ext(o) (load2ext[(o) - Oloads1])
#define storesz(o) (1 << ((o) - Ostore1))

/* Implements algorithm in 'Simple and Efficient Construction of Static Single Assignment' (Braun et al) */

struct pendingphi { ushort var, phi; };
struct ssabuilder {
   vec_of(struct pendingphi) *pendingphis; /* map of block to list of incomplete phis in block */
   imap_of(union ref *) curdefs; /* map of var to (map of block to def of var) */
   struct bitset *sealed;
   int lastvisit;
   int nblk;
};

static union ref readvar(struct ssabuilder *, int var, enum irclass, struct block *);

static union ref
deltrivialphis(struct ssabuilder *sb, struct block *blk, union ref phiref)
{
   union ref **pcurdefs;
   int var;
   struct use *use, *uend;
   union ref *args = phitab.p[instrtab[phiref.i].l.i];
   union ref same = {0};

   assert(instrtab[phiref.i].op == Ophi);

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
   imap_each(&sb->curdefs, var, pcurdefs) {
      (void)var;
      for (int i = blk->id; i < sb->nblk; ++i) {
         if ((*pcurdefs)[i].bits == phiref.bits)
            (*pcurdefs)[i] = same;
      }
   }

   /* stops infinite recursion and marks phi for removal */
   instrtab[phiref.i].op = Onop;

   /* recursively try to remove all phi users as they might have become trivial */
Redo:
   for (use = instruse[phiref.i], uend = use + instrnuse[phiref.i]; use < uend; ++use) {
      if (use->u != USERJUMP && instrtab[use->u].op == Ophi && use->u != phiref.i) {
         union ref it = mkref(RTMP, use->u);
         union ref vphi2 = deltrivialphis(sb, use->blk, it);
         if (vphi2.bits != it.bits) {
            /* deletion happened so phiref use may have changed */
            if (same.bits == it.bits) same.bits = vphi2.bits;
            goto Redo;
         }
      }
   }
   deluses(phiref.i);

   return same;
}

static union ref
addphiargs(struct ssabuilder *sb, int var, enum irclass cls, struct block *blk, union ref phiref)
{
   union ref *args = phitab.p[instrtab[phiref.i].l.i];
   for (int i = 0; i < blk->npred; ++i) {
      struct block *pred = blkpred(blk, i);
      args[i] = readvar(sb, var, cls, pred);
      adduse(blk, phiref.i, args[i]);
   }
   return deltrivialphis(sb, blk, phiref);
}

static void
writevar(struct ssabuilder *sb, int var, struct block *blk, union ref val)
{
   union ref **pcurdefs;
   if (!(pcurdefs = imap_get(&sb->curdefs, var))) {
      pcurdefs = imap_set(&sb->curdefs, var, xcalloc(sb->nblk * sizeof(union ref)));
   }
   if (val.t == RTMP) assert(instrtab[val.i].op != Onop);
   (*pcurdefs)[blk->id] = val;
}

static union ref
readvarrec(struct ssabuilder *sb, int var, enum irclass cls, struct block *blk)
{
   union ref val;
   assert(blk->npred > 0);
   if (!bstest(sb->sealed, blk->id)) { /* unsealed block */
      val = insertphi(blk, cls);
      vpush(&sb->pendingphis[blk->id], ((struct pendingphi) { var, val.i }));
   } else if (blk->npred == 1) {
      val = readvar(sb, var, cls, blkpred(blk, 0));
   } else {
      val = insertphi(blk, cls);
      writevar(sb, var, blk, val);
      val = addphiargs(sb, var, cls, blk, val);
   }
   writevar(sb, var, blk, val);
   return val;
}

static union ref
readvar(struct ssabuilder *sb, int var, enum irclass cls, struct block *blk)
{
   union ref **pcurdefs;
   if ((pcurdefs = imap_get(&sb->curdefs, var)) && (*pcurdefs)[blk->id].bits)
      return (*pcurdefs)[blk->id];
   if (blk->npred == 0) /* entry block, var is read before being written to */
      return UNDREF;
   return readvarrec(sb, var, cls, blk);
}

static bool
trysealrec(struct ssabuilder *sb, struct block *blk)
{
   vec_of(struct pendingphi) *pending;

Recur:
   if (bstest(sb->sealed, blk->id)) return 1;
   if (blk->id > sb->lastvisit) return 0;
   markvisited(blk);
   for (int i = 0; i < blk->npred; ++i) {
      struct block *p = blkpred(blk, i);
      if (wasvisited(p)) continue;
      if (p->id > sb->lastvisit) return 0;
   }

   bsset(sb->sealed, blk->id);
   pending = (void *) &sb->pendingphis[blk->id];
   for (int i = 0; i < pending->n; ++i) {
      addphiargs(sb, pending->p[i].var, instrtab[pending->p[i].phi].cls, blk,
                 mkref(RTMP, pending->p[i].phi));
   }
   vfree(pending);
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
tryseal(struct ssabuilder *sb, struct block *blk)
{
   startbbvisit();
   trysealrec(sb, blk);
}

/* require use, blkid; keeps use */
void
mem2reg(struct function *fn)
{
   static struct bitset ssealed[4];
   struct ssabuilder sb = { .nblk = fn->nblk };
   struct block *blk;

   sb.pendingphis = xcalloc(fn->nblk * sizeof *sb.pendingphis);
   if (fn->nblk <= 64 * arraylength(ssealed)) {
      sb.sealed = ssealed;
      memset(ssealed, 0, BSSIZE(fn->nblk) * sizeof *ssealed);
   } else {
      sb.sealed = xcalloc(BSSIZE(fn->nblk) * sizeof *sb.sealed);
   }

   sortrpo(fn);
   blk = fn->entry;

   do {
      for (int i = 0; i < blk->ins.n; ++i) {
         struct use *use, *uend;
         enum irclass k = 0;
         int sz = 0, ndef = 0;
         enum op ext = Ocopy;
         int var = blk->ins.p[i];
         struct instr *ins = &instrtab[var];
         
         if (!oisalloca(ins->op)) continue;
         uend = instruse[var] + instrnuse[var];
         for (use = instruse[var]; use < uend; ++use) {
            struct instr *m;
            
            if (use->u == USERJUMP) goto Next;
            m = &instrtab[use->u];
            if (oisload(m->op) && (!sz || sz == loadsz(m->op))) {
               sz = loadsz(m->op);
               k = loadcls(m->op);
               if (sz < 4) ext = load2ext(m->op);
               continue;
            }
            if (oisstore(m->op) && m->l.bits == mkref(RTMP, var).bits
            && (!sz || sz == storesz(m->op))) {
               sz = storesz(m->op);
               ++ndef;
               continue;
            }
            goto Next;
         }
         if (!ndef) /* slot is read from but never written to */
            goto Next;
         
         for (use = instruse[var]; use < uend; ++use) {
            struct instr *m = &instrtab[use->u];

            if (oisstore(m->op)) {
               writevar(&sb, var, use->blk, m->r);
               *m = mkinstr(Onop,0,);
            } else if (oisload(m->op)) {
               union ref val = readvar(&sb, var, k = m->cls, use->blk);
               if (!val.bits) { /* var is used uninitialized */
                  /* TODO emit diagnostic */
                  /* load some garbage */
                  *m = mkinstr(kisflt(k) ? Oloadf4 + (k==KF8) : Oloads1+ilog2(sz)*2,
                               k, mkref(RREG, mctarg->bpr));
               } else {
                  adduse(use->blk, use->u, val);
                  if (isintcon(val) && ext != Ocopy) {
                     vlong x = intconval(val);
                     switch (ext) {
                     case Oexts1: x = (schar)x; break;
                     case Oextu1: x = (uchar)x; break;
                     case Oexts2: x = (short)x; break;
                     case Oextu2: x = (ushort)x; break;
                     case Oexts4: x = (int)x; break;
                     case Oextu4: x = (uint)x; break;
                     default: assert(0);
                     }
                     val = mkintcon(k, x);
                     ext = Ocopy;
                  }
                  *m = mkinstr(ext, k, val);
               }
            }
         }
         /* remove alloca */
         delinstr(blk, i--);
         
      Next:;
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

   if (sb.sealed != ssealed) free(sb.sealed);
   free(sb.pendingphis);

   for (int i = 0; i < sb.curdefs.mb.N; ++i)
      if (bstest(sb.curdefs.mb.bs, i))
         free(sb.curdefs.v[i]);
   imap_free(&sb.curdefs);
   if (ccopt.dbg.m) {
      efmt("<< After mem2reg >>\n");
      irdump(fn);
   }
}


/* vim:set ts=3 sw=3 expandtab: */
