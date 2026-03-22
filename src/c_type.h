#pragma once

#include "antcc.h"

enum qualifier {
   QCONST    = 1<<0,
   QVOLATILE = 1<<1,
};

enum typetag { /* ordering is important here! */
   TYXXX,
   TYENUM,
   TYBOOL, TYCHAR, TYSCHAR, TYUCHAR, TYSHORT, TYUSHORT, TYINT, TYUINT, TYLONG, TYULONG, TYVLONG, TYUVLONG,
   TYFLOAT, TYDOUBLE, TYLDOUBLE, TYCOMPLEXF, TYCOMPLEX, TYCOMPLEXL,
   TYVOID,
   TYPTR, TYARRAY, TYFUNC,
   TYSTRUCT, TYUNION,
   NTYPETAG,
   TYSIGNEDSET_ = 1<<TYSCHAR | 1<<TYSHORT | 1<<TYINT | 1<<TYLONG | 1<<TYVLONG,
   TYUNSIGNEDSET_ = 1<<TYUCHAR | 1<<TYUSHORT | 1<<TYUINT | 1<<TYULONG | 1<<TYUVLONG,
   TYSCALARSET_ = ((1u << (TYCOMPLEXL - TYENUM + 1)) - 1) << TYENUM | 1<<TYPTR
};

enum typeflagmask {
   TFCHLDQUAL  = 3,
   TFCHLDPRIM  = 1<<2,
   TFCHLDISDAT = 1<<3,
};

typedef union Type {
   struct {
      uchar t; /* type tag */
      union {
         uchar flag;
         uchar backing; /* type tag for enum backing int */
      };
      union {
         struct {
            uchar child; /* prim type tag */
            uchar arrlen; /* small array */
         };
         ushort dat; /* index into typedata */
      };
   };
   uint bits;
} Type;
static_assert(sizeof(Type) == 4);

#define isprimt(t)     in_range((t), TYBOOL, TYVOID)
#define isintt(t)      in_range((t), TYENUM, TYUVLONG)
#define issignedt(t)   ((TYSIGNEDSET_ | targ_charsigned << TYCHAR) >> (t) & 1)
#define isunsignedt(t) ((TYUNSIGNEDSET_ | !targ_charsigned << TYCHAR) >> (t) & 1)
#define isfltt(t)      in_range((t), TYFLOAT, TYLDOUBLE)
#define isaritht(t)    in_range((t), TYENUM, TYCOMPLEXL)
#define isscalart(t)   (TYSCALARSET_ >> (t) & 1)
#define isptrcvtt(t)   in_range((t), TYPTR, TYFUNC)
#define isaggt(t)      in_range((t), TYSTRUCT, TYUNION)
#define isprim(ty)     isprimt((ty).t)
#define isint(ty)      isintt((ty).t)
#define issigned(ty)   issignedt(scalartypet(ty))
#define isunsigned(ty) isunsignedt(scalartypet(ty))
#define isflt(ty)      isfltt((ty).t)
#define isarith(ty)    isaritht((ty).t)
#define isscalar(ty)   isscalart((ty).t)
#define isptrcvt(ty)   isptrcvtt((ty).t)
#define isagg(ty)      isaggt((ty).t)
#define iscomplext(t)  in_range((t), TYCOMPLEXF, TYCOMPLEXL)
#define iscomplex(ty)  iscomplext((ty).t)
#define mktype(...)   ((Type) {{ __VA_ARGS__ }})

typedef struct {
   internstr name;
   union { s64int i; u64int u; };
} EnumVar;

typedef struct {
   Type t;
   ushort off;
   uchar bitsiz,
         bitoff : 6,
         qual : 2;
} FieldData;

typedef struct {
   internstr name;
   FieldData f;
} NamedField;

typedef struct TypeData {
   uchar t;
   ushort id;
   union {
      Type child;
      const Type *param; /* functions */
      struct { /* aggregates */
         NamedField *fld;
      };
      struct { /* enum */
         uchar backing;
         EnumVar *var;
      };
   };
   union {
      uint arrlen; /* array */
      struct {
         short nmemb; /* functions, aggregates, enums */
         uchar align;
         union {
            struct { /* function */
               bool kandr : 1, variadic : 1;
            };
            struct { /* aggregate */
               bool anyconst : 1, flexi : 1;
            };
         };
      };
   };
   union {
      uint siz; /* aggregate & array */
      Type ret; /* function */
   };
} TypeData;

extern TypeData typedata[];
extern internstr tagtypetags[/*id*/];
extern const char *const primtypenames[/*enum typetag*/];

bool isincomplete(Type);
uint typesize(Type);
uint typealign(Type);
Type mkptrtype(Type, int qual);
Type mkarrtype(Type t, int qual, uint n);
Type mkfntype(Type ret, uint n, const Type *, bool kandr, bool variadic);
Type mktagtype(internstr name, TypeData *td);
bool getfield(FieldData *res, Type, internstr);
Type completetype(internstr name, int id, TypeData *td);
Type typedecay(Type);
bool assigncompat(Type dst, Type src);
enum typetag intpromote(enum typetag);
Type cvtarith(Type a, Type b);
static inline Type
typechild(Type t)
{
   if (t.t == TYENUM) return mktype(t.backing ? t.backing : typedata[t.dat].backing);
   if (t.flag & TFCHLDPRIM) return mktype(t.child);
   if (t.flag & TFCHLDISDAT) {
      Type chld = mktype(typedata[t.dat].t, .dat = t.dat);
      if (chld.t == TYENUM) chld.backing = typedata[t.dat].backing;
      return chld;
   }
   return typedata[t.dat].child;
}
static inline enum typetag
scalartypet(Type t)
{
   if (t.t == TYENUM) return t.backing ? t.backing : typedata[t.dat].backing;
   if (isptrcvt(t)) return TYPTR;
   assert(!iscomplex(t));
   return t.t;
}
static inline uint
typearrlen(Type t)
{
   return (t.flag & TFCHLDPRIM) ? t.arrlen : typedata[t.dat].arrlen;
}

/**********/
/* Target */
/**********/

extern uint targ_valistsize;
extern uchar targ_primsizes[];
extern uchar targ_primalign[];
extern enum typetag targ_sizetype, targ_ptrdifftype, targ_wchartype;
extern bool targ_charsigned, targ_bigendian, targ_64bit;

/* vim:set ts=3 sw=3 expandtab: */
