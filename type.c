#include "common.h"

struct typedata typedata[1<<13];
const char *ttypenames[1<<10];

static ushort
hashtd(const struct typedata *td)
{
   uint h = td->t*33;
   bool t;
   switch (td->t) {
   case TYARRAY:
      h = hashb(h, &td->arrlen, sizeof td->arrlen);
      /* fallthru */
   case TYPTR:
      h = hashb(h, &td->child, sizeof td->child);
      break;
   case TYSTRUCT:
   case TYUNION:
   case TYENUM:
      h = hashb(h, &td->id, sizeof td->id);
      break;
   case TYFUNC:
      h = hashb(h, &td->ret, sizeof td->ret);
      h = hashb(h, &td->nmemb, sizeof td->nmemb);
      h = hashb(h, (t = td->kandr, &t), sizeof t);
      h = hashb(h, (t = td->variadic, &t), sizeof t);
      for (int i = 0; i < td->nmemb; ++i) {
         uchar q = tdgetqual(td->quals, i);
         h = hashb(h, &td->param[i], sizeof *td->param);
         h = hashb(h, &q, sizeof q);
      }
      break;
   default:
      assert(0 && "bad typedata tag");
   }
   return h ^ h>>16;
}

static bool
tdequ(const struct typedata *a, const struct typedata *b)
{
   if (a->t != b->t) return 0;
   switch (a->t) {
   case TYARRAY:
      return a->arrlen == b->arrlen && a->child.bits == b->child.bits;
   case TYPTR:
      return a->child.bits == b->child.bits;
   case TYSTRUCT:
   case TYUNION:
   case TYENUM:
      return a->id == b->id;
   case TYFUNC:
      if (a->ret.bits != b->ret.bits) return 0;
      if (a->nmemb != b->nmemb) return 0;
      if (a->variadic != b->variadic) return 0;
      if (a->kandr != b->kandr) return 0;
      for (int i = 0; i < a->nmemb; ++i) {
         if (!a->quals != !b->quals || a->param[i].bits != b->param[i].bits)
            return 0;
         if (a->quals && tdgetqual(a->quals, i) != tdgetqual(b->quals, i))
            return 0;
      }
      return 1;
      break;
   default:
      assert(0 && "bad typedata tag");
   }
}

static ushort
interntd(const struct typedata *td)
{
   uint h, i, n = arraylength(typedata);
   for (i = h = hashtd(td); n--; ++i) {
      struct typedata *slot = &typedata[i &= arraylength(typedata) - 1];
      if (!slot->t) {
         uint nmemb;
         static struct arena *datarena;
         if (!datarena) {
            enum { N = 1<<12 };
            static union { char m[sizeof(struct arena) + N]; struct arena *_align; } amem;
            datarena = (void *)amem.m, datarena->cap = N;
         }

      Copy:
         nmemb = td->nmemb;
         *slot = *td;
         switch (slot->t) {
         case TYENUM:
            if (slot->var)
               slot->var = alloccopy(&datarena, td->var, nmemb * sizeof *slot->var, 0);
            break;
         case TYSTRUCT:
         case TYUNION:
            if (slot->fld)
               slot->fld = alloccopy(&datarena, td->fld, nmemb * sizeof *slot->fld, 0);
            break;
         case TYFUNC:
            if (slot->param)
               slot->param = alloccopy(&datarena, td->param, nmemb * sizeof *slot->param, 0);
            if (slot->quals)
               slot->quals = alloccopy(&datarena, td->quals, tdqualsiz(nmemb), 1);
         }
         return i;
      } else if (tdequ(slot, td)) {
         if (td->t == TYSTRUCT || td->t == TYUNION)
            goto Copy;
         return i;
      }
   }
   assert(0 && "typedata[] full");
}

bool
isincomplete(union type t)
{
   switch (t.t) {
   case TYVOID: return 1;
   case TYARRAY: return !typearrlen(t);
   case TYSTRUCT:
   case TYUNION:
      return typedata[t.dat].nmemb == 0;
   }
   return 0;
}

uint
typesize(union type t)
{
   if (isprim(t) || t.t == TYPTR) return targ_primsizes[t.t];
   switch (t.t) {
   case TYENUM:
      return targ_primsizes[t.backing];
   case TYARRAY:
      if (t.flag & TFCHLDPRIM)
         return targ_primsizes[t.child] * t.arrlen;
      /* fallthru */
   case TYSTRUCT:
   case TYUNION:
      return typedata[t.dat].siz;
   }
   return 0;
}

uint
typealign(union type t)
{
   if (isprim(t) || t.t == TYPTR) return targ_primalign[t.t];
   switch (t.t) {
   case TYENUM:
      return targ_primalign[t.backing];
   case TYARRAY:
      return typealign(typechild(t));
   case TYSTRUCT:
   case TYUNION:
      return typedata[t.dat].align;
   }
   return 0;
}

union type
mkptrtype(union type t, int qual)
{
   if (isprim(t))
      return mktype(TYPTR, .flag = TFCHLDPRIM | (qual & TFCHLDQUAL), .child = t.t);
   else if (t.t == TYENUM || t.t == TYFUNC || isagg(t))
      return mktype(TYPTR, .flag = TFCHLDISDAT | (qual & TFCHLDQUAL), .dat = t.dat);
   return mktype(TYPTR, .flag = qual & TFCHLDQUAL,
                 .dat = interntd(&(struct typedata) { TYPTR, .child = t }));
}

