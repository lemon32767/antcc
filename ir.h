#include "common.h"

enum irclass {
   KXXX,
   KI4, KI8, KPTR,
   KF4, KF8,
};

#define kisint(k) in_range((k), KI4, KPTR)
#define kisflt(k) in_range((k), KF4, KF8)

union irtype {
   struct { ushort _     : 1, cls : 15; };
   struct { ushort isagg : 1, dat : 15; };
   ushort bits;
};

struct irdat {
   uchar align : 6, mut : 1, globl : 1;
   uint siz;
   union {
      vec_of(uchar) dat;
      uchar sdat[8];
   };
   const char *name;
   struct symref {
      struct symref *next;
      const char *sym;
      uint off;
      vlong addend;
   } *syms;
};

struct xcon {
   bool issym, isdat, deref;
   uchar cls;
   union {
      const char *sym;
      int dat;
      vlong i;
      double f;
   };
};

struct abiarg {
   union irtype ty;
   union {
      short reg; /* >= 0 */
      short stk; /* < 0 */
   };
};

struct call {
   union irtype ret;
   ushort narg;
   short vararg; /* first variadic arg or -1 */
   struct abiarg *abiarg;
   struct abiarg abiret[2];
};

struct phi {
   struct block **blk;
   union ref *ref;
   int n, cap;
};

enum refkind {
   RNONE,
   RTMP,   /* reference to another instruction's result */
   RREG,   /* machine register */
   RICON,  /* small integer constants */
   RXCON,  /* other constants (incl. external symbols) */
   RMORE,  /* Ocall -> calltab idx, Ophi -> phitab idx, else -> addrtab idx */
   RTYPE,  /* irtype */
};

union ref {
   struct { unsigned t : 3; signed i : 29; };
   uint bits;
};

struct addr {
   union ref base, index;
   int shift, disp;
};

enum op {
   Oxxx,
#define _(o,...) O##o,
#include "op.def"
#undef _
};

#define oiscmp(o) in_range(o, Oequ, Ougte)
#define oisalloca(o) in_range(o, Oalloca1, Oalloca16)
#define oisstore(o) in_range(o, Ostore1, Ostore8)
#define oisload(o) in_range(o, Oloads1, Oloadf8)
extern const char *opnames[];

enum intrin {
   INxxx,
#define _(b,...) IN##b,
#include "intrin.def"
#undef _
};

struct instr {
   uchar op,
         cls; /* operation data class; also result class except for cmp ops (always i4) */
   uchar skip : 1; /* ignore during codegen: forms part of one machine instruction */
   uchar inplace : 1; /* set (by isel) for instructions which modify its first arg in place */
   uchar reg; /* 0 -> no reg; else reg + 1 */
   union ref l, r;
};

enum jumpkind {
   JXXX, Jb, Jret,
};

struct block {
   int id;
   struct block *s1, *s2;
   vec_of(ushort) phi;
   vec_of(ushort) ins;
   struct { uchar t; union ref arg[2]; } jmp;
   struct block *lprev, *lnext;
};

enum { MAXREGS = 64 };

struct function {
   struct arena *arena;
   const char *name;
   struct block *entry, *curblk;
   union type fnty, retty;
   struct abiarg *abiarg, abiret[2];
   uint nblk;
   int stksiz;
   ushort nabiarg, nabiret;
   bool globl;
   struct bitset regusage[1];
};

enum objkind { OBJELF };
enum mcisa { ISamd64 };

struct mctarg {
   short gpr0, /* first gpr */
         ngpr, /* gpr count */
         bpr,  /* frame/base pointer reg */
         fpr0, /* first fpr */
         nfpr; /* fpr count */
   struct bitset rcallee[1], /* callee-saved */
                 rglob[1];   /* globally live (never used for regalloc) */
   const char (*rnames)[6];
   enum objkind objkind;
   enum mcisa isa;
   /* abiret: lower return type:
    *  scalar/small struct -> returns number of regs (1..2),
    *                         r & cls filled with reg and irclass of each scalar return
    *  big struct -> returns 0, is passed via hidden pointer argument,
    *                r[0] contains register for returning said pointer or -1,
    *                r[1] contains register for hidden pointer argument,
    *                     *ni is set to 1 if said register is the first ABI integer argument
    */
   int (*abiret)(short r[2], uchar cls[2], int *ni, union irtype);
   /* abiarg: lower argument type:
    *  scalar/small struct -> returns number of regs (1..2),
    *                         r & cls filled with reg and irclass of each scalar arg
    *                         if reg == -1 -> stack
    *  big struct -> returns 0,
    *                if passed in stack   cls[0] == 0, r[0] == negative SP offset
    *                if passed by pointer cls[0] == KPTR, r[0] contains integer register
    *                                                     or negative SP offset if stack
    */
   int (*abiarg)(short r[2], uchar cls[2], int *ni, int *nf, int *ns, union irtype);

