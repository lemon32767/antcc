#include "common.h"
#include "ir.h"

extern struct xcon conht[];

static const char *clsname[] = {
   "?", "i4", "i8", "ptr", "f4", "f8"
};

static void
prityp(union irtype typ)
{
   if (!typ.isagg)
      efmt(clsname[typ.cls]);
   else {
      const struct typedata *td = &typedata[typ.dat];
      const char *tag = td->t == TYSTRUCT ? "struct" : "union";
      if (ttypenames[td->id])
         efmt("%s.%s.%d", tag, ttypenames[td->id], td->id);
      else
         efmt("%s.%d", tag, td->id);
   }
}

static const char *intrinname[] = {
   "?\??",
#define _(b,...) #b,
#include "intrin.def"
#undef _
};

static void
dumpref(enum op o, union ref ref)
{
   struct xcon *con;
   switch (ref.t) {
   case RTMP: efmt("%%%d", ref.idx); break;
   case RPARAM: efmt("%%param%d", ref.idx); break;
   case RICON:
      if (o == Ointrin) efmt("\"%s\"", intrinname[ref.i]);
      else efmt("%d", ref.i);
      break;
   case RXCON:
      con = &conht[ref.idx];
      if (con->issym) efmt("$%s", con->sym);
      else switch (con->cls) {
      case KI4: efmt("%d", con->i4); break;
      case KI8: efmt("%ld", con->i8); break;
      case KPTR: efmt("%'x", con->i8); break;
      case KF4: efmt("%fs", con->fs); break;
      case KF8: efmt("%fd", con->fd); break;
      default: assert(0);
      }
      break;
   case RMORE:
      if (o == Ocall || o == Ointrin) {
         struct call *call = &calltab.p[ref.idx];
         if (call->sret) {
            efmt("sret ");
            prityp(call->typs[call->narg]);
         }
         for (int i = 0; i < call->narg; ++i) {
            if (i > 0 || call->sret) efmt(", ");
            if (call->vararg == i)
               efmt("..., ");
            prityp(call->typs[i]);
            efmt(" ");
            dumpref(0, call->args[i]);
         }
      } else if (o == Ophi) {
         struct phi *phi = &phitab.p[ref.idx];
         for (int i = 0; i < phi->n; ++i) {
            if (i > 0) efmt(", ");
            efmt("[.L%d ", phi->blk[i]->id);
            dumpref(0, phi->ref[i]);
            efmt("]");
         }
      } else assert(0);
      break;
   default: assert(!"ref");
   }
}

static const char *opname[] = {
   "?\??",
#define _(o,...) #o,
#include "op.def"
#undef _
};
static const uchar opnarg[] = {
   0,
#define _(o,n) n,
#include "op.def"
#undef _
};

static void
dumpinst(const struct instr *ins)
{
   int i;
   efmt("    ");
   if (ins->cls) {
      efmt("%s %%%d", clsname[ins->cls], ins - instrtab);
      if (ins->reg) efmt("(%ls)", mctarg->rnames[ins->reg - 1]);
      efmt(" = ");
   }
   efmt("%s ", opname[ins->op]);
   for (i = 0; i < opnarg[ins->op]; ++i) {
      if (i) efmt(", ");
      dumpref(ins->op, (&ins->l)[i]);
   }
   efmt("\n");
}

static void
dumpblk(struct function *fn, struct block *blk)
{
   static const char *jnames[] = { 0, "b", "ret" };
   int i;
   efmt("  .L%d:\n", blk->id);
   for (i = 0; i < blk->phi.n; ++i) {
      dumpinst(&instrtab[blk->phi.p[i]]);
   }
   for (i = 0; i < blk->ins.n; ++i) {
      dumpinst(&instrtab[blk->ins.p[i]]);
   }
   efmt("    %s ", jnames[blk->jmp.t]);
   for (i = 0; i < 2; ++i) {
      if (!blk->jmp.arg[i].t) break;
      if (i > 0) efmt(", ");
      if (blk->jmp.t == Jret && fn->nabiret > i) {
         prityp(fn->abiret[i].ty);
         efmt(" ");
      }
      dumpref(0, blk->jmp.arg[i]);
   }
   if (i && blk->s1) efmt(", ");
   if (blk->s1 && blk->s2) efmt(".L%d, .L%d", blk->s1->id, blk->s2->id);
   else if (blk->s1) efmt(".L%d", blk->s1->id);
   efmt("\n");
}

void
irdump(struct function *fn, const char *fname)
{
   struct block *blk;

   efmt("function %s : %ty\n", fname, fn->fnty);
   if (fn->abiarg || fn->retty.t != TYVOID) {
      efmt("abi: (");
      for (int i = 0; i < fn->nabiarg; ++i) {
         if (i > 0) efmt(", ");
         if (fn->abiarg[i].reg != -1) {
            efmt("%ls", mctarg->rnames[fn->abiarg[i].reg]);
         } else {
            prityp(fn->abiarg[i].ty);
            efmt(" <stk>");
         }
      }
      efmt(")");
      if (fn->retty.t != TYVOID) {
         efmt(" -> %ls", mctarg->rnames[fn->abiret[0].reg]);
         if (fn->nabiret > 1)
            efmt(", %ls", mctarg->rnames[fn->abiret[1].reg]);
      }
      efmt("\n");
   }
   blk = fn->entry;
   do {
      dumpblk(fn, blk);
   } while ((blk = blk->lnext) != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
