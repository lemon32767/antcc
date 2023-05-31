#include "../ir.h"


#define LIST_REGS(_)                                               \
   _(RAX) _(RCX) _(RDX) _(RBX) _(RSP) _(RBP) _(RSI) _(RDI)         \
   _(R8) _(R9) _(R10) _(R11) _(R12) _(R13) _(R14) _(R15)           \
   _(XMM0) _(XMM1) _(XMM2) _(XMM3) _(XMM4) _(XMM5) _(XMM6) _(XMM7) \
   _(XMM8) _(XMM9) _(XMM10) _(XMM11) _(XMM12) _(XMM13) _(XMM14) _(XMM15)

enum {
   Rxxx,
#define R(r) r,
   LIST_REGS(R)
#undef R
};

const char amd64_rnames[][6] = {
   "?",
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
};

/* vim:set ts=3 sw=3 expandtab: */
