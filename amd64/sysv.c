#include "all.h"

static int
classify(uchar cls[2], const struct typedata *td, uint off)
{
   uint siz = alignup(td->siz, 8);
   if (siz > 16) /* MEMORY */
      return 0;
   for (int i = 0; i < td->nmemb; ++i) {
      struct fielddata *fld = &td->fld[i].f;
      uint align = typealign(fld->t);
      if (alignup(fld->off, align) != fld->off) /* unaligned field */
         return cls[0] = cls[1] = 0;
      if (isagg(fld->t)) {
         if (!classify(cls, &typedata[fld->t.dat], fld->off))
            return cls[0] = cls[1] = 0;
      } else if (isflt(fld->t)) { /* SSE */
         if (!cls[(fld->off + off)/8])
            cls[(fld->off + off)/8] = KF8;
      } else { /* INTEGER */
         cls[(fld->off +  off)/8] = KI8;
      }
   }
   return !!cls[0] + !!cls[1];
}

static int
argabi(short r[2], uchar cls[2], int *ni, int *nf, union irtype typ)
{
   static const uchar intregs[] = { RDI, RSI, RDX, RCX, R8, R9 };
   enum { NINT = arraylength(intregs), NFLT = 8 };
   int ret, ni_save, nf_save;

   if (!typ.isagg) {
      if (kisflt(typ.cls) && *nf < NFLT) {
         cls[0] = KF8; /* SSE */
         r[0] = XMM0 + (*nf)++;
         return 1;
      }
      if (*ni < NINT) {
         cls[0] = KI8; /* INTEGER */
         r[0] = intregs[(*ni)++];
         return 1;
      }
      return 0; /* MEMORY */
   }
   cls[0] = cls[1] = 0;
   ret = classify(cls, &typedata[typ.dat], 0);
   if (!ret) return 0;
   ni_save = *ni, nf_save = *nf;
   assert(ret <= 2);
   for (int i = 0; i < ret; ++i) {
      assert(cls[i]);
      if (cls[i] == KF8 && *nf < NFLT)
         r[i] = XMM0 + (*nf)++;
      else if (cls[i] == KI8 && *ni < NINT)
         r[i] = intregs[(*ni)++];
      else { /* MEMORY */
         *ni = ni_save, *nf = nf_save;
         return cls[0] = cls[1] = 0;
      }
   }
   return ret;
}

static int
retabi(short r[2], uchar cls[2], union irtype typ)
{
   int ret;

   if (!typ.isagg) return kisflt(typ.cls) ? XMM0 : RAX;
   cls[0] = cls[1] = 0;
   ret = classify(cls, &typedata[typ.dat], 0);
   if (!ret) { /* MEMORY */
      r[0] = RDI; /* register for caller-owned result location argument */
      return 0;
   }
   assert(ret <= 2);
   for (int i = 0; i < ret; ++i) {
      assert(cls[i]);
      if (cls[i] == KF8) /* SSE (XMM0, XMM1) */
         r[i] = XMM0 + i;
      else if (cls[i] == KI8) /* INTEGER (RAX, RDX) */
         r[i] = i == 0 ? RAX : RDX;
      else assert(0);
   }
   return ret;
}

const char amd64_rnames[][6] = {
#define R(r) #r,
   LIST_REGS(R)
#undef R
};

const struct mctarg t_amd64_sysv = {
   .gpr0 = RAX, .ngpr = R15 - RAX + 1,
   .fpr0 = XMM0, .nfpr = XMM15 - XMM0 + 1,
   .rcallee = {{1<<RBX | 1<<R12 | 1<<R13 | 1<<R14 | 1<<R15}},
   .rglob = {{1<<RSP | 1<<RBP}},
   .rnames = amd64_rnames,
   .abi_argregs = argabi,
   .abi_retregs = retabi,
};

/* vim:set ts=3 sw=3 expandtab: */
