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

/* Implements algorithm in 'Simple and Efficient Construction of Static Single' (Braun et al) */

struct pendingphi { ushort var, phi; };
struct ssabuilder {
   vec_of(struct pendingphi) *pendingphis; /* map of block to list of incomplete phis in block */
   imap_of(union ref *) curdefs; /* map of var to (map of block to def of var) */
   int nsealed; /* num of sealed blocks */
   int nblk;
};

static union ref readvar(struct ssabuilder *, int var, enum irclass, struct block *);

static union ref
deltrivialphis(struct block *blk, union ref phiref)
{
   struct use *use, *uend;
   union ref *args = phitab.p[instrtab[phiref.i].l.i];
   union ref same = {0};
   for (int i = 0; i < blk->npred; ++i) {
      if (args[i].bits == same.bits || args[i].bits == phiref.bits)
         continue; /* unique value or self-reference */
      if (same.bits != 0) {
         return phiref; /* non-trivial */
      }
      same = args[i];
   }
   if (same.bits == 0)
      same = UNDREF; /* the phi is unreachable or in the start block */

   /* replace uses */
   replcuses(phiref, same);

   instrtab[phiref.i].op = Onop;

   /* recursively try to remove all phi users as they might have become trivial */
   for (use = instruse[phiref.i], uend = use + instrnuse[phiref.i]; use < uend; ++use)
      if (use->u != USERJUMP && instrtab[use->u].op == Ophi)
         deltrivialphis(use->blk, mkref(RTMP, use->u));

   /* remove phi */
   for (int i = 0; i < blk->phi.n; ++i) {
      if (blk->phi.p[i] == phiref.i) {
         delphi(blk, i);
         break;
      }
   }
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
   return deltrivialphis(blk, phiref);
}

static void
writevar(struct ssabuilder *sb, int var, struct block *blk, union ref val)
{
   union ref **pcurdefs;
   if (!(pcurdefs = imap_get(&sb->curdefs, var))) {
      pcurdefs = imap_set(&sb->curdefs, var, xcalloc(sb->nblk * sizeof(union ref)));
   }
   (*pcurdefs)[blk->id] = val;
}

static union ref
readvarrec(struct ssabuilder *sb, int var, enum irclass cls, struct block *blk)
{
   union ref val;
   assert(blk->npred > 0);
   if (blk->id >= sb->nsealed) {
      /* unsealed block */
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

void
mem2reg(struct function *fn)
{
   struct block *blk = fn->entry;
   struct ssabuilder sb = { .nblk = fn->nblk };

   sb.pendingphis = xcalloc(fn->nblk * sizeof *sb.pendingphis);

   do {
      vec_of(struct pendingphi) *pending;

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
               union ref val = readvar(&sb, var, k, use->blk);
               if (!val.bits) { /* var is used uninitialized */
                  /* TODO emit diagnostic */
                  /* load some garbage */
                  *m = mkinstr(kisflt(k) ? Oloadf4 + (k==KF8) : Oloads1+ilog2(sz)*2,
                               k, mkref(RREG, mctarg->bpr));
               } else {
                  adduse(use->blk, use->u, val);
                  *m = mkinstr(ext, k, val);
               }
            }
         }
         /* remove alloca */
         delinstr(blk, i--);
         
      Next:;
      }

      /* seal block */
      assert(sb.nsealed == blk->id);
      ++sb.nsealed;
      pending = (void *) &sb.pendingphis[blk->id];
      for (int i = 0; i < pending->n; ++i) {
         addphiargs(&sb, pending->p[i].var, instrtab[pending->p[i].phi].cls, blk,
                    mkref(RTMP, pending->p[i].phi));
      }
      vfree(pending);
   } while ((blk = blk->lnext) != fn->entry);

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
