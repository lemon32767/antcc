#include "common.h"

extern struct objfile {
   const char *file;
   uchar *textbegin, *textend;
   uchar *code;
} objout;

enum relockind {
   REL_ABS,
   REL_PCREL32,
   NRELOCKIND,
};
enum section { Snone, Stext, Srodata, Sdata, Sbss };

void objini(const char *);
void objdeffunc(const char *nam, bool globl, uint off, uint siz);
void objreloc(const char *sym, enum relockind, enum section, uint off, vlong addend);
void objfini(void);

/* vim:set ts=3 sw=3 expandtab: */
