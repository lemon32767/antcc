#include "ir.h"

enum reg {
   R0 = 0,
#define R(n) (R0+n)
   FP = R(29), LR = R(30), SP = R(31),
   V0,
#define V(n) (V0+n)
};

bool aarch64_logimm(uint *enc, enum irclass, uvlong x);
void aarch64_isel(struct function *);
void aarch64_emit(struct function *);

/* vim:set ts=3 sw=3 expandtab: */

