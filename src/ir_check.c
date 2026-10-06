#include "ir.h"
#include <stdlib.h>
#include <stdarg.h>

static struct {
   int nfail;
   const Instr *ins;
} checkstate;

static void
checkfail(const char *fmt, ...) {
   va_list ap;
   va_start(ap, fmt);
   efmt("CHECK FAIL: ");
   if (checkstate.ins && checkstate.ins->op)
      efmt("'%s' ", opnames[checkstate.ins->op]);
   vbfmt(&bstderr, fmt, ap);
   va_end(ap);
   ++checkstate.nfail;
}

#define CHECK(x, ...) do if (!(x)) { \
   checkfail(__VA_ARGS__);           \
   ioputc(&bstderr, '\n');           \
} while (0)

static enum irclass
refcls(Ref r) {
   switch (r.t) {
   case RICON: return KI32;
   case RTMP: return insrescls(instrtab[r.i]);
   case RXCON:
      if (!contab.p[r.i].cls) /* addr */ return KPTR;
      return contab.p[r.i].cls;
   case RSTACK: return KPTR;
   case RADDR:
      if (!contab.p[r.i].deref)
         return KPTR;
   }
   return 0;
}


static void
priclsfail(enum irclass k, const Ref *r, int n) {
   checkfail("type error: had <%s; args: ", k ? clsname[k] : "void");
   assert(n > 0);
   while (n --> 0)
      efmt("%k%s", refcls(*r++), n ? ", " : ">\n");
}

static inline enum irclass
clsptrfilt(enum irclass k) {
   return k == KPTR ? KI32+targ_64bit : k;
}


#define clseql0(k, q) (clsptrfilt(k) == clsptrfilt(q))

static bool
clseql(enum irclass k, Ref r) {
   if (r.t == RICON && kisint(k)) return 1;
   if (r.bits == UNDREF.bits) return 1;
   return clseql0(k, refcls(r));
}

/* homogenous */
static void
checkhomo(enum irclass k, const Ref *r, int n) {
   bool ok = 1;
   assert(n > 0);
   for (int i = 0; i < n; ++i) {
      ok = ok && clseql(k, r[i]);
   }
   if (!ok) priclsfail(k, r, n);
}

/* Oext* */
static void
checkintext(enum irclass k, Ref r) {
   enum irclass refk = refcls(r);
   if (!kisint(k) || !kisint(refk) || refk > k)
      priclsfail(k, &r, 1);
}

/* int only */
static void
checkint(enum irclass k, const Ref *r, int n) {
   if (kisint(k))
      checkhomo(k, r, n);
   else
      priclsfail(k, r, n);
}

/* Ocvtf*u/s* */
static void
checkf2icvt(enum irclass from, enum irclass k, Ref r) {
   if (!kisint(k) || !clseql(from, r))
      priclsfail(k, &r, 1);
}

/* Ocvtu/s*f* */
static void
checki2fcvt(enum irclass from, enum irclass k, Ref r) {
   if (!kisflt(k) || !clseql(from, r))
      priclsfail(k, &r, 1);
}

static void
checkreg(Ref r) {
   CHECK(r.t == RREG, "expected register (got %r)", r);
}

/* Oload* */
static void
checkload(enum irclass retk, enum irclass wantk, Ref oper) {
   if (wantk == KI32 ? !kisint(retk) : !clseql0(retk, wantk) || !clseql(KPTR, oper))
      priclsfail(retk, &oper, 1);
}

/* Ostore* */
static void
checkstore(enum irclass retk, enum irclass wantk, const Ref *oper) {
   if (retk || !clseql(KPTR, oper[0]) || !clseql(wantk, oper[1]))
      priclsfail(retk, oper, 2);
}

static void
checkcall(enum irclass k, const IRCall *call) {
   if (!call->abiarg)  {
      enum irclass retk = call->ret.isagg ? KPTR : call->ret.cls;
      CHECK(clseql0(retk, k),
            "returns %r, instr had %k", mktyperef(call->ret), k);
   }
   /* TODO check actual args */
   CHECK(call->vararg == -1 || (uint)call->vararg <= call->narg,
         "vararg index %d", call->vararg);
}

