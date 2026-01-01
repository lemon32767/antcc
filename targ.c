#include "common.h"
#include "type.h"

extern const struct mctarg t_x86_64_sysv, t_aarch64_aapcs;
static const struct targ {
   struct { enum mcarch arch; uint oss, abis; };
   struct { uchar longsize, vlongsize, ptrsize, valistsize; };
   struct { uchar longalign, vlongalign, doublealign, ptralign; };
   bool charsigned;
   uchar sizetype, ptrdifftype, wchartype;
   const struct mctarg *mctarg;
} targs[] = {
   { {ISx86_64,  -1, 1<<ABIgnu | 1<<ABImusl}, {8,8,8,24}, {8,8,8,8}, 1, TYULONG, TYLONG, TYINT, &t_x86_64_sysv },
   { {ISaarch64, -1, 1<<ABIgnu | 1<<ABImusl}, {8,8,8,32}, {8,8,8,8}, 0, TYULONG, TYLONG, TYUINT, &t_aarch64_aapcs },
};

struct targtriple target;
uchar targ_primsizes[TYPTR+1];
uchar targ_primalign[TYPTR+1];
uint targ_valistsize;
enum typetag targ_sizetype, targ_ptrdifftype, targ_wchartype;
bool targ_charsigned, targ_bigendian, targ_64bit;
enum mcarch targ_arch;
const struct mctarg *mctarg;

static bool
matchstr(const char **s, const char *pat)
{
   const char *p;
   for (p = *s; *pat; ++p, ++pat) {
      if (*pat == '$') { if (*p) return 0; else break; }
      else if (*p != *pat) return 0;
   }
   *s = p;
   return 1;
}

static bool
parsetriple(struct targtriple *trg, const char *str)
{
   if (matchstr(&str, "x86_64-"))
      trg->arch = ISx86_64;
   else if (matchstr(&str, "aarch64-") || matchstr(&str, "arm64-"))
      trg->arch = ISaarch64;
   else return 0;

   if (matchstr(&str, "unknown-") || matchstr(&str, "pc-")) {}

   if (matchstr(&str, "linux-")) {
      trg->os = OSlinux;
   } else if (matchstr(&str, "linux$")) {
      trg->os = OSlinux;
      trg->abi = ABIgnu;
   } else return 0;

   if (matchstr(&str, "gnu")) {
      trg->abi = ABIgnu;
   } else if (matchstr(&str, "musl")) {
      trg->abi = ABImusl;
   } else return 0;

   return 1;
}

#include "hostconfig.h" /* run ./configure */

void
targ_init(const char *starg)
{
   const struct targ *t = NULL;
   uchar *sizes = targ_primsizes, *align = targ_primalign;

   if (!starg) {
      target.arch = HOST_ARCH;
      target.os = HOST_OS;
      target.abi = HOST_ABI;
   } else if (!parsetriple(&target, starg)) {
      fatal(NULL, "unrecognized target: %s", starg);
   }

   for (size_t i = 0; i < countof(targs); ++i) {
      if (targs[i].arch == target.arch)
      if (targs[i].oss & (1 << target.os))
      if (targs[i].abis & (1 << target.abi)) {
         t = &targs[i];
         break;
      }
   }
   if (!t) fatal(NULL, "unsupported target: %s", starg ? starg : HOST_TRIPLE);

   sizes[TYBOOL] = sizes[TYCHAR] = sizes[TYSCHAR] = sizes[TYUCHAR] = 1;
   sizes[TYSHORT] = sizes[TYUSHORT] = 2;
   sizes[TYUINT] = sizes[TYINT] = 4;
   sizes[TYFLOAT] = 4;
   sizes[TYDOUBLE] = 8;
   sizes[TYLDOUBLE] = 8;
   memcpy(align, sizes, sizeof targ_primalign);
   sizes[TYULONG] = sizes[TYLONG] = t->longsize;
   sizes[TYUVLONG] = sizes[TYVLONG] = t->vlongsize;
   sizes[TYPTR] = t->ptrsize;
   align[TYULONG] = align[TYLONG] = t->longalign;
   align[TYUVLONG] = align[TYVLONG] = t->vlongalign;
   align[TYDOUBLE] = t->doublealign;
   align[TYLDOUBLE] = t->doublealign;
   align[TYPTR] = t->ptralign;
   targ_valistsize = t->valistsize;
   targ_sizetype = t->sizetype;
   targ_ptrdifftype = t->ptrdifftype;
   targ_wchartype = t->wchartype;
   targ_charsigned = t->charsigned;
   targ_bigendian = 0;
   targ_64bit = t->ptrsize == 8;
   mctarg = t->mctarg;
   targ_arch = ISx86_64;
}
