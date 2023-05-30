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

static void
dumpref(enum op o, union ref ref)
{
   struct xcon *con;
   switch (ref.t) {
   case RTMP: efmt("%%%d", ref.idx); break;
   case RARG: efmt("%%arg%d", ref.idx); break;
   case RICON: efmt("%d", ref.i); break;
   case RXCON:
      con = &conht[ref.idx];
      if (con->issym) efmt("$%s", con->sym);
      else switch (con->cls) {
      case KI4: efmt("%d", con->i4); break;
      case KI8: efmt("%ld", con->i8); break;
      case KPTR: efmt("%'x", con->i8); break;
      case KF4: efmt("%f", con->fs); break;
      case KF8: efmt("%f", con->fd); break;
      default: assert(0);
      }
      break;
   case REXT:
      if (o == Ocall) {
         extern vec_of(struct call) calls;
         struct call *call = &calls.p[ref.idx];
         for (int i = 0; i < call->narg; ++i) {
            if (call->vararg == i) {
               if (i > 0) efmt(", ");
               efmt("..., ");
            }
            prityp(call->typs[i]);
            efmt(" ");
            dumpref(0, call->args[i]);
         }
      } else if (o == Ophi) {
         extern vec_of(struct phi) phis;
         struct phi *phi = &phis.p[ref.idx];
         for (int i = 0; i < phi->n; ++i) {
            if (i > 0) efmt(", ");
            efmt("[.L%d ", phi->blk[i]->id);
            dumpref(0, phi->ref[i]);
            efmt("]");
         }
      }
      break;
   default: assert(!"ref");
   }
}

extern struct instr instr[];
static const char *opname[] = {
   "???",
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
   if (ins->cls)
      efmt("%s %%%d = ", clsname[ins->cls], ins - instr);
   efmt("%s ", opname[ins->op]);
   for (i = 0; i < opnarg[ins->op]; ++i) {
      if (i) efmt(", ");
      dumpref(ins->op, (&ins->l)[i]);
   }
   efmt("\n");
}

static void
dumpblk(struct block *blk)
{
   static const char *jnames[] = { 0, "b", "b", "ret", "rets" };
   static const uchar jnarg[] = { 0, 0, 1, 0, 1 };
   efmt("  .L%d:\n", blk->id);
   for (int i = 0; i < blk->phi.n; ++i) {
      dumpinst(&instr[blk->phi.p[i]]);
   }
   for (int i = 0; i < blk->ins.n; ++i) {
      dumpinst(&instr[blk->ins.p[i]]);
   }
   efmt("    %s ", jnames[blk->jmp.t]);
   if (jnarg[blk->jmp.t]) {
      dumpref(0, blk->jmp.arg);
      if (blk->s1) efmt(", ");
   }
   if (blk->s1 && blk->s2) efmt(".L%d, .L%d", blk->s1->id, blk->s2->id);
   else if (blk->s1) efmt(".L%d", blk->s1->id);
   efmt("\n");
}

void
irdump(struct function *fn, const char *fname)
{
   struct block *blk;

   efmt("function %s : %ty\n", fname, fn->fnty);
   blk = fn->entry;
   do {
      dumpblk(blk);
      blk = blk->lnext;
   } while (blk != fn->entry);
}

/* vim:set ts=3 sw=3 expandtab: */
