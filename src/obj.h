#pragma once

#include "antcc.h"

enum relockind {
   REL_ABS64,
   REL_ABS32,
   REL_ABS32S,
   REL_PCREL32,
   REL_PLT32,
   REL_GOTPCRELX,
   REL_GOTPCRELX_REX,
   REL_TPOFF32,
   REL_GOTTPOFF,
   REL_CALL26,
   REL_ADR_PREL_LO21,
   REL_ADR_PREL_PG_HI21,
   REL_ADD_ABS_LO12_NC,
   REL_LD_PREL_LO19,
   REL_ADR_GOT_PAGE,
   REL_LD64_GOT_LO12_NC,
   REL_TLSLE_ADD_TPREL_HI12,
   REL_TLSLE_ADD_TPREL_LO12_NC,
   REL_TLSIE_ADR_GOTTPREL_PAGE21,
   REL_TLSIE_LD64_GOTTPREL_LO12_NC,
   NRELOCKIND,
};
enum section { Snone, Stext, Srodata, Sdata, Sbss, Stdata, Stbss, NSECTION };

enum symaccess {
   SYMDIRECT,
   SYMGOT,
   SYMTLSLOCAL,
   SYMTLSGOT,
};

typedef struct ObjSection {
   internstr name;
   vec_of(uchar);
   short align;
   bool w:1, x:1, pb:1, tls:1;
} ObjSection;

typedef struct ObjFile {
   const char *infile, *outfile;
   ObjSection sec[NSECTION];
} ObjFile;

void objini(const char *infile, const char *outfile);
void objdeffunc(internstr nam, enum symflags, uint off, uint siz);
enum section objhassym(internstr name, uint *off);
uint objnewdat(internstr name, enum symflags, enum section, uint siz, uint align);
uchar *objsecdata(enum section, uint off);
uchar *objtextinit(void);
void objtextfini(uchar *pcode);
void objreloc(internstr sym, enum symflags, enum relockind, enum section, uint off, s64int addend);
void objfini(bool emit);
void markfnneeded(internstr);
bool fnisneeded(internstr);
#define objrelocxcon(xc, ...) objreloc(xcon2sym(xc), contab.p[xc].flag, __VA_ARGS__)

/* vim:set ts=3 sw=3 expandtab: */
