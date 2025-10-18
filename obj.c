#include "obj.h"
#include "common.h"
#include "ir.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>


void elfinit(void);
void elfaddsym(const char *, int info, enum section, uvlong value, uvlong size);
void elfreloc(const char *sym, enum relockind, enum section, uint off, vlong addend);
void elffini(struct wbuf *);

struct objfile objout;

enum { NTEXT = 4<<20 /* 4MiB */ };

void
objini(const char *infile, const char *outfile)
{
   assert(!objout.outfile);
   objout.infile = infile;
   objout.outfile = outfile;
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

uint
objnewdat(const char *name, enum section sec, bool globl, uint siz, uint align)
{
   uint off;

   assert(siz && align && ispo2(align));

   switch (sec) {
   default: assert(0);
   case Srodata:
      if (align > objout.rodataalign) objout.rodataalign = align;
      while (objout.rodata.n & (align - 1)) vpush(&objout.rodata, 0);
      off = objout.rodata.n;
      vresize(&objout.rodata, objout.rodata.n + siz);
      memset(objout.rodata.p+off, 0, siz);
      break;
   case Sdata:
      if (align > objout.dataalign) objout.dataalign = align;
      while (objout.data.n & (align - 1)) vpush(&objout.data, 0);
      off = objout.data.n;
      vresize(&objout.data, objout.data.n + siz);
      memset(objout.data.p+off, 0, siz);
      break;
   case Sbss:
      if (align > objout.bssalign) objout.bssalign = align;
      off = alignup(objout.nbss, align);
      objout.nbss = off + siz;
      break;
   }

   switch (mctarg->objkind) {
   case OBJELF:
      elfaddsym(name, /*STT_LOCAL/GLOBAL*/globl<<4 | /*STT_OBJECT*/1, sec, off, siz);
      break;
   }
   return off;
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
   struct wbuf out = FDBUF(buf, sizeof buf, open(objout.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666));
   if (out.fd < 0) fatal(NULL, "could not open %'s for writing: %s", objout.outfile, strerror(errno));

   switch (mctarg->objkind) {
   case OBJELF: elffini(&out); break;
   }

   munmap(objout.textbegin, NTEXT);
   ioflush(&out);
   close(out.fd);
}

/* vim:set ts=3 sw=3 expandtab: */
