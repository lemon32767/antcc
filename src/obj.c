#include "obj.h"
#include "ir.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>


void elfinit(void);
enum section elfhassym(internstr , uint *value);
void elfaddsym(internstr , int info, enum section, u64int value, u64int size);
void elfreloc(internstr sym, enum relockind, enum section, uint off, s64int addend);
void elffini(WriteBuf *);

ObjFile objout;

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
objdeffunc(internstr nam, bool globl, uint off, uint siz)
{
   switch (mctarg->objkind) {
   case OBJELF:
      elfaddsym(nam, /*STT_LOCAL/GLOBAL*/globl << 4 | /*STT_FUNC*/2, Stext, off, siz);
      break;
   }
}

enum section
objhassym(internstr name, uint *off)
{
   return elfhassym(name, off);
}

uint
objnewdat(internstr name, enum section sec, bool globl, uint siz, uint align)
{
   ObjFile *o = &objout;
   uint off;
   assert(align && ispo2(align));
   switch (sec) {
   default: assert(0);
   case Stext:
      assert(align <= targ_primsizes[TYPTR]);
      assert(o->textend - siz > o->code);
      while ((o->code - o->textbegin) & (align - 1)) ++o->code;
      off = o->code - o->textbegin;
      o->code += siz;
      break;
   case Srodata:
      if (align > o->rodataalign) o->rodataalign = align;
      while (o->rodata.n & (align - 1)) vpush(&o->rodata, 0);
      off = o->rodata.n;
      vresize(&o->rodata, o->rodata.n + siz);
      memset(o->rodata.p+off, 0, siz);
      break;
   case Sdata:
      if (align > o->dataalign) o->dataalign = align;
      while (o->data.n & (align - 1)) vpush(&o->data, 0);
      off = o->data.n;
      vresize(&o->data, o->data.n + siz);
      memset(o->data.p+off, 0, siz);
      break;
   case Sbss:
      if (align > o->bssalign) o->bssalign = align;
      off = alignup(o->nbss, align);
      o->nbss = off + siz;
      break;
   }

   switch (mctarg->objkind) {
   case OBJELF:
      elfaddsym(name, /*STT_LOCAL/GLOBAL*/globl<<4 | /*STT_OBJECT*/1, sec, off, siz);
      break;
   }
   return off;
}

static pmap_of(uchar) needed_fns;

void
objreloc(internstr sym, int symflags, enum relockind reloc, enum section section, uint off, s64int addend)
{
   if ((symflags & (SLOCAL|SFUNC)) == (SLOCAL|SFUNC))
      pmap_set(&needed_fns, sym, 1);
   switch (mctarg->objkind) {
   case OBJELF:
      elfreloc(sym, reloc, section, off, addend);
      break;
   }
}

bool
fnisneeded(internstr name)
{
   return pmap_get(&needed_fns, name) != NULL;
}

void
objfini(bool emit)
{
   emitxinlfns(/*all*/!emit);
   if (!emit) return;
   static char buf[1<<12];
   WriteBuf out = FDBUF(buf, sizeof buf, open(objout.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666));
   if (out.fd < 0) fatal(NULL, "could not open %'s for writing: %s", objout.outfile, strerror(errno));

   switch (mctarg->objkind) {
   case OBJELF: elffini(&out); break;
   }

   munmap(objout.textbegin, NTEXT);
   ioflush(&out);
   close(out.fd);
}

/* vim:set ts=3 sw=3 expandtab: */
