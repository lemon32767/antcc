#include "obj.h"
#include "ir.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>


void elfinit(void);
enum section elfhassym(internstr , uint *value);
void elfaddsym(internstr, enum symflags, enum section, u64int value, u64int size);
void elfreloc(internstr sym, enum symflags, enum relockind, enum section, uint off, s64int addend);
void elffini(WriteBuf *);
void elfmkmetaobj(WriteBuf *, const char *data, uint siz);

ObjFile objout = {
   .sec = {
      [Stext]   = { .x = 1, .pb = 1 },
      [Srodata] = { .pb = 1 },
      [Sdata]   = { .w = 1, .pb = 1 },
      [Sbss]    = { .w = 1 },
      [Stdata]  = { .w = 1, .pb = 1, .tls = 1 },
      [Stbss]   = { .w = 1, .tls = 1 },
   },
};

const char *objsecnames[] = {
   [Stext] = ".text", [Srodata] = ".rodata", [Sdata] = ".data", [Sbss] = ".bss",
   [Stdata] = ".tdata", [Stbss] = ".tbss",
};

void
objini(const char *infile, const char *outfile) {
   assert(!objout.outfile);
   objout.infile = infile;
   objout.outfile = outfile;
   for (enum section s = 1; s < NSECTION; ++s)
      objout.sec[s].name = intern(objsecnames[s]);

   switch (mctarg->objkind) {
   case OBJELF: elfinit(); break;
   }
}

void
objdeffunc(internstr nam, enum symflags s, uint off, uint siz) {
   switch (mctarg->objkind) {
   case OBJELF:
      elfaddsym(nam, s | SFUNC, Stext, off, siz);
      break;
   }
}

enum section
objhassym(internstr name, uint *off) {
   return elfhassym(name, off);
}

uint
objnewdat(internstr name, enum symflags sf, enum section s, uint siz, uint align) {
   uint off;
   assert(align && ispo2(align));
   ObjSection *sec = &objout.sec[s];

   if (align > sec->align) sec->align = align;
   if (sec->pb) {
      while (sec->n & (align - 1)) vpush(sec, 0);
      off = sec->n;
      if (siz) {
         vresize(sec, sec->n + siz);
         memset(sec->p + off, 0, siz);
      }
   } else {
      off = alignup(sec->n, align);
      sec->n = off + siz;
   }

   switch (mctarg->objkind) {
   case OBJELF:
      elfaddsym(name, sf, s, off, siz);
      break;
   }
   return off;
}

uchar *
objsecdata(enum section s, uint off) {
   ObjSection *sec = &objout.sec[s];
   assert(sec->pb && off <= sec->n);
   return sec->p + off;
}

uchar *
objtextinit(void) {
   enum { HEADROOM = 1<<20 }; /* 1 MiB; MAXINSTR bounds this in practice */
   ObjSection *txt = &objout.sec[Stext];
   uint off = txt->n;
   if ((u64int) off + HEADROOM > txt->_vb.cap)
      vresize(txt, off + HEADROOM);
   return txt->p + (txt->n = off);
}

void
objtextfini(uchar *pcode) {
   ObjSection *txt = &objout.sec[Stext];
   ptrdiff_t actual = pcode - txt->p;
   assert(actual <= txt->_vb.cap);
   txt->n = actual;
}

static pmap_of(uchar) needed_fns;

void
objreloc(internstr sym, enum symflags f, enum relockind reloc, enum section section, uint off, s64int addend) {
   if ((f & (SLOCAL|SFUNC)) == (SLOCAL|SFUNC))
      pmap_set(&needed_fns, sym, 1);
   switch (mctarg->objkind) {
   case OBJELF:
      elfreloc(sym, f, reloc, section, off, addend);
      break;
   }
}

void
markfnneeded(internstr sym) {
   pmap_set(&needed_fns, sym, 1);
}

bool
fnisneeded(internstr sym) {
   return pmap_get(&needed_fns, sym) != NULL;
}

void
objfini(bool emit) {
   emitxinlfns(/*all*/!emit);
   if (!emit) return;
   static char buf[1<<12];
   WriteBuf out = FDBUF(buf, sizeof buf, open(objout.outfile, O_WRONLY | O_CREAT | O_TRUNC, 0666));
   if (out.fd < 0) fatal(NULL, "could not open %'s for writing: %s", objout.outfile, strerror(errno));

   switch (mctarg->objkind) {
   case OBJELF: elffini(&out); break;
   }

   ioflush(&out);
   close(out.fd);
}

void
objmkmeta(WriteBuf *out, const char *data, uint siz) {
   switch (mctarg->objkind) {
   case OBJELF: elfmkmetaobj(out, data, siz);
   }
}

/* vim:set ts=3 sw=3 expandtab: */
