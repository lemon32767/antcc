#include "common.h"

enum irclass {
   KXXX,
   KI4, KI8, KPTR,
   KF4, KF8,
};

#define kisint(k) in_range((k), KI4, KIP)
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
   RNONE, RTMP, RARG, RICON, RXCON, RSYM, REXT
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
   uchar op;
   uchar cls;
   union ref l, r;
};

enum jumpkind {
   JXXX, Jb, Jbcnd, Jret, Jrets,
};

struct block {
   int id;
   struct block *s1, *s2;
   struct { uchar t; union ref arg; } jmp;   
   vec_of(ushort) phi;
   vec_of(ushort) ins;
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

extern uchar type2cls[];
extern uchar cls2siz[];
extern const uchar siz2intcls[];
void irinit(struct function *);
#define NOREF ((union ref) {0})
#define mkref(t, x) ((union ref) {{ (t), (x) }})
#define mkzerocon() ((union irref){ RICON, 0 })
union irtype mkirtype(union type);
union ref mkintcon(struct function *, enum irclass, vlong);
union ref mkfltcon(struct function *, enum irclass, double);
union ref mksymref(struct function *, const char *);
void conputdat(struct irdat *, uint off, enum typetag t, const void *dat);
union ref mkcall(struct function *, union type fnty, uint narg, union ref *, union irtype *);
union ref addinstr(struct function *, struct instr);
union ref addphi2(struct function *, enum irclass cls,
                  struct block *b1, union ref r1, struct block *b2, union ref r2);
struct block *newblk(struct function *);
void useblk(struct function *, struct block *);
void putjump(struct function *, enum jumpkind, union ref arg, struct block *t, struct block *f);
void irdump(struct function *, const char *fname);

/* vim:set ts=3 sw=3 expandtab: */
