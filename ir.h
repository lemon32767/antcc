#include "common.h"

enum irclass {
   KXXX,
   KI4, KI8, KPTR,
   KF4, KF8,
};

#define kisint(k) in_range((k), KI4, KPTR)
#define kisflt(k) in_range((k), KF4, KF8)

union irtype {
   struct { ushort isagg : 1, cls : 15; };
   struct { ushort _     : 1, dat : 15; };
   ushort bits;
};

struct irdat {
   uchar align : 7, mut : 1;
   uint siz;
   union {
      vec_of(uchar) dat;
      uchar sdat[8];
   };
   struct symref {
      struct symref *next;
      const char *sym;
      uint off;
      vlong addend;
   } *syms;
};

struct xcon {
   bool issym;
   uchar cls;
   union {
      const char *sym;
      int i4;
      vlong i8;
      float fs;
      double fd;
   };
};

struct call {
   short narg;
   short vararg; /* first variadic arg or -1 */
   union irtype *typs;
   union ref *args;
};

struct phi {
   struct block **blk;
   union ref *ref;
   int n, cap;
};

enum refkind {
   RNONE,
   RTMP,  /* reference to another instruction's result */
   RARG,  /* function argument */
   RICON, /* small integer constants */
   RXCON, /* other constants (incl. external symbols) */
   REXT,  /* reference to extra data for Ocall and Ophi */
   RREG,  /* machine register */
};

union ref {
   struct { uint t : 3, idx : 29; };
   struct { signed _0: 3, i : 29; }; /* RICON */
   uint bits;
};

enum op {
   Oxxx,
#define _(o,...) O##o,
#include "op.def"
#undef _
};

struct instr {
   uchar op, cls;
   uchar reg, hint;
   union ref l, r;
};

enum jumpkind {
   JXXX, Jb, Jbcnd, Jret, Jrets,
};

struct block {
   int id;
   struct block *s1, *s2;
   vec_of(ushort) phi;
   vec_of(ushort) ins;
   struct { uchar t; union ref arg; } jmp;
   struct block *lprev, *lnext;
};

struct function {
   struct arena *arena;
   const char *name;
   struct block *entry, *curblk;
   union type fnty, retty, *paramty;
   uint nblk;
   uint nparam;
   bool globl;
};

enum { MAXREGS = 32 };

struct mctarg {
   short gpr0, /* first gpr */
         ngpr, /* gpr count */
         fpr0, /* first fpr */
         nfpr; /* fpr count */
   struct bitset rcallee[1], /* callee-saved */
                 rglob[1];   /* globally live (never used for regalloc) */
   const char (*rnames)[6];
};

extern uchar type2cls[];
extern uchar cls2siz[];
extern const uchar siz2intcls[];
#define NOREF ((union ref) {0})
#define mkref(t, x) ((union ref) {{ (t), (x) }})
#define mkzerocon() ((union ref) {{ RICON, 0 }})
#define mkinstr(O, C, ...) ((struct instr) { .op = (O), .cls = (C), .reg=0,.hint=0, __VA_ARGS__ })
void irinit(struct function *);
void irfini(struct function *);
union irtype mkirtype(union type);
union ref mkintcon(struct function *, enum irclass, vlong);
union ref mkfltcon(struct function *, enum irclass, double);
union ref mksymref(struct function *, const char *);
void conputdat(struct irdat *, uint off, enum typetag t, const void *dat);
union ref mkcall(struct function *, union type fnty, uint narg, union ref *, union irtype *);
union ref addinstr(struct function *, struct instr);
union ref addphi2(struct function *, enum irclass cls,
                  struct block *b1, union ref r1, struct block *b2, union ref r2);
union ref addphi(struct function *, enum irclass cls, struct block **blk, union ref *ref, uint n);
struct block *newblk(struct function *);
void useblk(struct function *, struct block *);
void putjump(struct function *, enum jumpkind, union ref arg, struct block *t, struct block *f);
void insertinstr(struct block *, int idx, struct instr);
void delinstr(struct block *, int idx);

void irdump(struct function *, const char *fname);

void regalloc(struct function *);

/* vim:set ts=3 sw=3 expandtab: */
