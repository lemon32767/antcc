#ifndef TYPE_H_
#define TYPE_H_
#include "common.h"

enum qualifier {
   QCONST    = 1<<0,
   QVOLATILE = 1<<1,
};

enum typetag { /* ordering is important here! */
   TYXXX,
   TYENUM,
   TYBOOL, TYCHAR, TYSCHAR, TYUCHAR, TYSHORT, TYUSHORT, TYINT, TYUINT, TYLONG, TYULONG, TYVLONG, TYUVLONG,
   TYFLOAT, TYDOUBLE, TYLDOUBLE,
   TYVOID,
   TYPTR, TYARRAY, TYFUNC,
   TYSTRUCT, TYUNION,
   NTYPETAG,
   TYSIGNEDSET_ = 1<<TYSCHAR | 1<<TYSHORT | 1<<TYINT | 1<<TYLONG | 1<<TYVLONG,
   TYUNSIGNEDSET_ = 1<<TYUCHAR | 1<<TYUSHORT | 1<<TYUINT | 1<<TYULONG | 1<<TYUVLONG,
   TYSCALARSET_ = ((1u << (TYLDOUBLE - TYENUM + 1)) - 1) << TYENUM | 1<<TYPTR
};

enum typeflagmask {
   TFCHLDQUAL = 3,
   TFCHLDPRIM = 1<<2,
   TFCHLDISDAT = 1<<3,
};

union type {
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
};

#define isprimt(t)     in_range((t), TYBOOL, TYVOID)
#define isintt(t)      in_range((t), TYENUM, TYUVLONG)
#define issignedt(t)   ((TYSIGNEDSET_ | targ_charsigned << TYCHAR) >> (t) & 1)
#define isunsignedt(t) ((TYUNSIGNEDSET_ | !targ_charsigned << TYCHAR) >> (t) & 1)
#define isfltt(t)      in_range((t), TYFLOAT, TYLDOUBLE)
#define isaritht(t)    in_range((t), TYENUM, TYLDOUBLE)
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
#define mktype(...)   ((union type) {{ __VA_ARGS__ }})

struct enumvar {
   const char *name;
   union { vlong i; uvlong u; };
};

struct fielddata {
   union type t;
   ushort off;
   uchar bitsiz,
         bitoff : 6,
         qual : 2;
};
struct namedfield {
   const char *name;
   struct fielddata f;
};

struct typedata {
   uchar t;
   ushort id;
   union {
      union type child;
      struct { /* functions */
         const uchar *quals; /* packed N x 2bit array (NULL if no param has quals) */
         const union type *param;
      };
      struct { /* aggregates */
        /* struct fieldmap {
            union {
               struct field *fs;
               int *is;
            };
            const char **k;
            uint ;
         } *fmap; */
         struct namedfield *fld;
      };
      struct { /* enum */
         uchar backing;
         struct enumvar *var;
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
      union type ret; /* function */
   };
};

extern struct typedata typedata[];
extern const char *ttypenames[/*id*/];

#define tdqualsiz(nmemb) ((nmemb)/4 + ((nmemb)%4 != 0))
static inline int
tdgetqual(const uchar *pqual, uint idx)
{
   return pqual ? pqual[idx/4] >> 2*(idx%4) & 3 : 0;
}
static inline void
tdsetqual(uchar *pqual, uint idx, int qual)
{
   assert(pqual);
   pqual[idx/4] &= ~(3 << (2*(idx%4)));
   pqual[idx/4] |= (qual&3) << (2*(idx%4));
}

bool isincomplete(union type);
uint typesize(union type);
uint typealign(union type);
union type mkptrtype(union type, int qual);
union type mkarrtype(union type t, int qual, uint n);
union type mkfntype(union type ret, uint n, const union type *, const uchar *qual, bool kandr, bool variadic);
union type mktagtype(const char *name, struct typedata *td);
bool getfield(struct fielddata *res, union type, const char *);
union type completetype(const char *name, int id, struct typedata *td);
union type typedecay(union type);
bool assigncompat(union type dst, union type src);
enum typetag intpromote(enum typetag);
union type cvtarith(union type a, union type b);
static inline union type
typechild(union type t)
{
   if (t.t == TYENUM) return mktype(t.backing);
   if (t.flag & TFCHLDPRIM) return mktype(t.child);
   if (t.flag & TFCHLDISDAT) {
      union type chld = mktype(typedata[t.dat].t, .dat = t.dat);
      if (chld.t == TYENUM) chld.backing = typedata[t.dat].backing;
      return chld;
   }
   return typedata[t.dat].child;
}
static inline enum typetag
scalartypet(union type t)
{
   if (t.t == TYENUM) return t.backing;
   if (isptrcvt(t)) return TYPTR;
   return t.t;
}
static inline uint
typearrlen(union type t)
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

#endif

/* vim:set ts=3 sw=3 expandtab: */
