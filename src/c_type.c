#include "c_type.h"
#include "u_hash.h"

TypeData typedata[1<<13];
internstr tagtypetags[1<<10];
const char *const primtypenames[] = {
   [TYBOOL]     = "bool",
   [TYCHAR]     = "char",
   [TYSCHAR]    = "signed char",
   [TYUCHAR]    = "unsigned char",
   [TYSHORT]    = "short",
   [TYUSHORT]   = "unsigned short",
   [TYINT]      = "int",
   [TYUINT]     = "unsigned int",
   [TYLONG]     = "long",
   [TYULONG]    = "unsigned long",
   [TYVLONG]    = "long long",
   [TYUVLONG]   = "unsigned long long",
   [TYINT128]   = "__int128",
   [TYUINT128]  = "unsigned __int128",
   [TYFLOAT]    = "float",
   [TYDOUBLE]   = "double",
   [TYLDOUBLE]  = "long double",
   [TYCOMPLEXF] = "float complex",
   [TYCOMPLEX]  = "double complex",
   [TYCOMPLEXL] = "long double complex",
   [TYVOID]     = "void",
};

static ushort
hashtd(const TypeData *td) {
   uint h = td->t*33;
   bool t;
   switch (td->t) {
   case TYARRAY:
      if (td->flag & TFVLA) goto Id;
      h = hashb(h, &td->arrlen, sizeof td->arrlen);
      /* fallthru */
   case TYPTR:
      h = hashb(h, &td->child, sizeof td->child);
      break;
   case TYSTRUCT:
   case TYUNION:
   case TYENUM:
   Id:
      h = hashb(h, &td->id, sizeof td->id);
      break;
   case TYFUNC:
      h = hashb(h, &td->ret, sizeof td->ret);
      h = hashb(h, &td->nmemb, sizeof td->nmemb);
      h = hashb(h, (t = td->kandr, &t), sizeof t);
      h = hashb(h, (t = td->variadic, &t), sizeof t);
      for (int i = 0; i < td->nmemb; ++i) {
         h = hashb(h, &td->param[i], sizeof *td->param);
      }
      break;
   default:
      assert(0 && "bad typedata tag");
   }
   return h ^ h>>16;
}

static bool
tdequ(const TypeData *a, const TypeData *b) {
   if (a->t != b->t) return 0;
   switch (a->t) {
   case TYARRAY:
      if (a->flag & TFVLA) return (b->flag & TFVLA) && a->id == b->id;
      if (b->flag & TFVLA) return (a->flag & TFVLA) && a->id == b->id;
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
         if (a->param[i].bits != b->param[i].bits)
            return 0;
      }
      return 1;
   default:
      assert(0 && "bad typedata tag");
   }
}

static ushort
interntd(const TypeData *td) {
   uint h, i, n = countof(typedata);
   for (i = h = hashtd(td); n--; ++i) {
      TypeData *slot = &typedata[i &= countof(typedata) - 1];
      if (!slot->t) {
         uint nmemb;
         static Arena *datarena;
         if (!datarena) {
            enum { N = 1<<12 };
            static union { char m[sizeof(Arena) + N]; Arena *_align; } amem;
            datarena = (void *)amem.m, datarena->cap = N;
         }

      Copy:
         nmemb = td->nmemb;
         *slot = *td;
         switch (slot->t) {
         case TYENUM:
            assert(!(td->flag & TFUNKNOWN) || (!td->var && !td->siz));
            if (slot->var)
               slot->var = alloccopy(&datarena, td->var, nmemb * sizeof *slot->var, 0);
            break;
         case TYSTRUCT:
         case TYUNION:
            assert(!(td->flag & TFUNKNOWN) || (td->nmemb == 0 && !td->siz));
            if (slot->fld)
               slot->fld = alloccopy(&datarena, td->fld, nmemb * sizeof *slot->fld, 0);
            break;
         case TYFUNC:
            if (slot->param)
               slot->param = alloccopy(&datarena, td->param, nmemb * sizeof *slot->param, 0);
         }
         return i;
      } else if (tdequ(slot, td)) {
         if (td->t == TYSTRUCT || td->t == TYUNION || td->t == TYENUM)
            goto Copy;
         return i;
      }
   }
   assert(0 && "typedata[] full");
}

bool
isincomplete(Type t) {
   switch (t.t) {
   case TYVOID: return 1;
   case TYARRAY: return t.flag & TFUNKNOWN;
   case TYENUM:
   case TYSTRUCT:
   case TYUNION:
      return typedata[t.dat].flag & TFUNKNOWN;
   }
   return 0;
}

