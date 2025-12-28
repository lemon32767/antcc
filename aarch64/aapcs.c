#include "all.h"

static int
abiarg(short r[2], uchar cls[2], uchar *r2off, int *ni, int *nf, int *ns, union irtype typ)
{
   enum { NINT = 8, NFLT = 8 };
   if (!typ.isagg) {
      if (kisflt(cls[0] = typ.cls) && *nf < 8) {
         r[0] = V(0) + (*nf)++;
      } else if (kisint(cls[0]) && *ni < NINT) {
         r[0] = R0 + (*ni)++;
      } else {
         r[0] = *ns;
         *ns += 8;
         return 0; /* MEMORY */
      }
      return 1;
   } else assert(!"nyi");
}

static int
abiret(short r[2], uchar cls[2], uchar *r2off, int *ni, union irtype typ)
{
   if (!typ.isagg) {
      r[0] = kisflt(cls[0] = typ.cls) ? V(0) : R0;
      return 1;
   }
   int nf = 0, ns = 0;
   int ret = abiarg(r, cls, r2off, ni, &nf, &ns, typ);
   if (ret) return ret;
   /* caller-allocated result address in x8 */
   assert(*ni == 0);
   r[0] = -1;
   r[1] = R(8);
   return 0;
}

static void
vastart(struct function *fn, struct block *blk, int *curi)
{
   assert(!"nyi");
}

static void
vaarg(struct function *fn, struct block *blk, int *curi)
{
   assert(!"nyi");
}

static const char aarch64_rnames[][6] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7", "R8", "R9","R10","R11","R12","R13","R14","R15",
   "R16","R17","R18","R19","R20","R21","R22","R23","R24","R25","R26","R27","R28", "FP", "LR", "SP",
    "V0", "V1", "V2", "V3", "V4", "V5", "V6", "V7", "V8", "V9","V10","V11","V12","V13","V14","V15",
   "V16","V17","V18","V19","V20","V21","V22","V23","V24","V25","V26","V27","V28","V29","V30","V31",
};

const struct mctarg t_aarch64_aapcs = {
   .gpr0 = R0, .ngpr = 31,
   .bpr = FP,
   .gprscratch = R(16), .fprscratch = V(31),
   .fpr0 = V0, .nfpr = 32,
   .rcallee = BIT(R(19)) | BIT(R(20)) | BIT(R(21)) | BIT(R(22)) | BIT(R(23))
            | BIT(R(24)) | BIT(R(25)) | BIT(R(26)) | BIT(R(27)) | BIT(R(28))
            | BIT( V(8)) | BIT( V(9)) | BIT(V(10)) | BIT(V(11)) | BIT(V(12))
            | BIT(V(13)) | BIT(V(14)) | BIT(V(15)),
   .rglob = BIT(FP) | BIT(LR) | BIT(SP),
   .rnames = aarch64_rnames,
   .objkind = OBJELF,
   .abiret = abiret,
   .abiarg = abiarg,
   .vastart = vastart,
   .vaarg = vaarg,
   .isel = aarch64_isel,
   .emit = aarch64_emit,
};

/* vim:set ts=3 sw=3 expandtab: */
