#include "common.h"

extern const struct mctarg t_amd64_sysv;
static const struct targ {
   const char *name;
   struct { uchar longsize, vlongsize, ptrsize, valistsize; };
   struct { uchar longalign, vlongalign, doublealign, ptralign; };
   bool charsigned;
   uchar sizetype, ptrdifftype;
   const struct mctarg *mctarg;
} targs[] = {
   { "amd64-sysv", {8, 8, 8, 24}, {8, 8, 8, 8}, 1, TYULONG, TYLONG, &t_amd64_sysv },
   { "i686-sysv",  {4, 8, 4,  8}, {4, 4, 4, 4}, 1, TYUINT, TYINT }
};

uchar targ_primsizes[TYPTR+1];
uchar targ_primalign[TYPTR+1];
enum typetag targ_sizetype, targ_ptrdifftype;
bool targ_charsigned, targ_bigendian;
const struct mctarg *mctarg;

void
targ_init(const char *starg)
{
   const struct targ *t = &targs[0];
   uchar *sizes = targ_primsizes, *align = targ_primalign;

   sizes[TYBOOL] = sizes[TYCHAR] = sizes[TYSCHAR] = sizes[TYUCHAR] = 1;
   sizes[TYSHORT] = sizes[TYUSHORT] = 2;
   sizes[TYUINT] = sizes[TYINT] = 4;
   sizes[TYFLOAT] = 4;
   sizes[TYDOUBLE] = 8;
   memcpy(align, sizes, sizeof targ_primalign);
   sizes[TYULONG] = sizes[TYLONG] = t->longsize;
   sizes[TYUVLONG] = sizes[TYVLONG] = t->vlongsize;
   sizes[TYPTR] = t->ptrsize;
   sizes[TYVALIST] = t->valistsize;
   align[TYULONG] = align[TYLONG] = t->longalign;
   align[TYUVLONG] = align[TYVLONG] = t->vlongalign;
   align[TYDOUBLE] = t->doublealign;
   align[TYVALIST] = align[TYPTR] = t->ptralign;
   targ_sizetype = t->sizetype;
   targ_ptrdifftype = t->ptrdifftype;
   targ_charsigned = t->charsigned;
   targ_bigendian = 0;
   mctarg = t->mctarg;
}