union type
mkarrtype(union type t, int qual, uint n)
{
   if (isprim(t) && n < 256)
      return mktype(TYARRAY, .flag = TFCHLDPRIM | (qual & TFCHLDQUAL), .child = t.t, .arrlen = n);
   return mktype(TYARRAY, .flag = qual & TFCHLDQUAL,
                 .dat = interntd(&(struct typedata) { TYARRAY, .child = t, .arrlen = n, .siz = n * typesize(t) }));
}

union type
mkfntype(union type ret, uint n, const union type *par, const uchar *qual, bool kandr, bool variadic)
{
   struct typedata td = { TYFUNC, .ret = ret, .nmemb = n, .param = par, .quals = qual };
   td.kandr = kandr, td.variadic = variadic;
   return mktype(TYFUNC, .dat = interntd(&td));
}


union type
completetype(const char *name, int id, struct typedata *td)
{
   assert(td->t == TYENUM || td->t == TYSTRUCT || td->t == TYUNION);
   td->id = id;
   assert(id < arraylength(ttypenames) && "too many tag types");
   if (ttypenames[id])
      assert(ttypenames[id] == name && "bad redefn");
   else
      ttypenames[id] = name;
   return mktype(td->t, .dat = interntd(td));
}

union type
mktagtype(const char *name, struct typedata *td)
{
   static int id;
   return completetype(name, id++, td);
}

static bool
getfieldrec(struct fielddata *res, uint off, const struct typedata *td, const char *name)
{
Begin:
   for (int i = 0; i < td->nmemb; ++i) {
      struct namedfield *fld = &td->fld[i];
      if (fld->name == name) { /* match */
         *res = fld->f;
         res->off += off;
         return 1;
      } else if (!fld->name) { /* anonymous struct/union */
         const struct typedata *ftd = &typedata[fld->f.t.dat];
         assert(isagg(fld->f.t));
         if (i == td->nmemb - 1) { /* last field, tail recurse */
            off += fld->f.off;
            td = ftd;
            goto Begin;
         } else if (getfieldrec(res, off + fld->f.off, ftd, name))
            return 1;
      }
   }
   return 0;
}

bool
getfield(struct fielddata *res, union type ty, const char *name)
{
   assert(isagg(ty));
   return getfieldrec(res, 0, &typedata[ty.dat], name);
}

union type
typedecay(union type t)
{
   if (t.t == TYARRAY)
      return mkptrtype(typechild(t), t.flag & TFCHLDQUAL);
   if (t.t == TYFUNC)
      return mkptrtype(t, 0);
   return t;
}

bool /* 6.5.16.1 Simple assignment Constraints */
assigncompat(union type dst, union type src)
{
   union type ds, ss;
   if (dst.bits == src.bits) return 1;
   if (isarith(dst) && isarith(src)) return 1;
   if (dst.t == TYPTR && src.t == TYPTR) {
      ds = typechild(dst);
      ss = typechild(src);
      if (ds.bits == ss.bits || ss.t == TYVOID || ds.t == TYVOID)
         return ((dst.flag & TFCHLDQUAL) & (src.flag & TFCHLDQUAL)) == (src.flag & TFCHLDQUAL);
   }
   if (dst.t == TYBOOL && src.t == TYPTR)
      return 1;
   return 0;
}

enum typetag
intpromote(enum typetag t)
{
   static int intisshort = -1;
   if (intisshort < 0) intisshort = targ_primsizes[TYINT] == targ_primsizes[TYSHORT];
   if (intisshort && t == TYUSHORT) return TYUINT;
   return t < TYINT ? TYINT : t;
}

union type /* 6.3.1.8 Usual arithmetic conversions */
cvtarith(union type a, union type b)
{
   const union type none = {0};

   if (!isarith(a) || !isarith(b)) return none;
   if (a.t == TYENUM) a = typechild(a);
   if (b.t == TYENUM) b = typechild(b);
   if (isflt(a) || isflt(b)) {
      /* when one type is float, choose type with greatest rank */
      /* enumeration order of type tags reflects arithmetic type rank */
      return a.t > b.t ? a : b;
   }
   a.t = intpromote(a.t);
   b.t = intpromote(b.t);
   if (a.bits == b.bits) return a;

   if (issigned(a) == issigned(b)) {
      /* when both are integers with same signage, choose type with greatest rank */
      return a.t > b.t ? a : b;
   }
   /* if the signed type can represent all values of the unsigned type,
    * choose it, otherwise choose its corresponding unsigned type */
   /* so long long + unsigned = long long;
    * but long long + unsigned long = unsigned long long */
   if (issigned(a)) {
      if (targ_primsizes[a.t] <= targ_primsizes[b.t])
         a.t += 1; /* make unsigned */
      return a.t > b.t ? a : b;
   } else {
      if (targ_primsizes[b.t] <= targ_primsizes[a.t])
         b.t += 1; /* make unsigned */
      return b.t > a.t ? b : a;
   }
}

/* vim:set ts=3 sw=3 expandtab: */