static void
checkins(Function *fn, const Instr *ins) {
   enum irclass k = ins->cls;
   switch ((enum op)ins->op) {
   case Oxxx: case NOPER: assert(!"unreachable");
   case Onop: break;
   case Ocopy: break;
   case Oswap:
      checkreg(ins->r);
   case Omove:
      checkreg(ins->l);
      break;

   case Ocvtf32s: case Ocvtf32u: checkf2icvt(KF32, k, ins->l); break;
   case Ocvtf64s: case Ocvtf64u: checkf2icvt(KF64, k, ins->l); break;
   case Ocvts32f: case Ocvtu32f: checki2fcvt(KI32, k, ins->l); break;
   case Ocvts64f: case Ocvtu64f: checki2fcvt(KI64, k, ins->l); break;
   case Ocvtf32f64:
      if (k != KF64 || !clseql(KF32, ins->l))
         priclsfail(k, ins->oper, 1);
      break;
   case Ocvtf64f32:
      if (k != KF32 || !clseql(KF64, ins->l))
         priclsfail(k, ins->oper, 1);
      break;

   case Oneg: case Oadd: case Osub: case Omul: case Odiv:
   case Olth: case Ogth: case Olte: case Ogte: case Oequ: case Oneq:
   case Omsub:
      /* a, a -> a */
      checkhomo(k, ins->oper, opnoper[ins->op]);
      break;

   case Oexts8: case Oextu8: case Oexts16: case Oextu16: case Oexts32: case Oextu32:
      /* Int a -> Int b; rank b > rank a */
      checkintext(k, ins->l);
      break;

   case Onot: case Obswap16: case Obswap32: case Obswap64:
      /* Int a -> a */
   case Oudiv: case Orem: case Ourem: case Oand: case Oior: case Oxor:
   case Oulth: case Ougth: case Oulte: case Ougte:
      /* Int a, a -> a */
      checkint(k, ins->oper, opnoper[ins->op]);
      break;

   case Oshl: case Osar: case Oslr:
      /* Int a, Int b -> a */
      if (!kisint(k) || !clseql(k, ins->l) || !kisint(refcls(ins->r)))
         priclsfail(k, ins->oper, 2);
      break;

   case Oalloca1: case Oalloca2: case Oalloca4: case Oalloca8: case Oalloca16:
      if (k != KPTR || !kisint(refcls(ins->l)))
         priclsfail(k, ins->oper, 1);
      else if (!isintcon(ins->l))
         checkfail("expected int constant (%r)\n", ins->l);
      break;

   case Oloads8: case Oloadu8:
   case Oloads16: case Oloadu16:
   case Oloads32: case Oloadu32:
      checkload(k, KI32, ins->l);
      break;
   case Oloadi64: checkload(k, KI64, ins->l); break;
   case Oloadf32: checkload(k, KF32, ins->l); break;
   case Oloadf64: checkload(k, KF64, ins->l); break;

   case Ostorei8: case Ostorei16: case Ostorei32:
      checkstore(k, KI32, ins->oper);
      break;
   case Ostorei64: checkstore(k, KI64, ins->oper); break;
   case Ostoref32: checkstore(k, KF32, ins->oper); break;
   case Ostoref64: checkstore(k, KF64, ins->oper); break;

   case Oparam:
      if (fn->abiarg) {
         CHECK(ins->l.t == RICON && ins->l.i < fn->nabiarg,
               "no. %r", ins->l);
         ABIArg abi = fn->abiarg[ins->l.i];
         if (abi.isstk) {
            IRType rt = ref2type(ins->r);
            CHECK(ins->r.t == RTYPE && !rt.isagg && rt.cls == KPTR,
                  "type %r, expect ptr (to %r)", ins->r, mktyperef(abi.ty));
         } else {
            CHECK(ins->r.t == RTYPE && ins->r.i == abi.ty.bits,
                  "type %r (expect %r)", ins->r, mktyperef(abi.ty));
         }
      }
      break;

   case Ocall:
      if (!clseql(KPTR, ins->l))
         priclsfail(k, ins->oper, 1);
      assert(!ins->r.t);
      checkcall(k, &calltab.p[ins->r.i]);
      break;

   case Ointrin:
      if (ins->l.t != RICON)
         checkfail("intrin? %r\n", ins->l);
      assert(!ins->r.t);
      checkcall(k, &calltab.p[ins->r.i]);
      break;

   case Ocall2r:
      if (ins->l.t != RTMP || instrtab[ins->l.i].op != Ocall) {
         checkfail("expected to point to call op; got %r", ins->l);
         if (ins->l.t == RTMP)
            efmt(" (%s)", opnames[instrtab[ins->l.i].op]);
         efmt("\n");
      }
      break;

   case Oarg:
      if (ins->l.t != RTYPE) {
         checkfail("type? got %r\n", ins->l);
      } else {
         IRType irtyp = ref2type(ins->l);
         if (!clseql(irtyp.isagg ? KPTR : irtyp.cls, ins->r))
            checkfail("expected compatible with %r, got %k\n", ins->l, refcls(ins->r));
      }
      break;

   case Oxvaprologue:
      if (k || !clseql(KPTR, ins->l))
         priclsfail(k, ins->oper, 1);
      break;
   case Ovastart:
      if (k || !clseql(KPTR, ins->l))
         priclsfail(k, ins->oper, 1);
      break;
   case Ovaarg:
      if (!k || !clseql(KPTR, ins->l))
         priclsfail(k, ins->oper, 1);
      if (ins->r.t == RTYPE) {
         IRType irtyp = ref2type(ins->r);
         if (!clseql0(irtyp.isagg ? KPTR : irtyp.cls, k))
            checkfail("arg type %k != %r\n", k, ins->r);
      } else {
         checkfail("type? got %r\n", ins->l);
      }
      break;
   case Ophi: checkfail("unexpected phi instruction\n");
   }
}