uint
typesize(Type t) {
   if (isprim(t) || t.t == TYPTR) return targ_primsizes[t.t];
   switch (t.t) {
   case TYENUM:
      return targ_primsizes[typedata[t.dat].backing];
   case TYARRAY:
      assert(!isvla(t));
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
typealign(Type t) {
   if (isprim(t) || t.t == TYPTR) return targ_primalign[t.t];
   switch (t.t) {
   case TYENUM:
      return targ_primalign[typedata[t.dat].backing];
   case TYARRAY:
      return typealign(typechild(t));
   case TYSTRUCT:
   case TYUNION:
      return typedata[t.dat].align;
   }
   return 0;
}

Type
mkptrtype(Type t, int qual) {
   if (isprim(t))
      return mktype(TYPTR, .flag = TFCHLDPRIM | (qual & TFCHLDQUAL), .child = t.t);
   else if (t.t == TYENUM || t.t == TYFUNC || isagg(t))
      return mktype(TYPTR, .flag = TFCHLDISDAT | (qual & TFCHLDQUAL), .dat = t.dat);
   return mktype(TYPTR, .flag = qual & TFCHLDQUAL,
                 .dat = interntd(&(TypeData) { TYPTR, .child = t }));
}

Type
mkarrtype(Type t, int qual, uint n) {
   if (isprim(t) && n < 256)
      return mktype(TYARRAY, .flag = TFCHLDPRIM | (qual & TFCHLDQUAL), .child = t.t, .arrlen = n);
   assert(!isvla(t));
   return mktype(TYARRAY, .flag = qual & TFCHLDQUAL,
                 .dat = interntd(&(TypeData) { TYARRAY, .child = t, .arrlen = n, .siz = n * typesize(t) }));
}

Type
mkunszarrtype(Type t, int qual) {
   if (isprim(t))
      return mktype(TYARRAY, .flag = TFCHLDPRIM | (qual & TFCHLDQUAL) | TFUNKNOWN, .child = t.t);
   return mktype(TYARRAY, .flag = TFUNKNOWN | (qual & TFCHLDQUAL),
                 .dat = interntd(&(TypeData) { TYARRAY, TFUNKNOWN, .child = t }));
}

Type
mkvlarrtype(Type t, int qual, uint refbits) {
   static int id = 0;
   return mktype(TYARRAY, .flag = (qual & TFCHLDQUAL) | TFVLA,
                 .dat = interntd(&(TypeData) { TYARRAY, .id = id++, .child = t, .vlasizeref = refbits }));
}

Type
mkfntype(Type ret, uint n, const Type *par, bool kandr, bool variadic) {
   TypeData td = { TYFUNC, .ret = ret, .nmemb = n, .param = par };
   td.kandr = kandr, td.variadic = variadic;
   return mktype(TYFUNC, .dat = interntd(&td));
}

Type
completetype(internstr name, int id, TypeData *td) {
   assert(td->t == TYENUM || td->t == TYSTRUCT || td->t == TYUNION);
   td->id = id;
   assert(id < countof(tagtypetags) && "too many tag types");
   if (tagtypetags[id])
      assert(tagtypetags[id] == name && "bad redefn");
   else
      tagtypetags[id] = name;
   return mktype(td->t, .dat = interntd(td));
}

Type
mktagtype(internstr name, TypeData *td) {
   static int id;
   return completetype(name, id++, td);
}

static bool
getfieldrec(FieldData *res, uint off, const TypeData *td, internstr name) {
Begin:
   for (int i = 0; i < td->nmemb; ++i) {
      NamedField *fld = &td->fld[i];
      if (fld->name == name) { /* match */
         *res = fld->f;
         res->off += off;
         return 1;
      } else if (!fld->name && isagg(fld->f.t)) { /* anonymous struct/union */
         const TypeData *ftd = &typedata[fld->f.t.dat];
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
getfield(FieldData *res, Type ty, internstr name) {
   assert(isagg(ty));
   return getfieldrec(res, 0, &typedata[ty.dat], name);
}

Type
typedecay(Type t) {
   if (t.t == TYARRAY)
      return mkptrtype(typechild(t), t.flag & TFCHLDQUAL);
   if (t.t == TYFUNC)
      return mkptrtype(t, 0);
   return t;
}

bool /* 6.5.16.1 Simple assignment Constraints */
assigncompat(Type dst, Type src) {
   if (dst.bits == src.bits || typescompat(NULL, dst, src)) return 1;
   if (isarith(dst) && isarith(src)) return 1;
   if (dst.t == TYPTR && src.t == TYPTR) {
      Type ds = typechild(dst), ss = typechild(src);
      if (ds.bits == ss.bits) return 1; /* T* with different qualifiers */
      if (ss.t == TYVOID || ds.t == TYVOID) return 1; /* T* <-> void* */
      enum typetag dt = scalartypet(ds), /* handle enums */
                   st = scalartypet(ss);
      if (!dt || !st) return 0; /* unequal incomplete enums */
      /* plain char exception */
      if (st == TYCHAR && in_range(dt, TYUCHAR, TYSCHAR)) return 1;
      if (dt == TYCHAR && in_range(st, TYUCHAR, TYSCHAR)) return 1;
   } else if (dst.t == TYBOOL && src.t == TYPTR)
      return 1;
   return 0;
}


bool /* 6.2.7 Compatible type and composite type */
typescompat(Type *pcompt, Type t1, Type t2) {
   if (t1.bits == t2.bits) {
      if (pcompt) *pcompt = t1;
      return 1;
   }
   if (t1.t != t2.t) return 0;
   if (t1.t == TYPTR) {
      int qual;
      if ((qual = t1.flag & TFCHLDQUAL) != (t2.flag & TFCHLDQUAL)) return 0;
      Type chld;
      if (!typescompat(&chld, typechild(t1), typechild(t2))) return 0;
      if (pcompt)
         *pcompt = mkptrtype(chld, qual);
      return 1;
   } else if (t1.t == TYARRAY) {
      int qual;
      Type chld;
      if ((qual = t1.flag & TFCHLDQUAL) != (t2.flag & TFCHLDQUAL)) return 0;
      if (!typescompat(&chld, typechild(t1), typechild(t2))) return 0;
      bool sized1 = !(t1.flag & (TFUNKNOWN | TFVLA)),
           sized2 = !(t2.flag & (TFUNKNOWN | TFVLA));
      if (sized1 || sized2) {
         uint len1 = sized1 ? typearrlen(t1) : typearrlen(t2);
         uint len2 = sized2 ? typearrlen(t2) : len1;
         if (len1 != len2) return 0;
         if (pcompt) *pcompt = mkarrtype(chld, qual, len1);
      } else if (pcompt) {
         *pcompt = mkunszarrtype(chld, qual);
      }
      return 1;
   } else if (t1.t == TYFUNC) {
      Type ret, param[40];
      const TypeData *td1 = &typedata[t1.dat], *td2 = &typedata[t2.dat];
      if (!typescompat(&ret, td1->ret, td2->ret)) return 0;
      if (td1->kandr && td2->kandr) {
         if (pcompt) *pcompt = mkfntype(ret, 0, NULL, /*kandr=*/1, 0);
         return 1;
      } else if (td1->kandr || td2->kandr) {
         if (pcompt) {
            const TypeData *tdgood = td1->kandr ? td2 : td1;
            *pcompt = mkfntype(ret, tdgood->nmemb, tdgood->param,
                  /*kandr=*/0, tdgood->variadic);
         }
         return 1;
      } else if (td1->nmemb != td2->nmemb || td1->variadic != td2->variadic) {
         return 0;
      }
      if (td1->nmemb > countof(param)) return 0; /* meh */

      for (int i = 0, npar = td1->nmemb; i < npar; ++i) {
         if (!typescompat(&param[i], td1->param[i], td2->param[i]))
            return 0;
      }
      if (pcompt)
         *pcompt = mkfntype(ret, td1->nmemb, param, 0, td1->variadic);
      return 1;
   }
   return 0;
}

enum typetag
intpromote(enum typetag t) {
   static int intisshort = -1;
   if (intisshort < 0) intisshort = targ_primsizes[TYINT] == targ_primsizes[TYSHORT];
   if (intisshort && t == TYUSHORT) return TYUINT;
   return t < TYINT ? TYINT : t;
}

Type /* 6.3.1.8 Usual arithmetic conversions */
cvtarith(Type a, Type b) {
   const Type none = {0};

   if (!isarith(a) || !isarith(b)) return none;
   if (a.t == TYENUM) a = typechild(a);
   if (b.t == TYENUM) b = typechild(b);
   if (iscomplex(a)) {
      if (iscomplex(b)) {
         /* when both are complex, choose type with greatest rank */
         /* enumeration order of type tags reflects arithmetic type rank */
         return a.t > b.t ? a : b;
      }
      /* complex float + double -> complex double */
      if (a.t - TYCOMPLEXF + TYFLOAT < b.t)
         a.t = b.t + TYCOMPLEXF - TYFLOAT;
      return a;
   } else if (iscomplex(b)) {
      /* double + complex float -> complex double */
      if (b.t - TYCOMPLEXF + TYFLOAT < a.t)
         b.t = a.t + TYCOMPLEXF - TYFLOAT;
      return b;
   }
   if (isflt(a) || isflt(b)) {
      /* when one type is float, choose type with greatest rank */
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

/* transform 'complex T' -> struct 'complex T' { T real, imag; } */
Type
complex2struct(Type t) {
   assert(iscomplex(t));
   static Type cache[3];
   if (cache[t.t-TYCOMPLEXF].t) return cache[t.t-TYCOMPLEXF];
   static const char *names[] = {
      "complex$float", "complex$double", "complex$longdouble"
   };
   Type base = mktype(t.t - TYCOMPLEXF + TYFLOAT);
   return cache[t.t-TYCOMPLEXF] = mktagtype(intern(names[t.t-TYCOMPLEXF]), &(TypeData){
      TYSTRUCT, .fld = (NamedField [2]){
         {intern("real"), {.t = base, .off = 0}},
         {intern("imag"), {.t = base, .off = typesize(base)}},
      }, .nmemb = 2, .align = typealign(base), .siz = typesize(base) * 2
   });
}

/* vim:set ts=3 sw=3 expandtab: */
