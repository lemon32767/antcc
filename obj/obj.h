#include "../common.h"

extern struct objfile {
   const char *infile, *outfile;
   uchar *textbegin, *textend;
   uchar *code;
   uchar dataalign, rodataalign, bssalign;
   uint nbss;
   vec_of(uchar) data, rodata;
} objout;

enum relockind {
   REL_ABS64,
   REL_ABS32,
   REL_ABS32S,
   REL_PCREL32,
   REL_PLT32,
   REL_GOTPCRELX,
   REL_GOTPCRELX_REX,
   NRELOCKIND,
};
enum section { Snone, Stext, Srodata, Sdata, Sbss };

void objini(const char *infile, const char *outfile);
void objdeffunc(const char *nam, bool globl, uint off, uint siz);
enum section objhassym(const char *name, uint *off);
uint objnewdat(const char *name, enum section, bool globl, uint siz, uint align);
void objreloc(const char *sym, enum relockind, enum section, uint off, vlong addend);
void objfini(void);

/* vim:set ts=3 sw=3 expandtab: */