   void (*isel)(struct function *);
   void (*emit)(struct function *);
};

enum { MAXINSTR = 1<<14 };

extern uchar type2cls[];
extern uchar cls2siz[];
extern const uchar siz2intcls[];
extern struct instr instrtab[];
extern struct xcon conht[];
extern struct calltab {vec_of(struct call);} calltab;
extern struct phitab {vec_of(struct phi);} phitab;
extern struct dattab {vec_of(struct irdat);} dattab;
extern struct addr addrht[];
#define insrescls(ins) (oiscmp((ins).op) ? KI4 : (ins).cls)
#define NOREF ((union ref) {0})
#define ZEROREF ((union ref) {{ RICON, 0 }})
#define mkref(t, x) ((union ref) {{ (t), (x) }})
#define mktyperef(t) ((union ref) {{ RTYPE, (t).bits }})
#define ref2type(r) ((union irtype) {.bits = (r).i})
#define mkinstr(O, C, ...) ((struct instr) { .op = (O), .cls = (C), .reg=0, __VA_ARGS__ })
#define mkarginstr(ty, x) mkinstr(Oarg, 0, mktyperef(ty), (x))
void irinit(struct function *);
void irfini(struct function *);
#define cls2type(k) ((union irtype){.cls=(k)})
union irtype mkirtype(union type);
union ref mkintcon(enum irclass, vlong);
union ref mkfltcon(enum irclass, double);
#define iscon(r) in_range((r).t, RICON, RXCON)
#define concls(r) ((r).t == RICON ? KI4 : conht[(r).i].cls)
#define isintcon(r) (iscon(r) && kisint(concls(r)))
#define isfltcon(r) ((r).t == RXCON && kisflt(conht[(r).i].cls))
#define isnumcon(r) ((r).t == RICON || ((r).t == RXCON && conht[(r).i].cls))
#define intconval(r) ((r).t == RICON ? (r).i : conht[(r).i].i)
#define fltconval(r) (conht[(r).i].f)
union ref mksymref(const char *);
union ref mkdatref(const char *name, uint siz, uint align, const void *, uint n, bool deref);
const char *xcon2sym(int ref);
struct instr mkalloca(uint siz, uint align);
void conputdat(struct irdat *, uint off, enum typetag t, const void *dat);
union ref mkcallarg(union irtype ret, uint narg, int vararg);
#define mkintrin(B, C, N) mkinstr(Ointrin, C, {.t=RICON,B}, mkcallarg((union irtype){{0}},N,-1))
union ref mkaddr(struct addr);
union ref addinstr(struct function *, struct instr);
union ref insertinstr(struct block *, int idx, struct instr);
void delinstr(struct block *, int idx);
union ref addphi2(struct function *, enum irclass cls,
                  struct block *b1, union ref r1, struct block *b2, union ref r2);
union ref addphi(struct function *, enum irclass cls, struct block **blk, union ref *ref, uint n);
struct block *newblk(struct function *);
void useblk(struct function *, struct block *);
void putbranch(struct function *, struct block *);
void putcondbranch(struct function *, union ref arg, struct block *t, struct block *f);
void putreturn(struct function *, union ref r0, union ref r1);
void blkreplref(struct block *, int, union ref from, union ref to);
void replref(struct function *, struct block *, int, union ref from, union ref to);

void irdump(struct function *);

void lowerintrin(struct function *);
void abi0(struct function *);
void abi0_call(struct function *, struct instr *, struct block *blk, int *curi);
void regalloc(struct function *);

/* vim:set ts=3 sw=3 expandtab: */
