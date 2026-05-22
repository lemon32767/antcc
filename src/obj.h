#pragma once

#include "antcc.h"

typedef struct ObjFile {
   const char *infile, *outfile;
   uchar *textbegin, *textend;
   uchar *code;
   uchar dataalign, rodataalign, bssalign;
   uint nbss;
   vec_of(uchar) data, rodata;
} ObjFile;

enum relockind {
   REL_ABS64,
   REL_ABS32,
   REL_ABS32S,
   REL_PCREL32,
   REL_PLT32,
   REL_GOTPCRELX,
   REL_GOTPCRELX_REX,
   REL_CALL26,
   REL_ADR_PREL_LO21,
   REL_ADR_PREL_PG_HI21,
   REL_ADD_ABS_LO12_NC,
   REL_LD_PREL_LO19,
   REL_ADR_GOT_PAGE,
   REL_LD64_GOT_LO12_NC,
   NRELOCKIND,
};
enum section { Snone, Stext, Srodata, Sdata, Sbss };

extern ObjFile objout;
void objini(const char *infile, const char *outfile);
void objdeffunc(internstr nam, bool globl, uint off, uint siz);
enum section objhassym(internstr name, uint *off);
uint objnewdat(internstr name, enum section, bool globl, uint siz, uint align);
void objreloc(internstr sym, /*enum symflags*/int, enum relockind, enum section, uint off, s64int addend);
void objfini(bool emit);
void markfnneeded(internstr);
bool fnisneeded(internstr);

/* vim:set ts=3 sw=3 expandtab: */
