#include "obj.h"
#include "common.h"
#include "ir.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

void elfinit(void);
void elfaddsym(const char *, int info, enum section, uvlong value, uvlong size);
void elfreloc(const char *sym, enum relockind, enum section, uint off, vlong addend);
void elfputdat(const struct irdat *);
void elffini(struct wbuf *);

struct objfile objout;

enum { NTEXT = 4<<20 /* 4MiB */ };

void
objini(const char *file)
{
   assert(!objout.file);
   objout.file = file;
   objout.code = objout.textbegin = mapzeros(NTEXT);
   objout.textend = objout.textbegin + NTEXT;

   switch (mctarg->objkind) {
   case OBJELF: elfinit(); break;
   }
}

void
objdeffunc(const char *nam, bool globl, uint off, uint siz)
{
   switch (mctarg->objkind) {
   case OBJELF:
      elfaddsym(nam, /*STT_LOCAL/GLOBAL*/globl << 4 | /*STT_FUNC*/2, Stext, off, siz);
      break;
   }
}

void
objreloc(const char *sym, enum relockind reloc, enum section section, uint off, vlong addend)
{
   switch (mctarg->objkind) {
   case OBJELF:
      elfreloc(sym, reloc, section, off, addend);
      break;
   }
}

void
objfini(void)
{
   static char buf[1<<12];
   struct wbuf out = FDBUF(buf, sizeof buf, open(objout.file, O_WRONLY | O_CREAT | O_TRUNC, 0666));
   if (out.fd < 0) fatal(NULL, "could not open %'s for writing: %s", objout.file, strerror(errno));

   for (int i = 0; i < dattab.n; ++i) {
      elfputdat(&dattab.p[i]);
   }

   switch (mctarg->objkind) {
   case OBJELF: elffini(&out); break;
   }

   munmap(objout.textbegin, NTEXT);
   ioflush(&out);
   close(out.fd);
}

/* vim:set ts=3 sw=3 expandtab: */
