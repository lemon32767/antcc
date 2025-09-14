#include "common.h"

/*************/
/* EXPR TREE */
/*************/

enum exprkind {
   EXXX, ENUMLIT, ESTRLIT, ESYM, EINIT, EGETF, ECALL, ECOND,
   /* unary */
   EPLUS, ENEG, ECOMPL, ELOGNOT, EDEREF, EADDROF, ECAST,
   EPREINC, EPOSTINC, EPREDEC, EPOSTDEC,
   /* binary */
   EADD, ESUB, EMUL, EDIV, EREM, EBAND, EBIOR, EXOR, ESHL, ESHR,
   ELOGAND, ELOGIOR,
   EEQU, ENEQ, ELTH, EGTH, ELTE, EGTE,
   ESET, ESETADD, ESETSUB, ESETMUL, ESETDIV, ESETREM, ESETAND, ESETIOR, ESETXOR, ESETSHL, ESETSHR,
   ESEQ,
};
#define isunop(t) in_range(t, EPLUS, EPOSTDEC)
#define isbinop(t) in_range(t, EADD, ESEQ)
#define isassign(t) in_range(t, ESET, ESETSHR)
#define assigntobinop(t) ((t) - ESETADD + EADD)

struct expr {
   uchar t;
   uchar qual;
   ushort narg; /* ECALL */
   union type ty;
   struct span span;
   union {
     struct {
        struct expr *sub;
        struct exgetfld {
           ushort off;
           uchar bitsiz, bitoff;
        } fld; /* EGETF */
     };
     uvlong u; vlong i; double f; /* ENUMLIT */
     struct bytes s; /* ESTRLIT */
     struct decl *sym; /* ESYM */
     struct init *init; /* EINIT */
   };
};

struct init {
   uint n;
   struct bitset *zero;
   uint *offs;
   struct { uchar off, siz; } *bitf;
   struct expr **ex;
};

enum storageclass {
   SCNONE,
   SCTYPEDEF = 1<<0,
   SCEXTERN = 1<<1,
   SCSTATIC = 1<<2,
   SCTHREADLOCAL = 1<<3,
   SCAUTO = 1<<4,
   SCREGISTER = 1<<5,
};

struct decl {
   union type ty;
   uchar scls;
   uchar qual : 2;
   uchar isenum : 1;
   uchar isdef : 1;
   struct span span;
   const char *name;
   union {
      struct { ushort align; int id; };
      vlong value;
   };
};

enum evalmode {
   EVNONE,
   EVINTCONST,
   EVARITH,
   EVSTATICINI,
   EVFOLD,
};

bool eval(struct expr *, enum evalmode);

/* vim:set ts=3 sw=3 expandtab: */