#define WRAP_INSCHECK(_ins, ...) do { \
      int last = checkstate.nfail;    \
      checkstate.ins = _ins;          \
      __VA_ARGS__;                    \
      checkstate.ins = NULL;          \
      if (checkstate.nfail != last) { \
         efmt("  ... in: '");         \
         dumpinstr(&bstderr, ins);    \
         efmt("'\n");                 \
      }                               \
} while (0)


static void
checkblk(Function *fn, Block *b) {
   CHECK(b != fn->entry || b->npred == 0,
          "entry block @%d has %d predecessors", b->id, b->npred);
   CHECK(b->lnext->lprev == b && b->lprev->lnext == b,
          "block @%d has broken link list", b->id);
   CHECK(b->npred > 0 || b->phi.n == 0,
          "block @%d has phis but no predecessors", b->id);
   for (int i = 0; i < b->phi.n; ++i) {
      Instr *ins = &instrtab[b->phi.p[i]];
      WRAP_INSCHECK(ins,
         CHECK(ins->op == Ophi, "expected phi");
         checkhomo(ins->cls, phitab.p[ins->l.i], b->npred);
      );
   }
   for (int i = 0; i < b->ins.n; ++i) {
      Instr *ins = &instrtab[b->ins.p[i]];
      WRAP_INSCHECK(ins,
         checkins(fn, ins);
      );
   }
   checkstate.ins = NULL;
}

void
ircheck(Function *fn) {
   memset(&checkstate, 0, sizeof checkstate);
   int nblk = 0;
   Block *b = fn->entry;
   CHECK(b != NULL, "function %s has NULL entry", fn->name);
   do {
      checkblk(fn, b);
      ++nblk;
   } while ((b = b->lnext) != fn->entry);

   CHECK(fn->nblk == nblk,
          "function %s: expected %d blocks, counted %d", fn->name, fn->nblk, nblk);
   if (checkstate.nfail) {
      efmt(" ... %d fails while checking %'s\n", checkstate.nfail, fn->name);
      WriteBuf *dbgout = ccopt.dbg.out;
      ccopt.dbg.out = &bstderr;
      irdump(fn);
      ccopt.dbg.out = dbgout;
      abort();
   }
}

/* vim:set ts=3 sw=3 expandtab: */
